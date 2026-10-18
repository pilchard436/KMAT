#include "ksp_session.h"
#include <krpc_cnano/memory.h>
#include <krpc_cnano/services/space_center.h>
#include <orbitlib_fileio.h>
#include "tools/celestial_systems.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KSP_BODIES 1000

typedef struct KspBodySnapshot {
	krpc_SpaceCenter_CelestialBody_t object;
	krpc_SpaceCenter_CelestialBody_t parent;
	krpc_SpaceCenter_Orbit_t orbit;
	Body *body;
	double semi_major_axis, eccentricity, inclination, raan, argument_of_periapsis, true_anomaly;
} KspBodySnapshot;

static krpc_connection_t session_connection;
static CelestSystem *session_system;
static int session_connected;
static KspVesselStart session_vessel_start;
static int session_vessel_start_available;
static char session_vessel_start_error[256];

static int checked_rpc(krpc_error_t status, char *error, size_t error_size) {
	if(status == KRPC_OK) return 1;
	ksp_rpc_error_text(status, error, error_size);
	return 0;
}

static int find_remote_body(const KspBodySnapshot *bodies, size_t count, krpc_SpaceCenter_CelestialBody_t object) {
	for(size_t i = 0; i < count; i++)
		if(bodies[i].object == object) return (int)i;
	return -1;
}

static void free_remote_snapshot(KspBodySnapshot *bodies, size_t count) {
	if(bodies == NULL) return;
	for(size_t i = 0; i < count; i++) free(bodies[i].body);
	free(bodies);
}

