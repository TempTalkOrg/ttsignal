/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _XQC_CLIENT_H_INCLUDED_
#define _XQC_CLIENT_H_INCLUDED_

#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>

xqc_connection_t *jqc_client_connect(xqc_engine_t *engine,
    const xqc_conn_settings_t *conn_settings,
    const unsigned char *token,
    unsigned token_len,
    const char *server_host,
    int no_crypto_flag,
    const xqc_conn_ssl_config_t *conn_ssl_config,
    const char *alpn,
    const struct sockaddr *peer_addr,
    socklen_t peer_addrlen,
    jqc_timer_callback_t* timer_cbs,
    void *user_data);

xqc_connection_t *jqc_client_create_connection(xqc_engine_t *engine,
    xqc_cid_t dcid, xqc_cid_t scid,
    const xqc_conn_settings_t *settings,
    const char *server_host,
    int no_crypto_flag,
    const xqc_conn_ssl_config_t *conn_ssl_config,
    const char *alpn,
    jqc_timer_callback_t* timer_cbs,
    void *user_data);

#endif /* _XQC_CLIENT_H_INCLUDED_ */

