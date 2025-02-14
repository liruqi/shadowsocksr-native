#include "defs.h"
#include "common.h"
#include "s5.h"
#include "obfs.h"
#include "ssrbuffer.h"
#include "dump_info.h"
#include "ssr_executive.h"
#include "encrypt.h"
#include "tunnel.h"
#include "obfsutil.h"
#include "tls_cli.h"

/* A connection is modeled as an abstraction on top of two simple state
 * machines, one for reading and one for writing.  Either state machine
 * is, when active, in one of three states: busy, done or stop; the fourth
 * and final state, dead, is an end state and only relevant when shutting
 * down the connection.  A short overview:
 *
 *                          busy                  done           stop
 *  ----------|---------------------------|--------------------|------|
 *  readable  | waiting for incoming data | have incoming data | idle |
 *  writable  | busy writing out data     | completed write    | idle |
 *
 * We could remove the done state from the writable state machine. For our
 * purposes, it's functionally equivalent to the stop state.
 *
 * When the connection with upstream has been established, the struct tunnel_ctx
 * moves into a state where incoming data from the client is sent upstream
 * and vice versa, incoming data from upstream is sent to the client.  In
 * other words, we're just piping data back and forth.  See do_proxy()
 * for details.
 *
 * An interesting deviation from libuv's I/O model is that reads are discrete
 * rather than continuous events.  In layman's terms, when a read operation
 * completes, the connection stops reading until further notice.
 *
 * The rationale for this approach is that we have to wait until the data
 * has been sent out again before we can reuse the read buffer.
 *
 * It also pleasingly unifies with the request model that libuv uses for
 * writes and everything else; libuv may switch to a request model for
 * reads in the future.
 */

 /* Session states. */
enum tunnel_stage {
    tunnel_stage_handshake,        /* Wait for client handshake. */
    tunnel_stage_handshake_auth,   /* Wait for client authentication data. */
    tunnel_stage_handshake_replied,        /* Start waiting for request data. */
    tunnel_stage_s5_request,        /* Wait for request data. */
    tunnel_stage_s5_udp_accoc,
    tunnel_stage_tls_connecting,
    tunnel_stage_tls_first_package,
    tunnel_stage_tls_streaming,
    tunnel_stage_resolve_ssr_server_host_done,       /* Wait for upstream hostname DNS lookup to complete. */
    tunnel_stage_connecting_ssr_server,      /* Wait for uv_tcp_connect() to complete. */
    tunnel_stage_ssr_auth_sent,
    tunnel_stage_ssr_waiting_feedback,
    tunnel_stage_ssr_receipt_of_feedback_sent,
    tunnel_stage_auth_complition_done,      /* Connected. Start piping data. */
    tunnel_stage_streaming,            /* Connected. Pipe data back and forth. */
    tunnel_stage_kill,             /* Tear down session. */
};

struct client_ctx {
    struct server_env_t *env; // __weak_ptr
    struct tunnel_cipher_ctx *cipher;
    struct buffer_t *init_pkg;
    s5_ctx *parser;  /* The SOCKS protocol parser. */
    enum tunnel_stage stage;
};

