#ifndef KMAT_KSP_RPC_H
#define KMAT_KSP_RPC_H

#include "krpc_types.h"
#include <stddef.h>
#include <stdint.h>
#include <krpc_cnano.h>

int ksp_rpc_connect(const char *host, uint16_t port, krpc_connection_t *connection, char *error, size_t error_size);
void ksp_rpc_disconnect(krpc_connection_t connection);
void ksp_rpc_error_text(krpc_error_t status, char *error, size_t error_size);

#endif
