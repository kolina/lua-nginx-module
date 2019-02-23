
/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#if (NGX_HTTP_SSL)


#include "ngx_http_lua_cache.h"
#include "ngx_http_lua_initworkerby.h"
#include "ngx_http_lua_util.h"
#include "ngx_http_ssl_module.h"
#include "ngx_http_lua_contentby.h"
#include "ngx_http_lua_ssl_certby.h"


static void ngx_http_lua_ssl_cert_done(void *data);
static void ngx_http_lua_ssl_cert_aborted(void *data);
static u_char *ngx_http_lua_log_ssl_cert_error(ngx_log_t *log, u_char *buf,
    size_t len);
static ngx_int_t ngx_http_lua_ssl_cert_by_chunk(lua_State *L,
    ngx_http_request_t *r);


ngx_int_t
ngx_http_lua_ssl_cert_handler_file(ngx_http_request_t *r,
    ngx_http_lua_srv_conf_t *lscf, lua_State *L)
{
    ngx_int_t           rc;

    rc = ngx_http_lua_cache_loadfile(r->connection->log, L,
                                     lscf->ssl.cert_src.data,
                                     lscf->ssl.cert_src_key);
    if (rc != NGX_OK) {
        return rc;
    }

    /*  make sure we have a valid code chunk */
    ngx_http_lua_assert(lua_isfunction(L, -1));

    return ngx_http_lua_ssl_cert_by_chunk(L, r);
}


ngx_int_t
ngx_http_lua_ssl_cert_handler_inline(ngx_http_request_t *r,
    ngx_http_lua_srv_conf_t *lscf, lua_State *L)
{
    ngx_int_t           rc;

    rc = ngx_http_lua_cache_loadbuffer(r->connection->log, L,
                                       lscf->ssl.cert_src.data,
                                       lscf->ssl.cert_src.len,
                                       lscf->ssl.cert_src_key,
                                       "=ssl_certificate_by_lua");
    if (rc != NGX_OK) {
        return rc;
    }

    /*  make sure we have a valid code chunk */
    ngx_http_lua_assert(lua_isfunction(L, -1));

    return ngx_http_lua_ssl_cert_by_chunk(L, r);
}


char *
ngx_http_lua_ssl_cert_by_lua(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
#if OPENSSL_VERSION_NUMBER < 0x1000205fL

    ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                  "at least OpenSSL 1.0.2e required but found "
                  OPENSSL_VERSION_TEXT);

    return NGX_CONF_ERROR;

#else

    u_char                      *p;
    u_char                      *name;
    ngx_str_t                   *value;
    ngx_http_lua_srv_conf_t    *lscf = conf;

    /*  must specifiy a concrete handler */
    if (cmd->post == NULL) {
        return NGX_CONF_ERROR;
    }

    if (lscf->ssl.cert_handler) {
        return "is duplicate";
    }

    value = cf->args->elts;

    lscf->ssl.cert_handler = (ngx_http_lua_srv_conf_handler_pt) cmd->post;

    if (cmd->post == ngx_http_lua_ssl_cert_handler_file) {
        /* Lua code in an external file */

        name = ngx_http_lua_rebase_path(cf->pool, value[1].data,
                                        value[1].len);
        if (name == NULL) {
            return NGX_CONF_ERROR;
        }

        lscf->ssl.cert_src.data = name;
        lscf->ssl.cert_src.len = ngx_strlen(name);

        p = ngx_palloc(cf->pool, NGX_HTTP_LUA_FILE_KEY_LEN + 1);
        if (p == NULL) {
            return NGX_CONF_ERROR;
        }

        lscf->ssl.cert_src_key = p;

        p = ngx_copy(p, NGX_HTTP_LUA_FILE_TAG, NGX_HTTP_LUA_FILE_TAG_LEN);
        p = ngx_http_lua_digest_hex(p, value[1].data, value[1].len);
        *p = '\0';

    } else {
        /* inlined Lua code */

        lscf->ssl.cert_src = value[1];

        p = ngx_palloc(cf->pool, NGX_HTTP_LUA_INLINE_KEY_LEN + 1);
        if (p == NULL) {
            return NGX_CONF_ERROR;
        }

        lscf->ssl.cert_src_key = p;

        p = ngx_copy(p, NGX_HTTP_LUA_INLINE_TAG, NGX_HTTP_LUA_INLINE_TAG_LEN);
        p = ngx_http_lua_digest_hex(p, value[1].data, value[1].len);
        *p = '\0';
    }

    return NGX_CONF_OK;

