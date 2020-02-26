#ifndef _TLS_CLIENT_H
#define _TLS_CLIENT_H

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include "common.h"

typedef struct _tls_client
{
    /* mbedtls_ssl_free() */
    mbedtls_ssl_context ssl;

    /* mbedtls_net_free() */
    mbedtls_net_context net;
} tls_client_t;

int tls_client_connect(
    const char* server_name,
    const char* server_port,
    tls_client_t** client_out,
    tls_error_t* error);

int tls_client_read(
    tls_client_t* client,
    void* data,
    size_t size,
    tls_error_t* error);

int tls_client_write(
    tls_client_t* client,
    const void* data,
    size_t size,
    tls_error_t* error);

#endif /* _TLS_CLIENT_H */