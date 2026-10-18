#ifndef KMAT_KSP_NODE_INTERNAL_H
#define KMAT_KSP_NODE_INTERNAL_H

#include "ksp_session.h"
#include <krpc_cnano/services/space_center.h>

typedef struct KspNodeCandidate {
	Body *frame_body;
	double date;
	double step_date;
	int is_departure;
	struct ItinStep *flyby_step;
	double departure_anomaly;
	Vector3 excess;
	Vector3 delta_v;
} KspNodeCandidate;

void set_message(char *message, size_t message_size, const char *text);
int rpc_ok(krpc_error_t result, char *message, size_t message_size);
int find_remote_body(krpc_connection_t connection, const char *name,
		krpc_SpaceCenter_CelestialBody_t *body, char *message, size_t message_size);
int orbit_state_at(krpc_connection_t connection, krpc_SpaceCenter_Orbit_t orbit,
		krpc_SpaceCenter_ReferenceFrame_t frame, double ut, Vector3 *position, Vector3 *velocity,
		char *message, size_t message_size);
int outgoing_asymptote(double mu, Vector3 r, Vector3 v, Vector3 *direction, double *periapsis);
int burn_to_asymptote(double mu, Vector3 r, Vector3 v_old, Vector3 v_infinity, double min_periapsis,
		Vector3 *burns, int max_burns);
Vector3 planned_aim_offset(struct ItinStep *target, double arr_periapsis);
int predicted_encounter_offset(krpc_connection_t connection, krpc_SpaceCenter_Node_t node,
		struct ItinStep *target, Vector3 *offset);
void describe_predicted_path(krpc_connection_t connection, krpc_SpaceCenter_Node_t node, CelestSystem *system,
		struct ItinStep *route, double arr_periapsis, char *text, size_t text_size);
void refine_node(krpc_connection_t connection, CelestSystem *system, struct ItinStep *route,
		krpc_SpaceCenter_Node_t node, const KspVesselStart *start, const KspNodeCandidate *candidate,
		krpc_SpaceCenter_ReferenceFrame_t patch_frame, krpc_SpaceCenter_ReferenceFrame_t system_frame,
		Vector3 position, Vector3 velocity, double arr_periapsis);

#endif