static CelestSystem *read_live_system(krpc_connection_t connection, char *error, size_t error_size) {
	krpc_dictionary_string_object_t remote = KRPC_NULL_DICTIONARY;
	KspBodySnapshot *bodies = NULL;
	CelestSystem *system = NULL;
	int central_index = -1;
	if(!checked_rpc(krpc_SpaceCenter_Bodies(connection, &remote), error, error_size)) goto cleanup;
	if(remote.size == 0 || remote.size > MAX_KSP_BODIES) {
		snprintf(error, error_size, "KSP returned an invalid celestial-body count");
		goto cleanup;
	}
	bodies = calloc(remote.size, sizeof(*bodies));
	if(bodies == NULL) {
		snprintf(error, error_size, "not enough memory for the KSP body list");
		goto cleanup;
	}
	for(size_t i = 0; i < remote.size; i++) {
		const char *name = remote.entries[i].key;
		if(name == NULL || strlen(name) >= sizeof(((Body *)0)->name)) {
			snprintf(error, error_size, "a KSP body name is too long for KMAT");
			goto cleanup;
		}
		bodies[i].object = remote.entries[i].value;
		bodies[i].body = new_body();
		if(bodies[i].body == NULL) {
			snprintf(error, error_size, "not enough memory for the KSP body list");
			goto cleanup;
		}
		snprintf(bodies[i].body->name, sizeof(bodies[i].body->name), "%s", name);
		if(!checked_rpc(krpc_SpaceCenter_CelestialBody_GravitationalParameter(connection, &bodies[i].body->mu, bodies[i].object), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_CelestialBody_EquatorialRadius(connection, &bodies[i].body->radius, bodies[i].object), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_CelestialBody_RotationalPeriod(connection, &bodies[i].body->rotation_period, bodies[i].object), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_CelestialBody_AtmosphereDepth(connection, &bodies[i].body->atmo_alt, bodies[i].object), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_CelestialBody_Orbit(connection, &bodies[i].orbit, bodies[i].object), error, error_size)) goto cleanup;
		if(bodies[i].orbit == KRPC_NULL) {
			if(central_index >= 0) {
				snprintf(error, error_size, "KSP returned multiple central bodies");
				goto cleanup;
			}
			central_index = (int)i;
			continue;
		}
		if(!checked_rpc(krpc_SpaceCenter_Orbit_Body(connection, &bodies[i].parent, bodies[i].orbit), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_Orbit_SemiMajorAxis(connection, &bodies[i].semi_major_axis, bodies[i].orbit), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_Orbit_Eccentricity(connection, &bodies[i].eccentricity, bodies[i].orbit), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_Orbit_Inclination(connection, &bodies[i].inclination, bodies[i].orbit), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_Orbit_LongitudeOfAscendingNode(connection, &bodies[i].raan, bodies[i].orbit), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_Orbit_ArgumentOfPeriapsis(connection, &bodies[i].argument_of_periapsis, bodies[i].orbit), error, error_size) ||
			!checked_rpc(krpc_SpaceCenter_Orbit_TrueAnomalyAtUT(connection, &bodies[i].true_anomaly, bodies[i].orbit, 0), error, error_size)) goto cleanup;
	}
	if(central_index < 0) {
		snprintf(error, error_size, "could not identify the KSP central body");
		goto cleanup;
	}
	system = new_system();
	if(system == NULL) {
		snprintf(error, error_size, "not enough memory for the KSP system");
		goto cleanup;
	}
	snprintf(system->name, sizeof(system->name), "KSP live system");
	system->prop_method = ORB_ELEMENTS;
	system->ut0 = 0;
	system->cb = bodies[central_index].body;
	system->num_bodies = (int)remote.size - 1;
	system->bodies = calloc((size_t)system->num_bodies, sizeof(*system->bodies));
	if(system->num_bodies > 0 && system->bodies == NULL) {
		snprintf(error, error_size, "not enough memory for the KSP system");
		free(system);
		system = NULL;
		goto cleanup;
	}
	size_t next_body = 0;
	for(size_t i = 0; i < remote.size; i++) {
		if((int)i == central_index) continue;
		int parent_index = find_remote_body(bodies, remote.size, bodies[i].parent);
		if(parent_index < 0) {
			snprintf(error, error_size, "a KSP body references an unknown parent");
			free(system->bodies);
			free(system);
			system = NULL;
			goto cleanup;
		}
		bodies[i].body->orbit = constr_orbit_from_elements(bodies[i].semi_major_axis, bodies[i].eccentricity,
			bodies[i].inclination, bodies[i].raan, bodies[i].argument_of_periapsis, bodies[i].true_anomaly,
			bodies[parent_index].body);
		system->bodies[next_body++] = bodies[i].body;
	}
	for(size_t i = 0; i < remote.size; i++) bodies[i].body = NULL;
	parse_and_sort_into_celestial_subsystems(system);

cleanup:
	if(system == NULL) free_remote_snapshot(bodies, remote.size);
	else {
		for(size_t i = 0; i < remote.size; i++) free(bodies[i].body);
		free(bodies);
	}
	if(remote.entries != NULL) {
		for(size_t i = 0; i < remote.size; i++) krpc_free(remote.entries[i].key);
		KRPC_FREE_DICTIONARY(remote);
	}
	return system;
}

