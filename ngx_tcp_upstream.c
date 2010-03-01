
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_tcp.h>

static void ngx_tcp_upstream_cleanup(void *data);

static void ngx_tcp_upstream_connect(ngx_tcp_session_t *s, ngx_tcp_upstream_t *u);
static void ngx_tcp_upstream_resolve_handler(ngx_resolver_ctx_t *ctx);
/*static void ngx_tcp_upstream_check_broken_connection(ngx_tcp_session_t *s, ngx_event_t *ev);*/
static void ngx_tcp_upstream_finalize_session(ngx_tcp_session_t *s, ngx_tcp_upstream_t *u,
        ngx_int_t rc);

static char *ngx_tcp_upstream(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy);
static char *ngx_tcp_upstream_server(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);

static void *ngx_tcp_upstream_create_main_conf(ngx_conf_t *cf);
static char *ngx_tcp_upstream_init_main_conf(ngx_conf_t *cf, void *conf);

static ngx_command_t  ngx_tcp_upstream_commands[] = {

    { ngx_string("upstream"),
        NGX_TCP_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
        ngx_tcp_upstream,
        0,
        0,
        NULL },

    { ngx_string("server"),
        NGX_TCP_UPS_CONF|NGX_CONF_1MORE,
        ngx_tcp_upstream_server,
        NGX_TCP_SRV_CONF_OFFSET,
        0,
        NULL },

    ngx_null_command
};


static ngx_tcp_module_t  ngx_tcp_upstream_module_ctx = {
    NULL,

    ngx_tcp_upstream_create_main_conf,    /* create main configuration */
    ngx_tcp_upstream_init_main_conf,      /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */
};


ngx_module_t  ngx_tcp_upstream_module = {
    NGX_MODULE_V1,
    &ngx_tcp_upstream_module_ctx,         /* module context */
    ngx_tcp_upstream_commands,            /* module directives */
    NGX_TCP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


ngx_int_t
ngx_tcp_upstream_create(ngx_tcp_session_t *s) {
    ngx_tcp_upstream_t  *u;

    u = s->upstream;

    if (u && u->cleanup) {
        ngx_tcp_upstream_cleanup(s);
        *u->cleanup = NULL;
        u->cleanup = NULL;
    }

    u = ngx_pcalloc(s->pool, sizeof(ngx_tcp_upstream_t));
    if (u == NULL) {
        return NGX_ERROR;
    }

    s->upstream = u;

    u->peer.log = s->connection->log;
    u->peer.log_error = NGX_ERROR_ERR;

    return NGX_OK;
}


/*do something with the session*/
void
ngx_tcp_upstream_init(ngx_tcp_session_t *s) {

    ngx_str_t                      *host;
    ngx_uint_t                      i;
    ngx_connection_t               *c;
    ngx_tcp_cleanup_t             *cln;
    ngx_tcp_upstream_t            *u;
    ngx_tcp_core_srv_conf_t       *cscf;
    ngx_resolver_ctx_t             *ctx, temp;
    ngx_tcp_upstream_srv_conf_t   *uscf, **uscfp;
    ngx_tcp_upstream_main_conf_t  *umcf;

    c = s->connection;

    cscf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_core_module);

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, c->log, 0,
            "tcp init upstream, client timer: %d", c->read->timer_set);

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    u = s->upstream;

    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {

        if (!c->write->active) {
            if (ngx_add_event(c->write, NGX_WRITE_EVENT, NGX_CLEAR_EVENT)
                    == NGX_ERROR)
            {
                ngx_tcp_finalize_session(s);
                return;
            }
        }
    }

    cln = ngx_tcp_cleanup_add(s, 0);

    cln->handler = ngx_tcp_upstream_cleanup;
    cln->data = s;
    u->cleanup = &cln->handler;

    if (u->resolved == NULL) {

        uscf = u->conf->upstream;

    } else {

        /*TODO: support variable in the proxy_pass*/
        if (u->resolved->sockaddr) {

            if (ngx_tcp_upstream_create_round_robin_peer(s, u->resolved)
                    != NGX_OK)
            {
                ngx_tcp_finalize_session(s);
                return;
            }

            ngx_tcp_upstream_connect(s, u);

            return;
        }

        host = &u->resolved->host;

        umcf = ngx_tcp_get_module_main_conf(s, ngx_tcp_upstream_module);

        uscfp = umcf->upstreams.elts;

        for (i = 0; i < umcf->upstreams.nelts; i++) {

            uscf = uscfp[i];

            if (uscf->host.len == host->len
                    && ((uscf->port == 0 && u->resolved->no_port)
                        || uscf->port == u->resolved->port)
                    && ngx_memcmp(uscf->host.data, host->data, host->len) == 0)
            {
                goto found;
            }
        }

        temp.name = *host;

        ctx = ngx_resolve_start(cscf->resolver, &temp);
        if (ctx == NULL) {
            ngx_tcp_finalize_session(s);
            return;
        }

        if (ctx == NGX_NO_RESOLVER) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                    "no resolver defined to resolve %V", host);
            ngx_tcp_finalize_session(s);
            return;
        }

        ctx->name = *host;
        ctx->type = NGX_RESOLVE_A;
        ctx->handler = ngx_tcp_upstream_resolve_handler;
        ctx->data = s;
        ctx->timeout = cscf->resolver_timeout;

        u->resolved->ctx = ctx;

        if (ngx_resolve_name(ctx) != NGX_OK) {
            u->resolved->ctx = NULL;
            ngx_tcp_finalize_session(s);
            return;
        }

        return;
    }