#endif  /* OPENSSL_VERSION_NUMBER < 0x1000205fL */
}


int
ngx_http_lua_ssl_cert_handler(ngx_ssl_conn_t *ssl_conn, void *data)
{
    lua_State                       *L;
    ngx_int_t                        rc;
    ngx_connection_t                *c, *fc;
    ngx_http_request_t              *r = NULL;
    ngx_pool_cleanup_t              *cln;
    ngx_http_connection_t           *hc;
    ngx_http_lua_srv_conf_t         *lscf;
    ngx_http_lua_ssl_cert_ctx_t     *cctx;

    c = ngx_ssl_get_connection(ssl_conn);

    dd("c = %p", c);

    cctx = c->ssl->lua_ctx;

    dd("ssl cert handler, cert-ctx=%p", cctx);

    if (cctx) {
        /* not the first time */

        if (cctx->done) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "lua_certificate_by_lua: cert cb exit code: %d",
                           cctx->exit_code);

            dd("lua ssl cert done, finally");
            return cctx->exit_code;
        }

        return -1;
    }

    /* cctx == NULL */

    dd("first time");

    hc = c->data;

    fc = ngx_http_lua_create_fake_connection(NULL);
    if (fc == NULL) {
        goto failed;
    }

    fc->log->handler = ngx_http_lua_log_ssl_cert_error;
    fc->log->data = fc;

    fc->addr_text = c->addr_text;
    fc->listening = c->listening;

    r = ngx_http_lua_create_fake_request(fc);
    if (r == NULL) {
        goto failed;
    }

    r->main_conf = hc->conf_ctx->main_conf;
    r->srv_conf = hc->conf_ctx->srv_conf;
    r->loc_conf = hc->conf_ctx->loc_conf;

    fc->log->file = c->log->file;
    fc->log->log_level = c->log->log_level;
    fc->ssl = c->ssl;

    cctx = ngx_pcalloc(c->pool, sizeof(ngx_http_lua_ssl_cert_ctx_t));
    if (cctx == NULL) {
        goto failed;  /* error */
    }

    cctx->exit_code = 1;  /* successful by default */
    cctx->connection = c;
    cctx->request = r;

    dd("setting cctx");

    c->ssl->lua_ctx = cctx;

    lscf = ngx_http_get_module_srv_conf(r, ngx_http_lua_module);

    /* TODO honor lua_code_cache off */
    L = ngx_http_lua_get_lua_vm(r, NULL);

    c->log->action = "loading SSL certificate by lua";

    rc = lscf->ssl.cert_handler(r, lscf, L);

    if (rc >= NGX_OK || rc == NGX_ERROR) {
        cctx->done = 1;

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "lua_certificate_by_lua: handler return value: %i, "
                       "cert cb exit code: %d", rc, cctx->exit_code);

        c->log->action = "SSL handshaking";
        return cctx->exit_code;
    }

    /* rc == NGX_DONE */

    cln = ngx_pool_cleanup_add(fc->pool, 0);
    if (cln == NULL) {
        goto failed;
    }

    cln->handler = ngx_http_lua_ssl_cert_done;
    cln->data = cctx;

    cln = ngx_pool_cleanup_add(c->pool, 0);
    if (cln == NULL) {
        goto failed;
    }

    cln->handler = ngx_http_lua_ssl_cert_aborted;
    cln->data = cctx;

    return -1;

#if 1
failed:

    if (r && r->pool) {
        ngx_http_lua_free_fake_request(r);
    }

    if (fc) {
        ngx_http_lua_close_fake_connection(fc);
    }

    return 0;
#endif
}


static void
ngx_http_lua_ssl_cert_done(void *data)
{
    ngx_connection_t                *c;
    ngx_http_lua_ssl_cert_ctx_t     *cctx = data;

    dd("lua ssl cert done");

    if (cctx->aborted) {
        return;
    }

    ngx_http_lua_assert(cctx->done == 0);

    cctx->done = 1;

    c = cctx->connection;

    c->log->action = "SSL handshaking";

    ngx_post_event(c->write, &ngx_posted_events);
}