static int read_vessel_start(krpc_connection_t connection, CelestSystem *system,
		KspVesselStart *start, char *error, size_t error_size) {
	krpc_SpaceCenter_Vessel_t vessel = KRPC_NULL;
	krpc_SpaceCenter_Orbit_t orbit = KRPC_NULL;
	krpc_SpaceCenter_CelestialBody_t parent = KRPC_NULL;
	char *vessel_name = NULL, *body_name = NULL;
	int result = 0;
	memset(start, 0, sizeof(*start));
	if(error != NULL && error_size > 0) error[0] = 0;
	double ut = 0;
	if(!checked_rpc(krpc_SpaceCenter_ActiveVessel(connection, &vessel), error, error_size) || vessel == KRPC_NULL) {
		if(error != NULL && error_size > 0 && error[0] == 0) snprintf(error, error_size, "KSP has no active vessel");
		goto cleanup;
	}
	if(!checked_rpc(krpc_SpaceCenter_Vessel_Name(connection, &vessel_name, vessel), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_Vessel_Orbit(connection, &orbit, vessel), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_UT(connection, &ut), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_Orbit_Body(connection, &parent, orbit), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_CelestialBody_Name(connection, &body_name, parent), error, error_size)) goto cleanup;
	Body *body = get_body_by_name(body_name, system);
	if(body == NULL) {
		if(error != NULL && error_size > 0)
			snprintf(error, error_size, "Active vessel orbits a body not found in the matched KMAT system: %s", body_name);
		goto cleanup;
	}
	if(!checked_rpc(krpc_SpaceCenter_Orbit_SemiMajorAxis(connection, &start->orbit.semi_major_axis, orbit), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_Orbit_Eccentricity(connection, &start->orbit.eccentricity, orbit), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_Orbit_Inclination(connection, &start->orbit.inclination, orbit), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_Orbit_LongitudeOfAscendingNode(connection, &start->orbit.raan, orbit), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_Orbit_ArgumentOfPeriapsis(connection, &start->orbit.arg_periapsis, orbit), error, error_size) ||
		!checked_rpc(krpc_SpaceCenter_Orbit_TrueAnomalyAtUT(connection, &start->orbit.true_anomaly, orbit, ut), error, error_size)) goto cleanup;
	if(!starting_orbit_is_valid(&start->orbit, body)) {
		if(error != NULL && error_size > 0)
			snprintf(error, error_size, "Active vessel is not in a safe, bound orbit around %s", body->name);
		goto cleanup;
	}
	snprintf(start->vessel_name, sizeof(start->vessel_name), "%s", vessel_name);
	start->system = system;
	start->orbiting_body = body;
	start->universal_time = ut;
	start->date_jd = system->ut0 + ut / 86400.0;
	result = 1;

cleanup:
	if(vessel_name != NULL) krpc_free(vessel_name);
	if(body_name != NULL) krpc_free(body_name);
	return result;
}

int ksp_session_connect_and_match(const char *host, uint16_t port, int *matched_index,
		char *error, size_t error_size) {
	krpc_connection_t connection;
	if(matched_index != NULL) *matched_index = -1;
	if(!ksp_rpc_connect(host, port, &connection, error, error_size)) return 0;
	CelestSystem *live_system = read_live_system(connection, error, error_size);
	if(live_system == NULL) {
		ksp_rpc_disconnect(connection);
		return 0;
	}
	int index = find_equivalent_available_system(live_system);
	free_celestial_system(live_system);
	if(index < 0) {
		snprintf(error, error_size, "The running KSP system does not match any system loaded in KMAT.");
		ksp_rpc_disconnect(connection);
		return 0;
	}
	ksp_session_disconnect();
	session_connection = connection;
	session_system = get_available_systems()[index];
	session_connected = 1;
	session_vessel_start_available = read_vessel_start(connection, session_system, &session_vessel_start,
		session_vessel_start_error, sizeof(session_vessel_start_error));
	if(matched_index != NULL) *matched_index = index;
	return 1;
}

int ksp_session_is_connected(void) {
	return session_connected;
}

int ksp_session_get_connection(krpc_connection_t *connection, CelestSystem **system) {
	if(!session_connected) return 0;
	if(connection != NULL) *connection = session_connection;
	if(system != NULL) *system = session_system;
	return 1;
}

int ksp_session_get_vessel_start(KspVesselStart *start, char *error, size_t error_size) {
	if(!session_connected || !session_vessel_start_available) {
		if(error != NULL && error_size > 0)
			snprintf(error, error_size, "%s", session_vessel_start_error[0] != 0 ?
				session_vessel_start_error : "KSP is not connected");
		return 0;
	}
	if(start != NULL) *start = session_vessel_start;
	return 1;
}

void ksp_session_disconnect(void) {
	if(!session_connected) return;
	ksp_rpc_disconnect(session_connection);
	session_connected = 0;
	session_system = NULL;
	session_vessel_start_available = 0;
	session_vessel_start_error[0] = 0;
}