found:

    if (uscf->peer.init(s, uscf) != NGX_OK) {
        ngx_tcp_finalize_session(s);
        return;
    }

    ngx_tcp_upstream_connect(s, u);
}

static void
ngx_tcp_upstream_resolve_handler(ngx_resolver_ctx_t *ctx) {

    ngx_tcp_session_t            *s;
    ngx_tcp_upstream_resolved_t  *ur;

    s = ctx->data;

    s->upstream->resolved->ctx = NULL;

    if (ctx->state) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                "%V could not be resolved (%i: %s)",
                &ctx->name, ctx->state,
                ngx_resolver_strerror(ctx->state));

        ngx_resolve_name_done(ctx);
        ngx_tcp_finalize_session(s);
        return;
    }

    ur = s->upstream->resolved;
    ur->naddrs = ctx->naddrs;
    ur->addrs = ctx->addrs;

#if (NGX_DEBUG)
    {
        in_addr_t   addr;
        ngx_uint_t  i;

        for (i = 0; i < ctx->naddrs; i++) {
            addr = ntohl(ur->addrs[i]);

            ngx_log_debug4(NGX_LOG_DEBUG_TCP, s->connection->log, 0,
                    "name was resolved to %ud.%ud.%ud.%ud",
                    (addr >> 24) & 0xff, (addr >> 16) & 0xff,
                    (addr >> 8) & 0xff, addr & 0xff);
        }
    }
#endif

    if (ngx_tcp_upstream_create_round_robin_peer(s, ur) != NGX_OK) {
        ngx_resolve_name_done(ctx);
        ngx_tcp_finalize_session(s);
        return;
    }

    ngx_resolve_name_done(ctx);

    ngx_tcp_upstream_connect(s, s->upstream);

    /*need add the event.*/
}


static void
ngx_tcp_upstream_handler(ngx_event_t *ev) {

    ngx_connection_t     *c;
    ngx_tcp_session_t   *s;
    ngx_tcp_upstream_t  *u;

    c = ev->data;
    s = c->data;

    u = s->upstream;
    c = s->connection;

    if (ev->write) {
        if (u->write_event_handler) {
            u->write_event_handler(s, u);
        }

    } else {

        if (u->read_event_handler) {
            u->read_event_handler(s, u);
        }
    }
}


/*static void*/
/*ngx_tcp_upstream_rd_check_broken_connection(ngx_tcp_session_t *s)*/
/*{*/
/*ngx_tcp_upstream_check_broken_connection(s, s->connection->read);*/
/*}*/


/*static void*/
/*ngx_tcp_upstream_wr_check_broken_connection(ngx_tcp_session_t *s)*/
/*{*/
/*ngx_tcp_upstream_check_broken_connection(s, s->connection->write);*/
/*}*/


