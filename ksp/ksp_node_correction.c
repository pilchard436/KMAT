#include "ksp_node_internal.h"
#include "ksp_nodes.h"
#include <krpc_cnano/memory.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Adds a small node on the vessel's current heliocentric trajectory that re-aims it at the next planned encounter, so
// execution errors are fixed early instead of through a large burn at the encounter.
int ksp_session_add_correction_node(struct ItinStep *route, double arr_periapsis, char *message, size_t message_size) {
	if(message != NULL && message_size > 0) message[0] = 0;
	krpc_connection_t connection;
	CelestSystem *system = NULL;
	if(!ksp_session_get_connection(&connection, &system)) {
		set_message(message, message_size, "Connect to KSP first.");
		return 0;
	}
	if(route == NULL) {
		set_message(message, message_size, "Load or calculate an itinerary in the Transfer Planner first.");
		return 0;
	}
	krpc_SpaceCenter_Vessel_t vessel;
	krpc_SpaceCenter_Orbit_t orbit, patch, next = KRPC_NULL;
	krpc_SpaceCenter_Control_t control;
	krpc_SpaceCenter_CelestialBody_t orbit_body, system_root;
	krpc_SpaceCenter_ReferenceFrame_t system_frame;
	char *orbit_body_name = NULL;
	double current_ut, soi_time, patch_end;
	if(!rpc_ok(krpc_SpaceCenter_ActiveVessel(connection, &vessel), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_UT(connection, &current_ut), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Vessel_Orbit(connection, &orbit, vessel), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Orbit_Body(connection, &orbit_body, orbit), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &orbit_body_name, orbit_body), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Vessel_Control(connection, &control, vessel), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Orbit_TimeToSOIChange(connection, &soi_time, orbit), message, message_size) ||
		!find_remote_body(connection, system->cb->name, &system_root, message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_CelestialBody_NonRotatingReferenceFrame(connection, &system_frame, system_root), message, message_size)) goto fail;
	int in_central = strcmp(orbit_body_name, system->cb->name) == 0;
	krpc_free(orbit_body_name);
	orbit_body_name = NULL;
	patch = orbit;
	double patch_start = current_ut;
	if(!in_central) {
		krpc_SpaceCenter_CelestialBody_t next_body;
		char *next_name = NULL;
		if(!isfinite(soi_time) || !rpc_ok(krpc_SpaceCenter_Orbit_NextOrbit(connection, &next, orbit), NULL, 0) || next == KRPC_NULL ||
			!rpc_ok(krpc_SpaceCenter_Orbit_Body(connection, &next_body, next), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &next_name, next_body), NULL, 0)) {
			set_message(message, message_size, "The vessel is not on a path that leaves its current body yet. Add the departure node first.");
			goto fail;
		}
		int central = strcmp(next_name, system->cb->name) == 0;
		krpc_free(next_name);
		if(!central) {
			set_message(message, message_size, "The vessel's path does not leave to the system's central body, so there is nothing to correct yet.");
			goto fail;
		}
		patch = next;
		patch_start = current_ut + soi_time;
	}
	double node_ut = fmax(patch_start + 3600, current_ut + 600);
	if(!rpc_ok(krpc_SpaceCenter_Orbit_TimeToSOIChange(connection, &patch_end, patch), NULL, 0)) patch_end = INFINITY;
	patch_end = isfinite(patch_end) ? current_ut + patch_end : INFINITY;
	if(node_ut > patch_end - 3600) node_ut = fmax(current_ut + 600, patch_end - 3600);
	double node_date = node_ut / 86400.0 + system->ut0;
	struct ItinStep *target = NULL;
	for(struct ItinStep *step = get_first(route); step != NULL; step = step->next != NULL ? step->next[0] : NULL)
		if(step != get_first(route) && step->body != NULL && step->date > node_date + 0.05) { target = step; break; }
	if(target == NULL) {
		set_message(message, message_size, "The itinerary has no later encounter to correct toward.");
		goto fail;
	}
	double leg_seconds = (target->date - node_date) * 86400.0;
	Vector3 position, velocity;
	if(leg_seconds <= 3600 || !orbit_state_at(connection, patch, system_frame, node_ut, &position, &velocity, message, message_size)) {
		if(leg_seconds <= 3600) set_message(message, message_size, "The next encounter is too close for a course correction.");
		goto fail;
	}
	position = vec3(position.x, position.z, position.y);
	velocity = vec3(velocity.x, velocity.z, velocity.y);
	Vector3 base_offset = planned_aim_offset(target, arr_periapsis), correction = vec3(0, 0, 0);
	krpc_SpaceCenter_Node_t node;
	if(!rpc_ok(krpc_SpaceCenter_Control_AddNode(connection, &node, control, node_ut, 0, 0, 0), message, message_size)) goto fail;
	double burn_size = 0;
	for(int iteration = 0; iteration < 10; iteration++) {
		Vector3 aim_point = add_vec3(target->r, add_vec3(base_offset, correction)), v_leg = vec3(0, 0, 0);
		double closest = INFINITY;
		for(int revolutions = 0; revolutions <= 6; revolutions++)
			for(int branch = 0; branch < (revolutions == 0 ? 1 : 2); branch++) {
				Lambert3 leg = calc_lambert3_revs(position, aim_point, leg_seconds, system->cb, revolutions, branch);
				if(leg.success != LAMBERT_SUCCESS || !isfinite(leg.v0.x + leg.v0.y + leg.v0.z)) continue;
				double difference = mag_vec3(subtract_vec3(leg.v0, velocity));
				if(difference < closest) { closest = difference; v_leg = leg.v0; }
			}
		Vector3 burn = subtract_vec3(v_leg, velocity), components;
		if(!isfinite(closest) || !ksp_project_delta_v_to_node(position, velocity, burn, &components)) {
			set_message(message, message_size, "Could not solve a correction to the planned encounter.");
			krpc_SpaceCenter_Node_Remove(connection, node);
			goto fail;
		}
		burn_size = mag_vec3(burn);
		if(!rpc_ok(krpc_SpaceCenter_Node_set_Prograde(connection, node, components.x), message, message_size) ||
			!rpc_ok(krpc_SpaceCenter_Node_set_Normal(connection, node, components.y), message, message_size) ||
			!rpc_ok(krpc_SpaceCenter_Node_set_Radial(connection, node, components.z), message, message_size)) goto fail;
		Vector3 predicted;
		if(!predicted_encounter_offset(connection, node, target, &predicted)) break;
		Vector3 wanted = base_offset;
		if(target->next == NULL && mag_vec3(predicted) > 0) wanted = scale_vec3(norm_vec3(predicted), mag_vec3(base_offset));
		Vector3 miss = subtract_vec3(wanted, predicted);
		if(mag_vec3(miss) < 1000) break;
		correction = add_vec3(correction, miss);
	}
	if(message != NULL && message_size > 0) {
		int length = snprintf(message, message_size, "Added course correction at UT %.1f (%.2f m/s) toward %s.", node_ut, burn_size, target->body->name);
		if(length > 0 && (size_t)length < message_size)
			describe_predicted_path(connection, node, system, route, arr_periapsis, message + length, message_size - length);
	}
	return 1;

fail:
	if(orbit_body_name != NULL) krpc_free(orbit_body_name);
	return 0;
}
