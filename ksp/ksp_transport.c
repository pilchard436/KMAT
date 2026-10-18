#include "krpc_types.h"
#include <krpc_cnano/communication.h>
#include <krpc_cnano/error.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET KspSocket;
#define KSP_INVALID_SOCKET INVALID_SOCKET
#define ksp_close_socket closesocket
static int winsock_started;
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int KspSocket;
#define KSP_INVALID_SOCKET (-1)
#define ksp_close_socket close
#endif

static int split_endpoint(const char *endpoint, char *host, size_t host_size, char *port, size_t port_size) {
	const char *host_start = endpoint, *host_end = NULL, *port_start = NULL;
	if(endpoint[0] == '[') {
		host_start = endpoint + 1;
		host_end = strchr(host_start, ']');
		if(host_end == NULL || host_end[1] != ':') return 0;
		port_start = host_end + 2;
	} else {
		host_end = strrchr(endpoint, ':');
		if(host_end == NULL) return 0;
		port_start = host_end + 1;
	}
	size_t host_length = (size_t)(host_end - host_start);
	size_t port_length = strlen(port_start);
	if(host_length == 0 || host_length >= host_size || port_length == 0 || port_length >= port_size) return 0;
	memcpy(host, host_start, host_length);
	host[host_length] = 0;
	memcpy(port, port_start, port_length + 1);
	char *end = NULL;
	unsigned long parsed_port = strtoul(port, &end, 10);
	return end != port && *end == 0 && parsed_port > 0 && parsed_port <= 65535;
}

static void set_socket_timeout(KspSocket socket_handle) {
#ifdef _WIN32
	DWORD timeout_ms = 10000;
	setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
	setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
	struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
	setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
	int no_delay = 1;
	setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay));
}

krpc_error_t krpc_open(krpc_connection_t *connection, const krpc_connection_config_t *config) {
	const char *endpoint = (const char *)config;
	char host[256], port[8];
	if(connection == NULL || endpoint == NULL || !split_endpoint(endpoint, host, sizeof(host), port, sizeof(port)))
		KRPC_RETURN_ERROR(IO, "expected a host and port");
#ifdef _WIN32
	if(!winsock_started) {
		WSADATA data;
		if(WSAStartup(MAKEWORD(2, 2), &data) != 0) KRPC_RETURN_ERROR(IO, "failed to start Winsock");
		winsock_started = 1;
	}
#endif
	struct addrinfo hints = {0}, *addresses = NULL;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if(getaddrinfo(host, port, &hints, &addresses) != 0) KRPC_RETURN_ERROR(IO, "failed to resolve kRPC server");
	KspSocket connected = KSP_INVALID_SOCKET;
	for(struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
		KspSocket candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if(candidate == KSP_INVALID_SOCKET) continue;
		set_socket_timeout(candidate);
		if(connect(candidate, address->ai_addr, (int)address->ai_addrlen) == 0) {
			connected = candidate;
			break;
		}
		ksp_close_socket(candidate);
	}
	freeaddrinfo(addresses);
	if(connected == KSP_INVALID_SOCKET) KRPC_RETURN_ERROR(IO, "failed to connect to kRPC server");
	*connection = (krpc_connection_t)(uintptr_t)connected;
	return KRPC_OK;
}

krpc_error_t krpc_close(krpc_connection_t connection) {
	if(ksp_close_socket((KspSocket)(uintptr_t)connection) != 0) KRPC_RETURN_ERROR(IO, "failed to close kRPC connection");
	return KRPC_OK;
}

krpc_error_t krpc_read(krpc_connection_t connection, uint8_t *buffer, size_t count) {
	size_t total = 0;
	while(total < count) {
		size_t remaining = count - total;
		int request = remaining > INT_MAX ? INT_MAX : (int)remaining;
		int received = recv((KspSocket)(uintptr_t)connection, (char *)buffer + total, request, 0);
		if(received < 0) {
#ifndef _WIN32
			if(errno == EINTR) continue;
#endif
			KRPC_RETURN_ERROR(IO, "failed to read from kRPC server");
		}
		if(received == 0) KRPC_RETURN_ERROR(EOF, "kRPC server closed the connection");
		total += (size_t)received;
	}
	return KRPC_OK;
}

krpc_error_t krpc_write(krpc_connection_t connection, const uint8_t *buffer, size_t count) {
	size_t total = 0;
	while(total < count) {
		size_t remaining = count - total;
		int request = remaining > INT_MAX ? INT_MAX : (int)remaining;
#ifdef MSG_NOSIGNAL
		int sent = send((KspSocket)(uintptr_t)connection, (const char *)buffer + total, request, MSG_NOSIGNAL);
#else
		int sent = send((KspSocket)(uintptr_t)connection, (const char *)buffer + total, request, 0);
#endif
		if(sent < 0) {
#ifndef _WIN32
			if(errno == EINTR) continue;
#endif
			KRPC_RETURN_ERROR(IO, "failed to write to kRPC server");
		}
		if(sent == 0) KRPC_RETURN_ERROR(IO, "kRPC server closed the connection");
		total += (size_t)sent;
	}
	return KRPC_OK;
}