/*static void*/
/*ngx_tcp_upstream_check_broken_connection(ngx_tcp_session_t *s,*/
/*ngx_event_t *ev)*/
/*{*/
/*int                  n;*/
/*char                 buf[1];*/
/*ngx_err_t            err;*/
/*ngx_int_t            event;*/
/*ngx_connection_t     *c;*/
/*ngx_tcp_upstream_t  *u;*/

/*ngx_log_debug1(NGX_LOG_DEBUG_TCP, ev->log, 0,*/
/*"tcp upstream check client, write event:%d", ev->write);*/

/*c = s->connection;*/
/*u = s->upstream;*/

/*if (c->error) {*/
/*if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && ev->active) {*/

/*event = ev->write ? NGX_WRITE_EVENT : NGX_READ_EVENT;*/

/*if (ngx_del_event(ev, event, 0) != NGX_OK) {*/
/*ngx_tcp_upstream_finalize_request(s, u, 0);*/
/*return;*/
/*}*/
/*}*/

/*if (!u->cacheable) {*/
/*ngx_tcp_upstream_finalize_request(s, u, 0);*/
/*}*/

/*return;*/
/*}*/

/*if (u->peer.connection == NULL) {*/
/*return;*/
/*}*/

/*#if (NGX_HAVE_KQUEUE)*/

/*if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {*/

/*if (!ev->pending_eof) {*/
/*return;*/
/*}*/

/*ev->eof = 1;*/
/*c->error = 1;*/

/*if (ev->kq_errno) {*/
/*ev->error = 1;*/
/*}*/

/*if (!u->cacheable && u->peer.connection) {*/
/*ngx_log_error(NGX_LOG_INFO, ev->log, ev->kq_errno,*/
/*"kevent() reported that client closed prematurely "*/
/*"connection, so upstream connection is closed too");*/
/*ngx_tcp_upstream_finalize_request(r, u,*/
/*NGX_TCP_CLIENT_CLOSED_REQUEST);*/
/*return;*/
/*}*/

/*ngx_log_error(NGX_LOG_INFO, ev->log, ev->kq_errno,*/
/*"kevent() reported that client closed "*/
/*"prematurely connection");*/

/*if (u->peer.connection == NULL) {*/
/*ngx_tcp_upstream_finalize_request(s, u,*/
/*NGX_TCP_CLIENT_CLOSED_REQUEST);*/
/*return;*/
/*}*/

/*return;*/
/*}*/

/*#endif*/

/*n = recv(c->fd, buf, 1, MSG_PEEK);*/

/*err = ngx_socket_errno;*/

/*ngx_log_debug1(NGX_LOG_DEBUG_TCP, ev->log, err,*/
/*"tcp upstream recv(): %d", n);*/

/*if (ev->write && (n >= 0 || err == NGX_EAGAIN)) {*/
/*return;*/
/*}*/

/*if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && ev->active) {*/

/*event = ev->write ? NGX_WRITE_EVENT : NGX_READ_EVENT;*/

/*if (ngx_del_event(ev, event, 0) != NGX_OK) {*/
/*ngx_tcp_upstream_finalize_request(s, u, 0);*/
/*return;*/
/*}*/
/*}*/

/*if (n > 0) {*/
/*return;*/
/*}*/

/*if (n == -1) {*/
/*if (err == NGX_EAGAIN) {*/
/*return;*/
/*}*/

/*ev->error = 1;*/

/*} else { *//* n == 0 */
/*err = 0;*/
/*}*/

/*ev->eof = 1;*/
/*c->error = 1;*/

/*if (!u->cacheable && u->peer.connection) {*/
/*ngx_log_error(NGX_LOG_INFO, ev->log, err,*/
/*"client closed prematurely connection, "*/
/*"so upstream connection is closed too");*/
/*ngx_tcp_upstream_finalize_request(s, u, 0);*/
/*return;*/
/*}*/

/*ngx_log_error(NGX_LOG_INFO, ev->log, err,*/
/*"client closed prematurely connection");*/

/*if (u->peer.connection == NULL) {*/
/*ngx_tcp_upstream_finalize_request(s, u, 0);*/
/*return;*/
/*}*/
/*}*/


