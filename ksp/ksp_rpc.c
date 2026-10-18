#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#endif
#include "ksp_rpc.h"
#include <stdio.h>
#include <string.h>

void ksp_rpc_error_text(krpc_error_t status, char *error, size_t error_size) {
	if(error == NULL || error_size == 0) return;
#ifdef KRPC_ERROR_MESSAGES
	const char *message = krpc_get_error_message();
	if(message != NULL && message[0] != 0) {
		snprintf(error, error_size, "%s: %s", krpc_get_error(status), message);
		return;
	}
#endif
	snprintf(error, error_size, "%s", krpc_get_error(status));
}

int ksp_rpc_connect(const char *host, uint16_t port, krpc_connection_t *connection, char *error, size_t error_size) {
	if(host == NULL || host[0] == 0 || connection == NULL || port == 0) {
		if(error != NULL && error_size > 0) snprintf(error, error_size, "invalid kRPC endpoint");
		return 0;
	}
	char endpoint[512];
	int is_ipv6 = strchr(host, ':') != NULL;
	int length = snprintf(endpoint, sizeof(endpoint), is_ipv6 ? "[%s]:%u" : "%s:%u", host, (unsigned int)port);
	if(length < 0 || (size_t)length >= sizeof(endpoint)) {
		if(error != NULL && error_size > 0) snprintf(error, error_size, "kRPC endpoint is too long");
		return 0;
	}
	krpc_connection_t opened;
	krpc_error_t status = krpc_open(&opened, endpoint);
	if(status != KRPC_OK) {
		ksp_rpc_error_text(status, error, error_size);
		return 0;
	}
	status = krpc_connect(opened, "KMAT");
	if(status != KRPC_OK) {
		ksp_rpc_error_text(status, error, error_size);
		krpc_close(opened);
		return 0;
	}
	*connection = opened;
	return 1;
}

void ksp_rpc_disconnect(krpc_connection_t connection) {
	krpc_close(connection);
}
