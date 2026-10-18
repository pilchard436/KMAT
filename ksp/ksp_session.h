#ifndef KMAT_KSP_SESSION_H
#define KMAT_KSP_SESSION_H

#include "ksp_rpc.h"
#include "orbitlib.h"
#include "orbit_calculator/itin_tool.h"
#include "tools/starting_orbit.h"
#include <stdint.h>

typedef struct KspVesselStart {
	char vessel_name[64];
	CelestSystem *system;
	Body *orbiting_body;
	double universal_time;
	double date_jd;
	StartingOrbit orbit;
} KspVesselStart;

int ksp_session_connect_and_match(const char *host, uint16_t port, int *matched_index,
		char *error, size_t error_size);
int ksp_session_is_connected(void);
int ksp_session_get_connection(krpc_connection_t *connection, CelestSystem **system);
int ksp_session_get_vessel_start(KspVesselStart *start, char *error, size_t error_size);
void ksp_session_disconnect(void);
int ksp_session_add_next_maneuver_node(struct ItinStep *route, double dep_periapsis,
		double arr_periapsis, enum LastTransferType transfer_type, char *message, size_t message_size);
int ksp_session_add_correction_node(struct ItinStep *route, double arr_periapsis, char *message, size_t message_size);

#endif