static void
ngx_tcp_upstream_connect(ngx_tcp_session_t *s, ngx_tcp_upstream_t *u) {

    ngx_int_t                 rc;
    ngx_tcp_core_srv_conf_t  *cscf;
    ngx_connection_t         *c;

    s->connection->log->action = "connecting to upstream";

    cscf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_core_module);

    rc = ngx_event_connect_peer(&u->peer);

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, s->connection->log, 0, "tcp proxy connect peer: %d", rc);

    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
        ngx_tcp_upstream_finalize_session(s, u, 0);
        return;
    }

    /* rc == NGX_OK || rc == NGX_AGAIN */

    c = u->peer.connection;

    c->data = s;
    c->pool = s->connection->pool;
    c->log = s->connection->log;
    c->read->log = c->log;
    c->write->log = c->log;

    c->write->handler = ngx_tcp_upstream_handler;
    c->read->handler = ngx_tcp_upstream_handler;

    /*connect busy*/
    if (rc == NGX_AGAIN) {
        ngx_add_timer(c->write, u->conf->connect_timeout);
        return;
    }
    else {
        ngx_add_timer(c->read, u->conf->read_timeout);
        ngx_add_timer(c->write, u->conf->send_timeout);
    }
}


    static void
ngx_tcp_upstream_cleanup(void *data)
{
    ngx_tcp_session_t *s = data;

    ngx_tcp_upstream_t  *u;

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, s->connection->log, 0,
            "cleanup tcp upstream request: fd: %d", s->connection->fd);

    u = s->upstream;

    if (u->resolved && u->resolved->ctx) {
        ngx_resolve_name_done(u->resolved->ctx);
    }

    ngx_tcp_upstream_finalize_session(s, u, NGX_DONE);
}


    static void
ngx_tcp_upstream_finalize_session(ngx_tcp_session_t *s,
        ngx_tcp_upstream_t *u, ngx_int_t rc)
{
    ngx_time_t  *tp;

    ngx_log_debug1(NGX_LOG_DEBUG_TCP, s->connection->log, 0,
            "finalize tcp upstream request: %i", rc);

    if (u->cleanup) {
        *u->cleanup = NULL;
        u->cleanup = NULL;
    }

    if (u->state && u->state->response_sec) {
        tp = ngx_timeofday();
        u->state->response_sec = tp->sec - u->state->response_sec;
        u->state->response_msec = tp->msec - u->state->response_msec;

        if (u->pipe) {
            u->state->response_length = u->pipe->read_length;
        }
    }

    /*u->finalize_request(r, rc);*/

    if (u->peer.free) {
        u->peer.free(&u->peer, u->peer.data, 0);
    }

    if (u->peer.connection) {

        ngx_log_debug1(NGX_LOG_DEBUG_TCP, s->connection->log, 0,
                "close tcp upstream connection: %d",
                u->peer.connection->fd);

            ngx_close_connection(u->peer.connection);
    }

    u->peer.connection = NULL;

    if (u->pipe && u->pipe->temp_file) {
        ngx_log_debug1(NGX_LOG_DEBUG_TCP, s->connection->log, 0,
                "tcp upstream temp fd: %d",
                u->pipe->temp_file->file.fd);
    }

    /*if (u->header_sent*/
    /*&& (rc == NGX_ERROR))*/
    /*{*/
    /*rc = 0;*/
    /*}*/

    if (rc == NGX_DECLINED || rc == NGX_DONE) {
        return;
    }

    s->connection->log->action = "sending to client";

    ngx_tcp_finalize_session(s);
}