static void
ngx_http_lua_ssl_cert_aborted(void *data)
{
    ngx_http_lua_ssl_cert_ctx_t     *cctx = data;

    dd("lua ssl cert done");

    if (cctx->done) {
        /* completed successfully already */
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cctx->connection->log, 0,
                   "lua_certificate_by_lua: cert cb aborted");

    cctx->aborted = 1;
    cctx->request->connection->ssl = NULL;

    ngx_http_lua_finalize_fake_request(cctx->request, NGX_ERROR);
}


static u_char *
ngx_http_lua_log_ssl_cert_error(ngx_log_t *log, u_char *buf, size_t len)
{
    u_char              *p;
    ngx_connection_t    *c;

    if (log->action) {
        p = ngx_snprintf(buf, len, " while %s", log->action);
        len -= p - buf;
        buf = p;
    }

    p = ngx_snprintf(buf, len, ", context: ssl_certificate_by_lua*");
    len -= p - buf;
    buf = p;

    c = log->data;

    if (c->addr_text.len) {
        p = ngx_snprintf(buf, len, ", client: %V", &c->addr_text);
        len -= p - buf;
        buf = p;
    }

    if (c && c->listening && c->listening->addr_text.len) {
        p = ngx_snprintf(buf, len, ", server: %V", &c->listening->addr_text);
        /* len -= p - buf; */
        buf = p;
    }

    return buf;
}