static struct buffer_t * initial_package_create(const s5_ctx *parser);
static void do_next(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void do_handshake(struct tunnel_ctx *tunnel);
static void do_handshake_auth(struct tunnel_ctx *tunnel);
static void do_wait_s5_request(struct tunnel_ctx *tunnel);
static void do_parse_s5_request(struct tunnel_ctx *tunnel);
static void do_resolve_ssr_server_host_aftercare(struct tunnel_ctx *tunnel);
static void do_connect_ssr_server(struct tunnel_ctx *tunnel);
static void do_connect_ssr_server_done(struct tunnel_ctx *tunnel);
static void do_ssr_auth_sent(struct tunnel_ctx *tunnel);
static bool do_ssr_receipt_for_feedback(struct tunnel_ctx *tunnel);
static void do_socks5_reply_success(struct tunnel_ctx *tunnel);
static void do_launch_streaming(struct tunnel_ctx *tunnel);
static uint8_t* tunnel_extract_data(struct socket_ctx *socket, void*(*allocator)(size_t size), size_t *size);
static void tunnel_dying(struct tunnel_ctx *tunnel);
static void tunnel_timeout_expire_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_outgoing_connected_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_read_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_getaddrinfo_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_write_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static size_t tunnel_get_alloc_size(struct tunnel_ctx *tunnel, struct socket_ctx *socket, size_t suggested_size);
static void tunnel_tls_do_launch_streaming(struct tunnel_ctx *tunnel);
static void tunnel_tls_client_incoming_streaming(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_tls_on_connection_established(struct tunnel_ctx *tunnel);
static void tunnel_tls_on_data_received(struct tunnel_ctx *tunnel, const uint8_t *data, size_t size);
static void tunnel_tls_on_shutting_down(struct tunnel_ctx *tunnel);

static bool can_auth_none(const uv_tcp_t *lx, const struct tunnel_ctx *cx);
static bool can_auth_passwd(const uv_tcp_t *lx, const struct tunnel_ctx *cx);
static bool can_access(const uv_tcp_t *lx, const struct tunnel_ctx *cx, const struct sockaddr *addr);

static bool init_done_cb(struct tunnel_ctx *tunnel, void *p) {
    struct server_env_t *env = (struct server_env_t *)p;

    struct client_ctx *ctx = (struct client_ctx *) calloc(1, sizeof(struct client_ctx));
    ctx->env = env;
    tunnel->data = ctx;

    tunnel->tunnel_dying = &tunnel_dying;
    tunnel->tunnel_timeout_expire_done = &tunnel_timeout_expire_done;
    tunnel->tunnel_outgoing_connected_done = &tunnel_outgoing_connected_done;
    tunnel->tunnel_read_done = &tunnel_read_done;
    tunnel->tunnel_getaddrinfo_done = &tunnel_getaddrinfo_done;
    tunnel->tunnel_write_done = &tunnel_write_done;
    tunnel->tunnel_get_alloc_size = &tunnel_get_alloc_size;
    tunnel->tunnel_extract_data = &tunnel_extract_data;
    tunnel->tunnel_tls_on_connection_established = &tunnel_tls_on_connection_established;
    tunnel->tunnel_tls_on_data_received = &tunnel_tls_on_data_received;
    tunnel->tunnel_tls_on_shutting_down = &tunnel_tls_on_shutting_down;

    cstl_set_container_add(ctx->env->tunnel_set, tunnel);

    ctx->parser = (s5_ctx *)calloc(1, sizeof(s5_ctx));
    s5_init(ctx->parser);
    ctx->cipher = NULL;
    ctx->stage = tunnel_stage_handshake;

    return true;
}

void client_tunnel_initialize(uv_tcp_t *lx, unsigned int idle_timeout) {
    uv_loop_t *loop = lx->loop;
    struct server_env_t *env = (struct server_env_t *)loop->data;

    tunnel_initialize(lx, idle_timeout, &init_done_cb, env);
}

static void _do_shutdown_tunnel(const void *obj, void *p) {
    tunnel_shutdown((struct tunnel_ctx *)obj);
    (void)p;
}

void client_shutdown(struct server_env_t *env) {
    cstl_set_container_traverse(env->tunnel_set, &_do_shutdown_tunnel, NULL);
}

static struct buffer_t * initial_package_create(const s5_ctx *parser) {
    struct buffer_t *buffer = buffer_create(SSR_BUFF_SIZE);

    uint8_t *iter = buffer->buffer;
    uint8_t len;
    iter[0] = (uint8_t)parser->atyp;
    iter++;

    switch (parser->atyp) {
    case s5_atyp_ipv4:  // IPv4
        memcpy(iter, parser->daddr, sizeof(struct in_addr));
        iter += sizeof(struct in_addr);
        break;
    case s5_atyp_ipv6:  // IPv6
        memcpy(iter, parser->daddr, sizeof(struct in6_addr));
        iter += sizeof(struct in6_addr);
        break;
    case s5_atyp_host:
        len = (uint8_t)strlen((char *)parser->daddr);
        iter[0] = len;
        iter++;
        memcpy(iter, parser->daddr, len);
        iter += len;
        break;
    default:
        ASSERT(0);
        break;
    }
    *((unsigned short *)iter) = htons(parser->dport);
    iter += sizeof(unsigned short);

    buffer->len = iter - buffer->buffer;

    return buffer;
}

/* This is the core state machine that drives the client <-> upstream proxy.
* We move through the initial handshake and authentication steps first and
* end up (if all goes well) in the proxy state where we're just proxying
* data between the client and upstream.
*/
static void do_next(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct server_env_t *env = ctx->env;
    struct server_config *config = env->config;
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;

    switch (ctx->stage) {
    case tunnel_stage_handshake:
        ASSERT(incoming->rdstate == socket_done);
        incoming->rdstate = socket_stop;
        do_handshake(tunnel);
        break;
    case tunnel_stage_handshake_auth:
        do_handshake_auth(tunnel);
        break;
    case tunnel_stage_handshake_replied:
        ASSERT(incoming->wrstate == socket_done);
        incoming->wrstate = socket_stop;
        do_wait_s5_request(tunnel);
        break;
    case tunnel_stage_s5_request:
        ASSERT(incoming->rdstate == socket_done);
        incoming->rdstate = socket_stop;
        do_parse_s5_request(tunnel);
        break;
    case tunnel_stage_s5_udp_accoc:
        ASSERT(incoming->wrstate == socket_done);
        incoming->wrstate = socket_stop;
        tunnel_shutdown(tunnel);
        break;
    case tunnel_stage_resolve_ssr_server_host_done:
        do_resolve_ssr_server_host_aftercare(tunnel);
        break;
    case tunnel_stage_connecting_ssr_server:
        do_connect_ssr_server_done(tunnel);
        break;
    case tunnel_stage_ssr_auth_sent:
        ASSERT(outgoing->wrstate == socket_done);
        outgoing->wrstate = socket_stop;
        do_ssr_auth_sent(tunnel);
        break;
    case tunnel_stage_ssr_waiting_feedback:
        ASSERT(outgoing->rdstate == socket_done);
        outgoing->rdstate = socket_stop;
        if (do_ssr_receipt_for_feedback(tunnel) == false) {
            do_socks5_reply_success(tunnel);
        }
        break;
    case tunnel_stage_ssr_receipt_of_feedback_sent:
        ASSERT(outgoing->wrstate == socket_done);
        outgoing->wrstate = socket_stop;
        do_socks5_reply_success(tunnel);
        break;
    case tunnel_stage_auth_complition_done:
        ASSERT(incoming->wrstate == socket_done);
        incoming->wrstate = socket_stop;
        if (config->over_tls_enable) {
            tunnel_tls_do_launch_streaming(tunnel);
        } else {
            do_launch_streaming(tunnel);
        }
        break;
    case tunnel_stage_tls_streaming:
        tunnel_tls_client_incoming_streaming(tunnel, socket);
        break;
    case tunnel_stage_streaming:
        tunnel_traditional_streaming(tunnel, socket);
        break;
    case tunnel_stage_kill:
        tunnel_shutdown(tunnel);
        break;
    default:
        UNREACHABLE();
    }
}

static void do_handshake(struct tunnel_ctx *tunnel) {
    enum s5_auth_method methods;
    struct socket_ctx *incoming = tunnel->incoming;
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    s5_ctx *parser = ctx->parser;
    uint8_t *data;
    size_t size;
    enum s5_err err;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);

    if (incoming->result < 0) {
        pr_err("read error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    data = (uint8_t *)incoming->buf->base;
    size = (size_t)incoming->result;
    err = s5_parse(parser, &data, &size);
    if (err == s5_ok) {
        socket_read(incoming, true);
        ctx->stage = tunnel_stage_handshake;  /* Need more data. */
        return;
    }

    if (size != 0) {
        /* Could allow a round-trip saving shortcut here if the requested auth
        * method is s5_auth_none (provided unauthenticated traffic is allowed.)
        * Requires client support however.
        */
        pr_err("junk in handshake");
        tunnel_shutdown(tunnel);
        return;
    }

    if (err != s5_auth_select) {
        pr_err("handshake error: %s", s5_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    methods = s5_auth_methods(parser);
    if ((methods & s5_auth_none) && can_auth_none(tunnel->listener, tunnel)) {
        s5_select_auth(parser, s5_auth_none);
        socket_write(incoming, "\5\0", 2);  /* No auth required. */
        ctx->stage = tunnel_stage_handshake_replied;
        return;
    }

    if ((methods & s5_auth_passwd) && can_auth_passwd(tunnel->listener, tunnel)) {
        /* TODO(bnoordhuis) Implement username/password auth. */
        tunnel_shutdown(tunnel);
        return;
    }

    socket_write(incoming, "\5\377", 2);  /* No acceptable auth. */
    ctx->stage = tunnel_stage_kill;
}

/* TODO(bnoordhuis) Implement username/password auth. */
static void do_handshake_auth(struct tunnel_ctx *tunnel) {
    UNREACHABLE();
    tunnel_shutdown(tunnel);
}

static void do_wait_s5_request(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct socket_ctx *incoming = tunnel->incoming;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);

    if (incoming->result < 0) {
        pr_err("write error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    socket_read(incoming, true);
    ctx->stage = tunnel_stage_s5_request;
}

static void do_parse_s5_request(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    s5_ctx *parser = ctx->parser;
    uint8_t *data;
    size_t size;
    enum s5_err err;
    struct server_env_t *env = ctx->env;
    struct server_config *config = env->config;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (incoming->result < 0) {
        pr_err("read error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    data = (uint8_t *)incoming->buf->base;
    size = (size_t)incoming->result;

    socks5_address_parse(data+3, size-3, tunnel->desired_addr);

    err = s5_parse(parser, &data, &size);
    if (err == s5_ok) {
        socket_read(incoming, true);
        ctx->stage = tunnel_stage_s5_request;  /* Need more data. */
        return;
    }

    if (size != 0) {
        pr_err("junk in request %u", (unsigned)size);
        tunnel_shutdown(tunnel);
        return;
    }

    if (err != s5_exec_cmd) {
        pr_err("request error: %s", s5_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    if (parser->cmd == s5_cmd_tcp_bind) {
        /* Not supported but relatively straightforward to implement. */
        pr_warn("BIND requests are not supported.");
        tunnel_shutdown(tunnel);
        return;
    }

    if (parser->cmd == s5_cmd_udp_assoc) {
        // UDP ASSOCIATE requests
        size_t len = incoming->buf->len;
        uint8_t *buf = build_udp_assoc_package(config->udp, config->listen_host, config->listen_port,
            (uint8_t *)incoming->buf->base, &len);
        socket_write(incoming, buf, len);
        ctx->stage = tunnel_stage_s5_udp_accoc;
        return;
    }

    ASSERT(parser->cmd == s5_cmd_tcp_connect);

    ctx->init_pkg = initial_package_create(parser);
    ctx->cipher = tunnel_cipher_create(ctx->env, 1452);

    {
        struct obfs_t *protocol = ctx->cipher->protocol;
        struct obfs_t *obfs = ctx->cipher->obfs;
        struct server_info_t *info;
        info = protocol ? protocol->get_server_info(protocol) : (obfs ? obfs->get_server_info(obfs) : NULL);
        if (info) {
            info->buffer_size = SSR_BUFF_SIZE;
            info->head_len = (int) get_s5_head_size(ctx->init_pkg->buffer, ctx->init_pkg->len, 30);
        }
    }

    if (config->over_tls_enable) {
        ctx->stage = tunnel_stage_tls_connecting;
        tls_client_launch(tunnel, config);
        return;
    } else
    {
        union sockaddr_universal remote_addr = { 0 };
        if (convert_universal_address(config->remote_host, config->remote_port, &remote_addr) != 0) {
            socket_getaddrinfo(outgoing, config->remote_host);
            ctx->stage = tunnel_stage_resolve_ssr_server_host_done;
            return;
        }

        outgoing->addr = remote_addr;

        do_connect_ssr_server(tunnel);
    }
}

static void do_resolve_ssr_server_host_aftercare(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct server_env_t *env = ctx->env;
    struct server_config *config = env->config;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (outgoing->result < 0) {
        /* TODO(bnoordhuis) Escape control characters in parser->daddr. */
        pr_err("lookup error for \"%s\": %s",
            config->remote_host,
            uv_strerror((int)outgoing->result));
        /* Send back a 'Host unreachable' reply. */
        socket_write(incoming, "\5\4\0\1\0\0\0\0\0\0", 10);
        ctx->stage = tunnel_stage_kill;
        return;
    }

    /* Don't make assumptions about the offset of sin_port/sin6_port. */
    switch (outgoing->addr.addr.sa_family) {
    case AF_INET:
        outgoing->addr.addr4.sin_port = htons(config->remote_port);
        break;
    case AF_INET6:
        outgoing->addr.addr6.sin6_port = htons(config->remote_port);
        break;
    default:
        UNREACHABLE();
    }

    do_connect_ssr_server(tunnel);
}

/* Assumes that cx->outgoing.t.sa contains a valid AF_INET/AF_INET6 address. */
static void do_connect_ssr_server(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct server_config *config = ctx->env->config;
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;
    int err;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (!can_access(tunnel->listener, tunnel, &outgoing->addr.addr)) {
        pr_warn("connection not allowed by ruleset");
        /* Send a 'Connection not allowed by ruleset' reply. */
        socket_write(incoming, "\5\2\0\1\0\0\0\0\0\0", 10);
        ctx->stage = tunnel_stage_kill;
        return;
    }

    err = socket_connect(outgoing);
    if (err != 0) {
        pr_err("connect error: %s", uv_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    ctx->stage = tunnel_stage_connecting_ssr_server;
}

static void do_connect_ssr_server_done(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (outgoing->result == 0) {
        struct buffer_t *tmp = buffer_clone(ctx->init_pkg);
        if (ssr_ok != tunnel_cipher_client_encrypt(ctx->cipher, tmp)) {
            buffer_release(tmp);
            tunnel_shutdown(tunnel);
            return;
        }
        socket_write(outgoing, tmp->buffer, tmp->len);
        buffer_release(tmp);

        ctx->stage = tunnel_stage_ssr_auth_sent;
        return;
    } else {
        socket_dump_error_info("upstream connection", outgoing);
        /* Send a 'Connection refused' reply. */
        socket_write(incoming, "\5\5\0\1\0\0\0\0\0\0", 10);
        ctx->stage = tunnel_stage_kill;
        return;
    }

    UNREACHABLE();
    tunnel_shutdown(tunnel);
}

static void do_ssr_auth_sent(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (outgoing->result < 0) {
        pr_err("write error: %s", uv_strerror((int)outgoing->result));
        tunnel_shutdown(tunnel);
        return;
    }

    if (tunnel_cipher_client_need_feedback(ctx->cipher)) {
        socket_read(outgoing, true);
        ctx->stage = tunnel_stage_ssr_waiting_feedback;
    } else {
        do_socks5_reply_success(tunnel);
    }
}

static bool do_ssr_receipt_for_feedback(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct tunnel_cipher_ctx *cipher_ctx = ctx->cipher;
    enum ssr_error error = ssr_error_client_decode;
    struct buffer_t *buf = NULL;
    struct buffer_t *feedback = NULL;
    bool done = false;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (outgoing->result < 0) {
        pr_err("read error: %s", uv_strerror((int)outgoing->result));
        tunnel_shutdown(tunnel);
        return done;
    }

    buf = buffer_create_from((uint8_t *)outgoing->buf->base, (size_t)outgoing->result);
    error = tunnel_cipher_client_decrypt(cipher_ctx, buf, &feedback);
    ASSERT(error == ssr_ok);
    ASSERT(buf->len == 0);

    if (feedback) {
        socket_write(outgoing, feedback->buffer, feedback->len);
        ctx->stage = tunnel_stage_ssr_receipt_of_feedback_sent;
        buffer_release(feedback);
        done = true;
    }

    buffer_release(buf);
    return done;
}

static void do_socks5_reply_success(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;
    uint8_t *buf;
    struct buffer_t *init_pkg = ctx->init_pkg;
    buf = (uint8_t *)calloc(3 + init_pkg->len, sizeof(uint8_t));

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    buf[0] = 5;  // Version.
    buf[1] = 0;  // Success.
    buf[2] = 0;  // Reserved.
    memcpy(buf + 3, init_pkg->buffer, init_pkg->len);
    socket_write(incoming, buf, 3 + init_pkg->len);
    free(buf);
    ctx->stage = tunnel_stage_auth_complition_done;
}

static void do_launch_streaming(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (incoming->result < 0) {
        pr_err("write error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    socket_read(incoming, false);
    socket_read(outgoing, true);
    ctx->stage = tunnel_stage_streaming;
}

static uint8_t* tunnel_extract_data(struct socket_ctx *socket, void*(*allocator)(size_t size), size_t *size) {
    struct tunnel_ctx *tunnel = socket->tunnel;
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct tunnel_cipher_ctx *cipher_ctx = ctx->cipher;
    enum ssr_error error = ssr_error_client_decode;
    struct buffer_t *buf = NULL;
    uint8_t *result = NULL;

    if (socket==NULL || allocator==NULL || size==NULL) {
        return result;
    }
    *size = 0;

    buf = buffer_create_from((uint8_t *)socket->buf->base, (size_t)socket->result);

    if (socket == tunnel->incoming) {
        error = tunnel_cipher_client_encrypt(cipher_ctx, buf);
    } else if (socket == tunnel->outgoing) {
        struct buffer_t *feedback = NULL;
        error = tunnel_cipher_client_decrypt(cipher_ctx, buf, &feedback);
        if (feedback) {
            ASSERT(false);
            buffer_release(feedback);
        }
    } else {
        ASSERT(false);
    }

    if (error == ssr_ok) {
        size_t len = buf->len;
        *size = len;
        result = (uint8_t *)allocator(len + 1);
        memcpy(result, buf->buffer, len);
    }

    buffer_release(buf);
    return result;
}

static void tunnel_dying(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    cstl_set_container_remove(ctx->env->tunnel_set, tunnel);
    if (ctx->cipher) {
        tunnel_cipher_release(ctx->cipher);
    }
    buffer_release(ctx->init_pkg);
    free(ctx->parser);
    free(ctx);
}

static void tunnel_timeout_expire_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    (void)tunnel;
    (void)socket;
}

static void tunnel_outgoing_connected_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

static void tunnel_read_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

static void tunnel_getaddrinfo_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

static void tunnel_write_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

static size_t tunnel_get_alloc_size(struct tunnel_ctx *tunnel, struct socket_ctx *socket, size_t suggested_size) {
    (void)tunnel;
    (void)socket;
    (void)suggested_size;
    return SSR_BUFF_SIZE;
}

static void tunnel_tls_do_launch_streaming(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (incoming->result < 0) {
        PRINT_ERR("write error: %s", uv_strerror((int)incoming->result));
        tls_client_shutdown(tunnel);
    } else {
        socket_read(incoming, true);
        ctx->stage = tunnel_stage_tls_streaming;
    }
}

void tunnel_tls_client_incoming_streaming(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    ASSERT(socket == tunnel->incoming);

    ASSERT((socket->wrstate == socket_done && socket->rdstate != socket_done) ||
        (socket->wrstate != socket_done && socket->rdstate == socket_done));

    if (socket->wrstate == socket_done) {
        socket->wrstate = socket_stop;
    }
    else if (socket->rdstate == socket_done) {
        socket->rdstate = socket_stop;
        {
            size_t len = 0;
            uint8_t *buf = NULL;
            ASSERT(tunnel->tunnel_extract_data);
            buf = tunnel->tunnel_extract_data(socket, &malloc, &len);
            if (buf /* && size > 0 */) {
                ASSERT(tunnel->tunnel_tls_send_data);
                tunnel->tunnel_tls_send_data(tunnel, buf, len);
            } else {
                tls_client_shutdown(tunnel);
            }
            free(buf);
        }
        socket_read(socket, false);
    }
    else {
        ASSERT(false);
    }
}

static void tunnel_tls_on_connection_established(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming = tunnel->incoming;
    struct socket_ctx *outgoing = tunnel->outgoing;
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    ASSERT (tunnel->tunnel_tls_send_data);
    {
        struct buffer_t *tmp = buffer_clone(ctx->init_pkg);
        if (ssr_ok != tunnel_cipher_client_encrypt(ctx->cipher, tmp)) {
            tls_client_shutdown(tunnel);
        } else {
            ctx->stage = tunnel_stage_tls_first_package;
            tunnel->tunnel_tls_send_data(tunnel, tmp->buffer, tmp->len);
        }
        buffer_release(tmp);
    }
}

static void tunnel_tls_on_data_received(struct tunnel_ctx *tunnel, const uint8_t *data, size_t size) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct socket_ctx *incoming = tunnel->incoming;
    if (ctx->stage == tunnel_stage_tls_first_package) {
        struct buffer_t *tmp = buffer_create_from(data, size);
        struct buffer_t *feedback = NULL;
        if (ssr_ok != tunnel_cipher_client_decrypt(ctx->cipher, tmp, &feedback)) {
            tls_client_shutdown(tunnel);
        } else {
            assert(!feedback);
            do_socks5_reply_success(tunnel);
        }
        buffer_release(tmp);
        return;
    } else {
        ASSERT(false);
    }
}

static void tunnel_tls_on_shutting_down(struct tunnel_ctx *tunnel) {
    tunnel_shutdown(tunnel);
}

static bool can_auth_none(const uv_tcp_t *lx, const struct tunnel_ctx *cx) {
    return true;
}

static bool can_auth_passwd(const uv_tcp_t *lx, const struct tunnel_ctx *cx) {
    return false;
}

static bool can_access(const uv_tcp_t *lx, const struct tunnel_ctx *cx, const struct sockaddr *addr) {
    const struct sockaddr_in6 *addr6;
    const struct sockaddr_in *addr4;
    const uint32_t *p;
    uint32_t a, b, c, d;

#if !defined(NDEBUG)
    return true;
#endif

    /* TODO(bnoordhuis) Implement proper access checks.  For now, just reject
    * traffic to localhost.
    */
    if (addr->sa_family == AF_INET) {
        addr4 = (const struct sockaddr_in *) addr;
        d = ntohl(addr4->sin_addr.s_addr);
        return (d >> 24) != 0x7F;
    }

    if (addr->sa_family == AF_INET6) {
        addr6 = (const struct sockaddr_in6 *) addr;
        p = (const uint32_t *)&addr6->sin6_addr.s6_addr;
        a = ntohl(p[0]);
        b = ntohl(p[1]);
        c = ntohl(p[2]);
        d = ntohl(p[3]);
        if (a == 0 && b == 0 && c == 0 && d == 1) {
            return false;  /* "::1" style address. */
        }
        if (a == 0 && b == 0 && c == 0xFFFF && (d >> 24) == 0x7F) {
            return false;  /* "::ffff:127.x.x.x" style address. */
        }
        return true;
    }

    return false;
}