ngx_tcp_upstream_srv_conf_t *
ngx_tcp_upstream_add(ngx_conf_t *cf, ngx_url_t *u, ngx_uint_t flags) {

    ngx_uint_t                      i;
    ngx_tcp_upstream_server_t     *us;
    ngx_tcp_upstream_srv_conf_t   *uscf, **uscfp;
    ngx_tcp_upstream_main_conf_t  *umcf;

    if (!(flags & NGX_TCP_UPSTREAM_CREATE)) {

        if (ngx_parse_url(cf->pool, u) != NGX_OK) {
            if (u->err) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "%s in upstream \"%V\"", u->err, &u->url);
            }

            return NULL;
        }
    }

    umcf = ngx_tcp_conf_get_module_main_conf(cf, ngx_tcp_upstream_module);

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (uscfp[i]->host.len != u->host.len || 
                ngx_strncasecmp(uscfp[i]->host.data, u->host.data, u->host.len) != 0)
        {
            continue;
        }

        if ((flags & NGX_TCP_UPSTREAM_CREATE)
                && (uscfp[i]->flags & NGX_TCP_UPSTREAM_CREATE))
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "duplicate upstream \"%V\"", &u->host);
            return NULL;
        }

        if ((uscfp[i]->flags & NGX_TCP_UPSTREAM_CREATE) && u->port) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                    "upstream \"%V\" may not have port %d",
                    &u->host, u->port);
            return NULL;
        }

        if ((flags & NGX_TCP_UPSTREAM_CREATE) && uscfp[i]->port) {
            ngx_log_error(NGX_LOG_WARN, cf->log, 0,
                    "upstream \"%V\" may not have port %d in %s:%ui",
                    &u->host, uscfp[i]->port,
                    uscfp[i]->file_name, uscfp[i]->line);
            return NULL;
        }

        if (uscfp[i]->port != u->port) {
            continue;
        }

        if (uscfp[i]->default_port && u->default_port
                && uscfp[i]->default_port != u->default_port)
        {
            continue;
        }

        return uscfp[i];
    }

    uscf = ngx_pcalloc(cf->pool, sizeof(ngx_tcp_upstream_srv_conf_t));
    if (uscf == NULL) {
        return NULL;
    }

    uscf->flags = flags;
    uscf->host = u->host;
    uscf->file_name = cf->conf_file->file.name.data;
    uscf->line = cf->conf_file->line;
    uscf->port = u->port;
    uscf->default_port = u->default_port;

    if (u->naddrs == 1) {
        uscf->servers = ngx_array_create(cf->pool, 1,
                sizeof(ngx_tcp_upstream_server_t));
        if (uscf->servers == NULL) {
            return NGX_CONF_ERROR;
        }

        us = ngx_array_push(uscf->servers);
        if (us == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_memzero(us, sizeof(ngx_tcp_upstream_server_t));

        us->addrs = u->addrs;
        us->naddrs = u->naddrs;
    }

    uscfp = ngx_array_push(&umcf->upstreams);
    if (uscfp == NULL) {
        return NULL;
    }

    *uscfp = uscf;

    return uscf;
}


static char *
ngx_tcp_upstream(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy) {

    char                          *rv;
    void                          *mconf;
    ngx_str_t                     *value;
    ngx_url_t                      u;
    ngx_uint_t                     m;
    ngx_conf_t                     pcf;
    ngx_tcp_module_t             *module;
    ngx_tcp_conf_ctx_t           *ctx, *tcp_ctx;
    ngx_tcp_upstream_srv_conf_t  *uscf;

    ngx_memzero(&u, sizeof(ngx_url_t));

    value = cf->args->elts;
    u.host = value[1];
    u.no_resolve = 1;

    uscf = ngx_tcp_upstream_add(cf, &u, NGX_TCP_UPSTREAM_CREATE
            |NGX_TCP_UPSTREAM_WEIGHT
            |NGX_TCP_UPSTREAM_MAX_FAILS
            |NGX_TCP_UPSTREAM_FAIL_TIMEOUT
            |NGX_TCP_UPSTREAM_MAX_BUSY
            |NGX_TCP_UPSTREAM_DOWN
            |NGX_TCP_UPSTREAM_BACKUP);
    if (uscf == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_tcp_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    tcp_ctx = cf->ctx;
    ctx->main_conf = tcp_ctx->main_conf;

    /* the upstream{}'s srv_conf */

    ctx->srv_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_tcp_max_module);
    if (ctx->srv_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->srv_conf[ngx_tcp_upstream_module.ctx_index] = uscf;

    uscf->srv_conf = ctx->srv_conf;


    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_TCP_MODULE) {
            continue;
        }

        module = ngx_modules[m]->ctx;

        if (module->create_srv_conf) {
            mconf = module->create_srv_conf(cf);
            if (mconf == NULL) {
                return NGX_CONF_ERROR;
            }

            ctx->srv_conf[ngx_modules[m]->ctx_index] = mconf;
        }

    }


    /* parse inside upstream{} */

    pcf = *cf;
    cf->ctx = ctx;
    cf->cmd_type = NGX_TCP_UPS_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = pcf;

    if (rv != NGX_CONF_OK) {
        return rv;
    }

    if (uscf->servers == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "no servers are inside upstream");
        return NGX_CONF_ERROR;
    }

    return rv;
}