static ngx_int_t
ngx_http_lua_ssl_cert_by_chunk(lua_State *L, ngx_http_request_t *r)
{
    int                      co_ref;
    ngx_int_t                rc;
    lua_State               *co;
    ngx_http_lua_ctx_t      *ctx;
    ngx_http_cleanup_t      *cln;

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);

    if (ctx == NULL) {
        ctx = ngx_http_lua_create_ctx(r);
        if (ctx == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

    } else {
        dd("reset ctx");
        ngx_http_lua_reset_ctx(r, L, ctx);
    }

    ctx->entered_content_phase = 1;

    /*  {{{ new coroutine to handle request */
    co = ngx_http_lua_new_thread(r, L, &co_ref);

    if (co == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "lua: failed to create new coroutine to handle request");

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /*  move code closure to new coroutine */
    lua_xmove(L, co, 1);

    /*  set closure's env table to new coroutine's globals table */
    ngx_http_lua_get_globals_table(co);
    lua_setfenv(co, -2);

    /* save nginx request in coroutine globals table */
    ngx_http_lua_set_req(co, r);

    ctx->cur_co_ctx = &ctx->entry_co_ctx;
    ctx->cur_co_ctx->co = co;
    ctx->cur_co_ctx->co_ref = co_ref;
#ifdef NGX_LUA_USE_ASSERT
    ctx->cur_co_ctx->co_top = 1;
#endif

    /* register request cleanup hooks */
    if (ctx->cleanup == NULL) {
        cln = ngx_http_cleanup_add(r, 0);
        if (cln == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        cln->handler = ngx_http_lua_request_cleanup_handler;
        cln->data = ctx;
        ctx->cleanup = &cln->handler;
    }

    ctx->context = NGX_HTTP_LUA_CONTEXT_SSL_CERT;

    rc = ngx_http_lua_run_thread(L, r, ctx, 0);

    if (rc == NGX_ERROR || rc >= NGX_OK) {
        /* do nothing */

    } else if (rc == NGX_AGAIN) {
        rc = ngx_http_lua_content_run_posted_threads(L, r, ctx, 0);

    } else if (rc == NGX_DONE) {
        rc = ngx_http_lua_content_run_posted_threads(L, r, ctx, 1);

    } else {
        rc = NGX_OK;
    }

    ngx_http_lua_finalize_request(r, rc);
    return rc;
}


#ifndef NGX_LUA_NO_FFI_API

int
ngx_http_lua_ffi_ssl_get_tls1_version(ngx_http_request_t *r, char **err)
{
#ifndef TLS1_get_version

    *err = "no TLS1 support";
    return NGX_ERROR;

#else

    ngx_ssl_conn_t    *ssl_conn;

    if (r->connection == NULL || r->connection->ssl == NULL) {
        *err = "bad request";
        return NGX_ERROR;
    }

    ssl_conn = r->connection->ssl->connection;
    if (ssl_conn == NULL) {
        *err = "bad ssl conn";
        return NGX_ERROR;
    }

    dd("tls1 ver: %d", (int) TLS1_get_version(ssl_conn));

    return (int) TLS1_get_version(ssl_conn);

#endif
}


int
ngx_http_lua_ffi_ssl_clear_certs(ngx_http_request_t *r, char **err)
{
#if OPENSSL_VERSION_NUMBER < 0x1000205fL

    *err = "at least OpenSSL 1.0.2e required but found " OPENSSL_VERSION_TEXT;
    return NGX_ERROR;

#else

    ngx_ssl_conn_t    *ssl_conn;

    if (r->connection == NULL || r->connection->ssl == NULL) {
        *err = "bad request";
        return NGX_ERROR;
    }

    ssl_conn = r->connection->ssl->connection;
    if (ssl_conn == NULL) {
        *err = "bad ssl conn";
        return NGX_ERROR;
    }

    SSL_certs_clear(ssl_conn);
    return NGX_OK;

#endif  /* OPENSSL_VERSION_NUMBER < 0x1000205fL */
}


int
ngx_http_lua_ffi_ssl_set_der_certificate(ngx_http_request_t *r,
    const char *data, size_t len, char **err)
{
#if OPENSSL_VERSION_NUMBER < 0x1000205fL

    *err = "at least OpenSSL 1.0.2e required but found " OPENSSL_VERSION_TEXT;
    return NGX_ERROR;

#else

    BIO               *bio = NULL;
    X509              *x509 = NULL;
    ngx_ssl_conn_t    *ssl_conn;

    if (r->connection == NULL || r->connection->ssl == NULL) {
        *err = "bad request";
        return NGX_ERROR;
    }

    ssl_conn = r->connection->ssl->connection;
    if (ssl_conn == NULL) {
        *err = "bad ssl conn";
        return NGX_ERROR;
    }

    bio = BIO_new_mem_buf((char *) data, len);
    if (bio == NULL) {
        *err = " BIO_new_mem_buf() failed";
        goto failed;
    }

    x509 = d2i_X509_bio(bio, NULL);
    if (x509 == NULL) {
        *err = " d2i_X509_bio() failed";
        goto failed;
    }

    if (SSL_use_certificate(ssl_conn, x509) == 0) {
        *err = " SSL_use_certificate() failed";
        goto failed;
    }

#if 0
    if (SSL_set_ex_data(ssl_conn, ngx_ssl_certificate_index, x509) == 0) {
        *err = " SSL_set_ex_data() failed";
        goto failed;
    }
#endif

    X509_free(x509);
    x509 = NULL;

    /* read rest of the chain */

    while (!BIO_eof(bio)) {

        x509 = d2i_X509_bio(bio, NULL);
        if (x509 == NULL) {
            *err = "d2i_X509_bio() failed";
            goto failed;
        }

        if (SSL_add0_chain_cert(ssl_conn, x509) == 0) {
            *err = "SSL_add0_chain_cert() failed";
            goto failed;
        }
    }

    BIO_free(bio);

    *err = NULL;
    return NGX_OK;

failed:

    if (bio) {
        BIO_free(bio);
    }

    if (x509) {
        X509_free(x509);
    }

    return NGX_ERROR;

#endif  /* OPENSSL_VERSION_NUMBER < 0x1000205fL */
}


int
ngx_http_lua_ffi_ssl_set_der_private_key(ngx_http_request_t *r,
    const char *data, size_t len, char **err)
{
    BIO               *bio = NULL;
    EVP_PKEY          *pkey = NULL;
    ngx_ssl_conn_t    *ssl_conn;

    if (r->connection == NULL || r->connection->ssl == NULL) {
        *err = "bad request";
        return NGX_ERROR;
    }

    ssl_conn = r->connection->ssl->connection;
    if (ssl_conn == NULL) {
        *err = "bad ssl conn";
        return NGX_ERROR;
    }

    bio = BIO_new_mem_buf((char *) data, len);
    if (bio == NULL) {
        *err = "BIO_new_mem_buf() failed";
        goto failed;
    }

    pkey = d2i_PrivateKey_bio(bio, NULL);
    if (pkey == NULL) {
        *err = "d2i_PrivateKey_bio() failed";
        goto failed;
    }

    if (SSL_use_PrivateKey(ssl_conn, pkey) == 0) {
        *err = "SSL_CTX_use_PrivateKey() failed";
        goto failed;
    }

    EVP_PKEY_free(pkey);
    BIO_free(bio);

    return NGX_OK;

failed:

    if (pkey) {
        EVP_PKEY_free(pkey);
    }

    if (bio) {
        BIO_free(bio);
    }

    return NGX_ERROR;
}


int
ngx_http_lua_ffi_ssl_raw_server_addr(ngx_http_request_t *r, char **addr,
    size_t *addrlen, int *addrtype, char **err)
{
#if (NGX_HAVE_UNIX_DOMAIN)
    struct sockaddr_un   *saun;
#endif
    ngx_ssl_conn_t       *ssl_conn;
    ngx_connection_t     *c;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    if (r->connection == NULL || r->connection->ssl == NULL) {
        *err = "bad request";
        return NGX_ERROR;
    }

    ssl_conn = r->connection->ssl->connection;
    if (ssl_conn == NULL) {
        *err = "bad ssl conn";
        return NGX_ERROR;
    }

    c = ngx_ssl_get_connection(ssl_conn);

    if (ngx_connection_local_sockaddr(c, NULL, 0) != NGX_OK) {
        return 0;
    }

    switch (c->local_sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:
        sin6 = (struct sockaddr_in6 *) c->local_sockaddr;
        *addrlen = 16;
        *addr = (char *) &sin6->sin6_addr.s6_addr;
        *addrtype = AF_INET6;

        break;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
    case AF_UNIX:
        saun = (struct sockaddr_un *) c->local_sockaddr;

        /* on Linux sockaddr might not include sun_path at all */
        if (c->local_socklen <= (socklen_t)
            offsetof(struct sockaddr_un, sun_path))
        {
            *addr = "";
            *addrlen = 0;

        } else {
            *addr = saun->sun_path;
            *addrlen = ngx_strlen(saun->sun_path);
        }

        *addrtype = AF_UNIX;
        break;
#endif

    default: /* AF_INET */
        sin = (struct sockaddr_in *) c->local_sockaddr;
        *addr = (char *) &sin->sin_addr.s_addr;
        *addrlen = 4;
        *addrtype = AF_INET;
        break;
    }

    return NGX_OK;
}


int
ngx_http_lua_ffi_ssl_server_name(ngx_http_request_t *r, char **name,
    size_t *namelen, char **err)
{
    ngx_ssl_conn_t          *ssl_conn;

    if (r->connection == NULL || r->connection->ssl == NULL) {
        *err = "bad request";
        return NGX_ERROR;
    }

    ssl_conn = r->connection->ssl->connection;
    if (ssl_conn == NULL) {
        *err = "bad ssl conn";
        return NGX_ERROR;
    }

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME

    *name = (char *) SSL_get_servername(ssl_conn, TLSEXT_NAMETYPE_host_name);

    if (*name) {
        *namelen = ngx_strlen(*name);
        return NGX_OK;
    }

    return NGX_DECLINED;

#else

    *err = "no TLS extension support";
    return NGX_ERROR;

#endif
}


int
ngx_http_lua_ffi_cert_pem_to_der(const u_char *pem, size_t pem_len, u_char *der,
    char **err)
{
    int       total, len;
    BIO      *bio;
    X509     *x509;
    u_long    n;

    bio = BIO_new_mem_buf((char *) pem, (int) pem_len);
    if (bio == NULL) {
        *err = "BIO_new_mem_buf() failed";
        return NGX_ERROR;
    }

    x509 = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
    if (x509 == NULL) {
        *err = "PEM_read_bio_X509_AUX() failed";
        return NGX_ERROR;
    }

    total = i2d_X509(x509, &der);
    if (total < 0) {
        X509_free(x509);
        BIO_free(bio);
        return NGX_ERROR;
    }

    X509_free(x509);

    /* read rest of the chain */

    for ( ;; ) {

        x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        if (x509 == NULL) {
            n = ERR_peek_last_error();

            if (ERR_GET_LIB(n) == ERR_LIB_PEM
                && ERR_GET_REASON(n) == PEM_R_NO_START_LINE)
            {
                /* end of file */
                ERR_clear_error();
                break;
            }

            /* some real error */

            *err = "PEM_read_bio_X509() failed";
            BIO_free(bio);
            return NGX_ERROR;
        }

        len = i2d_X509(x509, &der);
        if (len < 0) {
            *err = "i2d_X509() failed";
            X509_free(x509);
            BIO_free(bio);
            return NGX_ERROR;
        }

        total += len;

        X509_free(x509);
    }

    BIO_free(bio);

    return total;
}

int
ngx_http_lua_ffi_ssl_rsa_generate_key(
    int bits, unsigned char *out, size_t *out_size, char **err)
{
    int             rc = NGX_ERROR;
    size_t          len = 0;
    BIGNUM          *bne = NULL;
    RSA             *key = NULL;
    EVP_PKEY        *pkey = NULL;

    unsigned long   e = RSA_F4;

    bne = BN_new();
    if (!BN_set_word(bne,e)) {
        *err = "BN_set_word() failed";
        goto failed;
    }

    key = RSA_new();
    if (!RSA_generate_key_ex(key, bits, bne, NULL)) {
        *err = "RSA_generate_key_ex() failed";
        goto failed;
    }

    pkey = EVP_PKEY_new();
    if (!EVP_PKEY_assign_RSA(pkey, key)) {
        *err = "EVP_PKEY_assign_RSA() failed";
        goto failed;
    }

    key = NULL;

    len = i2d_PrivateKey(pkey, NULL);
    if (len <= 0) {
        *err = "i2d_PrivateKey() failed";
        goto failed;
    }

    if (len > *out_size) {
        *err = "output buffer too small";
        *out_size = len;
        rc = NGX_BUSY;
        goto failed;
    }

    len = i2d_PrivateKey(pkey, &out);
    if (len <= 0) {
        *err = "i2d_PrivateKey() failed";
        goto failed;
    }

    *out_size = len;

    // key will be freed when the parent pKey is freed
    EVP_PKEY_free(pkey);
    BN_free(bne);

    return NGX_OK;

failed:

    if (pkey) {
        EVP_PKEY_free(pkey);
    }

    if (key) {
        RSA_free(key);
    }

    if (bne) {
        BN_free(bne);
    }

    return rc;
}

int
ngx_http_lua_ffi_ssl_generate_certificate_sign_request(
    const char *data, size_t data_len, csr_info_t *info, unsigned char *out, size_t *out_size, char **err)
{
        int             rc = NGX_ERROR;
        int             ret = 0;
        BIO             *bio = NULL;
        EVP_PKEY        *pkey = NULL;
        size_t          len = 0;

        X509_REQ        *x509_req = NULL;
        X509_NAME       *x509_name = NULL;

        int             nVersion = 1;

        char*           subject_alt_name = NULL;
        STACK_OF(X509_EXTENSION) *exts = NULL;
        X509_EXTENSION  *ex;

        bio = BIO_new_mem_buf((char *) data, data_len);
        if (bio == NULL) {
            *err = "BIO_new_mem_buf() failed";
            goto failed;
        }

        pkey = d2i_PrivateKey_bio(bio, NULL);
        if (pkey == NULL) {
            *err = "d2i_PrivateKey_bio() failed";
            goto failed;
        }

        x509_req = X509_REQ_new();
        if (X509_REQ_set_version(x509_req, nVersion) != 1) {
            *err = "X509_REQ_set_version() failed";
            goto failed;
        }

        x509_name = X509_REQ_get_subject_name(x509_req);

        if (X509_NAME_add_entry_by_txt(x509_name,"C", MBSTRING_ASC, info->country, -1, -1, 0) != 1) {
            *err = "X509_NAME_add_entry_by_txt() for country failed";
            goto failed;
        }

        if (X509_NAME_add_entry_by_txt(x509_name,"ST", MBSTRING_ASC, info->state, -1, -1, 0) != 1) {
            *err = "X509_NAME_add_entry_by_txt() for state failed";
            goto failed;
        }

        if (X509_NAME_add_entry_by_txt(x509_name,"L", MBSTRING_ASC, info->city, -1, -1, 0) != 1) {
            *err = "X509_NAME_add_entry_by_txt() for city failed";
            goto failed;
        }

        if (X509_NAME_add_entry_by_txt(x509_name,"O", MBSTRING_ASC, info->organisation, -1, -1, 0) != 1) {
            *err = "X509_NAME_add_entry_by_txt() for organisation failed";
            goto failed;
        }

        if (X509_NAME_add_entry_by_txt(x509_name,"CN", MBSTRING_ASC, info->common_name, -1, -1, 0) != 1) {
            *err = "X509_NAME_add_entry_by_txt() for common name failed";
            goto failed;
        }

        subject_alt_name = malloc(strlen(info->common_name) + 15);
        if (subject_alt_name == NULL) {
            *err = "allocation for subject_alt_name failed";
            goto failed;
        }
        strcpy(subject_alt_name, "DNS:");
        strcat(subject_alt_name, info->common_name);

        exts = sk_X509_EXTENSION_new_null();
        ex = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, subject_alt_name);
        if (!ex) {
            *err = "X509V3_EXT_conf_nid() for common name failed";
            goto failed;
        }
        sk_X509_EXTENSION_push(exts, ex);
        X509_REQ_add_extensions(x509_req, exts);

        if (X509_REQ_set_pubkey(x509_req, pkey) != 1) {
            *err = "X509_REQ_set_pubkey() failed";
            goto failed;
        }

        ret = X509_REQ_sign(x509_req, pkey, EVP_sha256());
        if (ret <= 0) {
            *err = "X509_REQ_sign() failed";
            goto failed;
        }

        len = i2d_X509_REQ(x509_req, NULL);
        if (len <= 0) {
            *err = "i2d_X509_REQ() failed";
            goto failed;
        }

        if (len > *out_size) {
            *err = "output buffer too small";
            *out_size = len;
            rc = NGX_BUSY;
            goto failed;
        }


        len = i2d_X509_REQ(x509_req, &out);
        if (len <= 0) {
            *err = "i2d_X509_REQ() failed";
            goto failed;
        }

        *out_size = len;

        sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
        free(subject_alt_name);
        X509_REQ_free(x509_req);
        EVP_PKEY_free(pkey);
        BIO_free(bio);

        return NGX_OK;

failed:

        if (pkey) {
                EVP_PKEY_free(pkey);
        }

        if (bio) {
                BIO_free(bio);
        }

        if (x509_req) {
                X509_REQ_free(x509_req);
        }

        if (subject_alt_name) {
                free(subject_alt_name);
        }

        if (exts) {
                sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
        }

        return rc;
}