static char *
ngx_tcp_upstream_server(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {

    ngx_tcp_upstream_srv_conf_t  *uscf = conf;

    time_t                       fail_timeout;
    ngx_str_t                   *value, s;
    ngx_url_t                    u;
    ngx_int_t                    weight, max_fails, max_busy;
    ngx_uint_t                   i;
    ngx_tcp_upstream_server_t  *us;

    if (uscf->servers == NULL) {
        uscf->servers = ngx_array_create(cf->pool, 4,
                sizeof(ngx_tcp_upstream_server_t));
        if (uscf->servers == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    us = ngx_array_push(uscf->servers);
    if (us == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(us, sizeof(ngx_tcp_upstream_server_t));

    value = cf->args->elts;

    ngx_memzero(&u, sizeof(ngx_url_t));

    u.url = value[1];
    u.default_port = 80;

    if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "%s in upstream \"%V\"", u.err, &u.url);
        }

        return NGX_CONF_ERROR;
    }

    weight = 1;
    max_fails = 1;
    max_busy = 0;
    fail_timeout = 10;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "weight=", 7) == 0) {

            if (!(uscf->flags & NGX_TCP_UPSTREAM_WEIGHT)) {
                goto invalid;
            }

            weight = ngx_atoi(&value[i].data[7], value[i].len - 7);

            if (weight == NGX_ERROR || weight == 0) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "max_fails=", 10) == 0) {

            if (!(uscf->flags & NGX_TCP_UPSTREAM_MAX_FAILS)) {
                goto invalid;
            }

            max_fails = ngx_atoi(&value[i].data[10], value[i].len - 10);

            if (max_fails == NGX_ERROR) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "max_busy=", 9) == 0) {

            if (!(uscf->flags & NGX_TCP_UPSTREAM_MAX_BUSY)) {
                goto invalid;
            }

            max_busy = ngx_atoi(&value[i].data[9], value[i].len - 9);

            if (max_busy == NGX_ERROR) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "fail_timeout=", 13) == 0) {

            if (!(uscf->flags & NGX_TCP_UPSTREAM_FAIL_TIMEOUT)) {
                goto invalid;
            }

            s.len = value[i].len - 13;
            s.data = &value[i].data[13];

            fail_timeout = ngx_parse_time(&s, 1);

            if (fail_timeout == NGX_ERROR) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "backup", 6) == 0) {

            if (!(uscf->flags & NGX_TCP_UPSTREAM_BACKUP)) {
                goto invalid;
            }

            us->backup = 1;

            continue;
        }

        if (ngx_strncmp(value[i].data, "down", 4) == 0) {

            if (!(uscf->flags & NGX_TCP_UPSTREAM_DOWN)) {
                goto invalid;
            }

            us->down = 1;

            continue;
        }

        goto invalid;
    }

    us->addrs = u.addrs;
    us->naddrs = u.naddrs;
    us->weight = weight;
    us->max_fails = max_fails;
    us->max_busy = max_busy;
    us->fail_timeout = fail_timeout;

    return NGX_CONF_OK;

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}

static void *
ngx_tcp_upstream_create_main_conf(ngx_conf_t *cf) {

    ngx_tcp_upstream_main_conf_t  *umcf;

    umcf = ngx_pcalloc(cf->pool, sizeof(ngx_tcp_upstream_main_conf_t));
    if (umcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&umcf->upstreams, cf->pool, 4,
                sizeof(ngx_tcp_upstream_srv_conf_t *))
            != NGX_OK)
    {
        return NULL;
    }

    return umcf;
}


static char *
ngx_tcp_upstream_init_main_conf(ngx_conf_t *cf, void *conf) {

    ngx_tcp_upstream_main_conf_t  *umcf = conf;

    ngx_uint_t                      i;
    ngx_tcp_upstream_init_pt       init;
    ngx_tcp_upstream_srv_conf_t  **uscfp;

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        init = uscfp[i]->peer.init_upstream ? uscfp[i]->peer.init_upstream:
            ngx_tcp_upstream_init_round_robin;

        if (init(cf, uscfp[i]) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}