int loadX509(const char *ca, size_t len, X509 **ppx509)
{
        int     ret = 0;
        BIO     *in = NULL;

        in = BIO_new_mem_buf((char *) ca, len);
        if (in == NULL)
            return 0;

        ret = (PEM_read_bio_X509(in, ppx509, NULL, NULL) != NULL);
        BIO_free(in);

        return ret;
}

int loadX509Req(const char *req, size_t len, X509_REQ **ppReq)
{
        int     ret = 0;
        BIO     *in = NULL;

        in = BIO_new_mem_buf((char *) req, len);
        if (in == NULL)
            return 0;

        ret = (d2i_X509_REQ_bio(in, ppReq) != NULL);

        BIO_free(in);
        return ret;
}

int loadRSAPrivateKey(const char *ca, size_t len, EVP_PKEY **ppkey)
{
        BIO         *in = NULL;
        RSA         *key = NULL;
        EVP_PKEY    *pkey = NULL;

        in = BIO_new_mem_buf((char *) ca, len);
        if (in == NULL)
            return 0;

        if (PEM_read_bio_RSAPrivateKey(in, &key, NULL, NULL) == NULL)
            goto failed;

        pkey = EVP_PKEY_new();
        if (EVP_PKEY_assign_RSA(pkey, key) == 0)
            goto failed;

        *ppkey = pkey;

        key = NULL;

        BIO_free(in);

        return 1;

failed:
        if (in)
            BIO_free(in);

        if (key)
            RSA_free(key);

        if (pkey)
            EVP_PKEY_free(pkey);

        return 0;
}

int do_X509_sign(X509 *cert, EVP_PKEY *pkey, const EVP_MD *md)
{
        int ret;
        EVP_MD_CTX mctx;
        EVP_PKEY_CTX *pkctx = NULL;

        EVP_MD_CTX_init(&mctx);
        ret = EVP_DigestSignInit(&mctx, &pkctx, md, NULL, pkey);

        if (ret > 0)
            ret = X509_sign_ctx(cert, &mctx);

        EVP_MD_CTX_cleanup(&mctx);
        return ret > 0 ? 1 : 0;
}


int
ngx_http_lua_ffi_ssl_sign_certificate_sign_request(
        const char *cadata, size_t calen, const char *csr, size_t csrlen, unsigned char *out, size_t *out_size, char **err)
{
        int         rc = NGX_ERROR;
        int         serial = 1;
        long        days = 3650 * 24 * 3600; // 10 years
        size_t      len = 0;

        X509        *ca = NULL;
        X509        *cert = NULL;
        X509_REQ    *req = NULL;
        EVP_PKEY    *pkey = NULL;
        X509_NAME   *subject_name = NULL;

        int         subjAltName_pos;
        X509_EXTENSION *subjAltName;
        STACK_OF (X509_EXTENSION) * req_exts;

        if (!loadX509(cadata, calen, &ca)) {
            *err = "loadX509() failed";
            goto failed;
        }

        if (!loadRSAPrivateKey(cadata, calen, &pkey)) {
            *err = "loadRSAPrivateKey() failed";
            goto failed;
        }

        if (!loadX509Req(csr, csrlen, &req)) {
            *err = "loadX509Req() failed";
            goto failed;
        }

        cert = X509_new();

        if (!X509_set_version(cert, 2)) {
            *err = "X509_set_version() failed";
            goto failed;
        }

        ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);

        X509_gmtime_adj(X509_get_notBefore(cert), 0);
        X509_gmtime_adj(X509_get_notAfter(cert), days);

        subject_name = X509_REQ_get_subject_name(req);
        if (X509_set_subject_name(cert, X509_NAME_dup(subject_name)) == 0) {
            *err = "X509_set_subject_name() failed";
            goto failed;
        }

        subject_name = X509_get_subject_name(ca);
        if (X509_set_issuer_name(cert, X509_NAME_dup(subject_name)) == 0) {
            *err = "X509_set_issuer_name() failed";
            goto failed;
        }

        if (!(req_exts = X509_REQ_get_extensions (req))) {
            *err = "X509_REQ_get_extensions() failed";
            goto failed;
        }
        subjAltName_pos = X509v3_get_ext_by_NID (req_exts, OBJ_sn2nid ("subjectAltName"), -1);
        subjAltName = X509v3_get_ext (req_exts, subjAltName_pos);

        if (X509_set_pubkey(cert, X509_REQ_get_pubkey(req)) == 0) {
            *err = "X509_set_pubkey() failed";
            goto failed;
        }

        if (!X509_add_ext (cert, subjAltName, -1)) {
            *err = "X509_add_ext() failed";
            goto failed;
        }

        if (X509_sign(cert, pkey, EVP_sha256()) == 0) {
            *err = "X509_sign() failed";
            goto failed;
        }

        len = i2d_X509(cert, NULL);
        if (len <= 0) {
            *err = "i2d_X509() failed";
            goto failed;
        }

        if (len > *out_size) {
            *err = "output buffer too small";
            *out_size = len;
            rc = NGX_BUSY;
            goto failed;
        }


        len = i2d_X509(cert, &out);
        if (len <= 0) {
            *err = "i2d_X509_REQ() failed";
            goto failed;
        }

        *out_size = len;

        X509_free(cert);
        X509_free(ca);
        X509_REQ_free(req);
        EVP_PKEY_free(pkey);

    return NGX_OK;

failed:
    if (cert) {
        X509_free(cert);
    }

    if (ca) {
        X509_free(ca);
    }

    if (req) {
        X509_REQ_free(req);
    }

    if (pkey) {
        EVP_PKEY_free(pkey);
    }

    return rc;
}

#endif  /* NGX_LUA_NO_FFI_API */


#endif /* NGX_HTTP_SSL */