#include "ksp_nodes.h"
#include "ksp_node_internal.h"
#include "ksp_session.h"
#include "orbit_calculator/flyby_policy.h"
#include "orbit_calculator/moon_flyby.h"
#include "tools/starting_orbit.h"
#include <krpc_cnano/memory.h>
#include <krpc_cnano/services/space_center.h>
#include <math.h>
#include <stdio.h>
#include <string.h>



void set_message(char *message, size_t message_size, const char *text) {
	if(message != NULL && message_size > 0) snprintf(message, message_size, "%s", text);
}

int rpc_ok(krpc_error_t result, char *message, size_t message_size) {
	if(result == KRPC_OK) return 1;
	ksp_rpc_error_text(result, message, message_size);
	return 0;
}

int ksp_project_delta_v_to_node(Vector3 position, Vector3 velocity, Vector3 delta_v, Vector3 *components) {
	if(components == NULL) return 0;
	Vector3 radial_out = norm_vec3(position);
	Vector3 normal_raw = cross_vec3(position, velocity);
	if(!(mag_vec3(radial_out) > 0) || !(mag_vec3(normal_raw) > 0)) return 0;
	// KSP node axes: prograde along the velocity, normal along r x v, radial perpendicular to both.
	Vector3 normal = norm_vec3(normal_raw);
	Vector3 prograde = norm_vec3(velocity);
	Vector3 radial = cross_vec3(prograde, normal);
	if(!(mag_vec3(prograde) > 0) || !(mag_vec3(radial) > 0)) return 0;
	*components = vec3(dot_vec3(delta_v, prograde), dot_vec3(delta_v, normal), dot_vec3(delta_v, radial));
	return isfinite(components->x) && isfinite(components->y) && isfinite(components->z);
}

int find_remote_body(krpc_connection_t connection, const char *name,
		krpc_SpaceCenter_CelestialBody_t *body, char *message, size_t message_size) {
	krpc_dictionary_string_object_t bodies = KRPC_NULL_DICTIONARY;
	if(!rpc_ok(krpc_SpaceCenter_Bodies(connection, &bodies), message, message_size)) return 0;
	int found = 0;
	for(size_t i = 0; i < bodies.size; i++) {
		if(strcmp(bodies.entries[i].key, name) == 0) {
			*body = bodies.entries[i].value;
			found = 1;
			break;
		}
	}
	for(size_t i = 0; i < bodies.size; i++) krpc_free(bodies.entries[i].key);
	KRPC_FREE_DICTIONARY(bodies);
	if(!found) set_message(message, message_size, "The system central body is not present in KSP.");
	return found;
}

static int get_remote_body(krpc_connection_t connection, const char *name,
		krpc_SpaceCenter_CelestialBody_t *body, char *message, size_t message_size) {
	krpc_dictionary_string_object_t bodies = KRPC_NULL_DICTIONARY;
	if(!rpc_ok(krpc_SpaceCenter_Bodies(connection, &bodies), message, message_size)) return 0;
	int found = 0;
	for(size_t i = 0; i < bodies.size; i++) {
		if(strcmp(bodies.entries[i].key, name) == 0) {
			*body = bodies.entries[i].value;
			found = 1;
			break;
		}
	}
	for(size_t i = 0; i < bodies.size; i++) krpc_free(bodies.entries[i].key);
	KRPC_FREE_DICTIONARY(bodies);
	if(!found) set_message(message, message_size, "The maneuver's reference body is not present in KSP.");
	return found;
}

int orbit_state_at(krpc_connection_t connection, krpc_SpaceCenter_Orbit_t orbit,
		krpc_SpaceCenter_ReferenceFrame_t frame, double ut, Vector3 *position, Vector3 *velocity,
		char *message, size_t message_size) {
	const double half_step = 0.5;
	krpc_tuple_double_double_double_t at, before, after;
	if(!rpc_ok(krpc_SpaceCenter_Orbit_PositionAt(connection, &at, orbit, ut, frame), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Orbit_PositionAt(connection, &before, orbit, ut - half_step, frame), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Orbit_PositionAt(connection, &after, orbit, ut + half_step, frame), message, message_size)) return 0;
	*position = vec3(at.e0, at.e1, at.e2);
	*velocity = scale_vec3(vec3(after.e0 - before.e0, after.e1 - before.e1, after.e2 - before.e2), 1 / (2 * half_step));
	return isfinite(position->x + position->y + position->z + velocity->x + velocity->y + velocity->z);
}

static double mean_anomaly_from_true(double true_anomaly, double eccentricity) {
	double eccentric = 2 * atan2(sqrt(1 - eccentricity) * sin(true_anomaly / 2), sqrt(1 + eccentricity) * cos(true_anomaly / 2));
	return eccentric - eccentricity * sin(eccentric);
}

static int next_candidate(struct ItinStep *first, CelestSystem *system, const KspVesselStart *start,
		double dep_periapsis, double arr_periapsis, enum LastTransferType transfer_type,
		double after_date, const char *start_error, KspNodeCandidate *candidate, char *message, size_t message_size) {
	for(struct ItinStep *step = get_first(first); step != NULL; step = step->next != NULL ? step->next[0] : NULL) {
		if(step->date <= after_date) continue;
		if(step->body == NULL) {
			if(step->prev == NULL || step->next == NULL) continue;
			*candidate = (KspNodeCandidate){.frame_body = system->cb, .date = step->date, .step_date = step->date,
				.delta_v = subtract_vec3(step->next[0]->v_dep, step->v_arr)};
		} else if(step->prev == NULL && step->next != NULL) {
			if(start->orbiting_body == NULL) {
				set_message(message, message_size, start_error);
				return -1;
			}
			if(step->body != start->orbiting_body) {
				set_message(message, message_size, "The itinerary departure body differs from the connected vessel's parking-orbit body.");
				return -1;
			}
			Vector3 excess = subtract_vec3(step->next[0]->v_dep, step->v_body);
			double delta_seconds = (step->date - start->date_jd) * 86400.0;
			if(delta_seconds < -1) {
				set_message(message, message_size, "The itinerary departure time is before the connected vessel snapshot. Reconnect to KSP.");
				return -1;
			}
			// The itinerary's departure dv assumes the best point on the starting orbit, so the node goes at the
			// pass of that point nearest the itinerary date rather than wherever the vessel happens to be.
			double best_burn, best_anomaly;
			if(!starting_orbit_best_departure_burn(&start->orbit, start->orbiting_body, excess, &best_burn, &best_anomaly)) {
				set_message(message, message_size, "Could not calculate a safe departure burn from the connected orbit.");
				return -1;
			}
			double period = starting_orbit_period(&start->orbit, start->orbiting_body);
			double e = start->orbit.eccentricity;
			double mean_now = mean_anomaly_from_true(start->orbit.true_anomaly, e);
			double mean_best = mean_anomaly_from_true(best_anomaly, e);
			double wait = fmod(fmod((mean_best - mean_now) / (2 * M_PI), 1) + 1, 1) * period;
			double passes = round((delta_seconds - wait) / period);
			if(passes < 0) passes = 0;
			double node_seconds = wait + passes * period;
			StartingOrbit orbit_at_departure = start->orbit;
			orbit_at_departure.true_anomaly = best_anomaly;
			Vector3 burn;
			double burn_magnitude;
			if(!starting_orbit_departure_burn_at(&orbit_at_departure, start->orbiting_body,
				orbit_at_departure.true_anomaly, excess, &burn_magnitude, &burn)) {
				set_message(message, message_size, "Could not calculate a safe departure burn from the connected orbit.");
				return -1;
			}
			*candidate = (KspNodeCandidate){.frame_body = step->body, .date = start->date_jd + node_seconds / 86400.0, .is_departure = 1, .step_date = step->date, .departure_anomaly = best_anomaly, .excess = excess, .delta_v = burn};
		} else if(step->next != NULL) {
			PoweredFlybyResult flyby;
			if(!calculate_flyby_with_moons(step->v_arr, step->next[0]->v_dep, step->v_body, step->body,
				flyby_minimum_periapsis(step->body), step->date, &flyby)) {
				set_message(message, message_size, "Could not reconstruct the powered flyby burn for this route.");
				return -1;
			}
			*candidate = (KspNodeCandidate){.frame_body = step->body, .date = step->date, .step_date = step->date,
				.flyby_step = step, .delta_v = flyby.burn_delta_v_vector};
		} else if(transfer_type == TF_CAPTURE || transfer_type == TF_CIRC) {
			Vector3 excess = subtract_vec3(step->v_arr, step->v_body);
			double radius = alt2radius(step->body, arr_periapsis), speed = mag_vec3(excess);
			double plain = transfer_type == TF_CIRC ? dv_circ(step->body, radius, speed) : dv_capture(step->body, radius, speed);
			MoonAssist assist;
			arrival_dv_with_assist(step->body, excess, radius, transfer_type == TF_CIRC, plain, step->date, &assist);
			if(assist.moon != NULL) {
				set_message(message, message_size, "This arrival uses a moon assist; its aiming and residual capture nodes are not supported yet.");
				return -1;
			}
			set_message(message, message_size, "Arrival insertion is deferred until its periapsis epoch is modeled explicitly.");
			return -1;
		} else continue;
		if(mag_vec3(candidate->delta_v) >= 0.05) return 1;
	}
	set_message(message, message_size, "The itinerary has no remaining maneuver nodes to export.");
	return 0;
}

// Aims the flyby burn at the vessel's real state, integrating the planet's moons, so the trajectory leaving the
// sphere of influence reaches the planned next encounter. A burn that would hit a moon is tried earlier on the approach.
static int aim_flyby_burn(krpc_connection_t connection, CelestSystem *system, struct ItinStep *flyby_step,
		krpc_SpaceCenter_Orbit_t patch, krpc_SpaceCenter_ReferenceFrame_t frame, double pe_ut, double earliest_ut,
		double latest_ut, double arr_periapsis, enum LastTransferType transfer_type,
		KspNodeCandidate *candidate, char *message, size_t message_size) {
	static const double offsets[] = {0, 600, -600, 1800, -1800, 3600, -3600, 7200, -7200, 14400, -14400, 28800, -28800};
	Body *planet = flyby_step->body, *central = planet->orbit.cb;
	struct ItinStep *following = flyby_step->next[0];
	Vector3 aim_point = add_vec3(following->r, planned_aim_offset(following, arr_periapsis));
	double soi = fabs(planet->orbit.a) * pow(planet->mu / central->mu, 0.4);
	int blocked_by_moon = 0;
	for(size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
		double burn_ut = pe_ut + offsets[o];
		if(burn_ut < earliest_ut || burn_ut > latest_ut) continue;
		Vector3 position, velocity;
		if(!orbit_state_at(connection, patch, frame, burn_ut, &position, &velocity, message, message_size)) return -1;
		// kRPC frames are left-handed with y up; KMAT's are right-handed with z up.
		position = vec3(position.x, position.z, position.y);
		velocity = vec3(velocity.x, velocity.z, velocity.y);
		double burn_jd = burn_ut / 86400.0 + system->ut0, best_cost = INFINITY;
		Vector3 best_burn = vec3(0, 0, 0);
		for(int branch = 0; branch < 3; branch++) {
			Vector3 request = subtract_vec3(following->v_dep, osv_from_elements(planet->orbit, burn_jd).v), burn = vec3(0, 0, 0);
			int converged = 0, usable = 1;
			for(int iteration = 0; iteration < 25 && usable && !converged; iteration++) {
				Vector3 candidates[4];
				if(burn_to_asymptote(planet->mu, position, velocity, request, flyby_minimum_periapsis(planet), candidates, 4) <= branch) {
					usable = 0;
					break;
				}
				burn = candidates[branch];
				Vector3 r_end = position, v_end = add_vec3(velocity, burn);
				double elapsed;
				Body *hit;
				if(!moon_propagate_outbound(planet, burn_jd, 20 * 86400.0, soi, &r_end, &v_end, &elapsed, &hit)) { usable = 0; break; }
				if(hit != NULL) {
					if(hit != planet) blocked_by_moon = 1;
					usable = 0;
					break;
				}
				double exit_jd = burn_jd + elapsed / 86400.0, leg_seconds = (following->date - exit_jd) * 86400.0, closest = INFINITY;
				if(leg_seconds <= 3600) { usable = 0; break; }
				OSV body_state = osv_from_elements(planet->orbit, exit_jd);
				Vector3 exit_position = add_vec3(body_state.r, r_end), v_leg = vec3(0, 0, 0);
				for(int revolutions = 0; revolutions <= 6; revolutions++) {
					for(int lambert_branch = 0; lambert_branch < (revolutions == 0 ? 1 : 2); lambert_branch++) {
						Lambert3 leg = calc_lambert3_revs(exit_position, aim_point, leg_seconds, central, revolutions, lambert_branch);
						if(leg.success != LAMBERT_SUCCESS || !isfinite(leg.v0.x + leg.v0.y + leg.v0.z)) continue;
						double difference = mag_vec3(subtract_vec3(leg.v0, following->v_dep));
						if(difference < closest) { closest = difference; v_leg = leg.v0; }
					}
				}
				if(!isfinite(closest)) { usable = 0; break; }
				Vector3 error = subtract_vec3(subtract_vec3(v_leg, body_state.v), v_end);
				if(mag_vec3(error) < 0.005) converged = 1;
				else request = add_vec3(request, error);
			}
			if(usable && converged && mag_vec3(burn) < best_cost) {
				best_cost = mag_vec3(burn);
				best_burn = burn;
			}
		}
		if(isfinite(best_cost)) {
			*candidate = (KspNodeCandidate){.frame_body = planet, .date = burn_jd, .step_date = flyby_step->date, .flyby_step = flyby_step, .delta_v = best_burn};
			return mag_vec3(best_burn) >= 0.05 ? 1 : 0;
		}
	}
	set_message(message, message_size, blocked_by_moon ?
		"Every burn tried for this flyby would hit a moon or reach no encounter. Add the node manually." :
		"Could not aim a burn at the predicted encounter that reaches the next planned encounter.");
	return -1;
}

// A flyby whose itinerary date has already passed (the trajectory slipped) but whose encounter KSP still predicts:
// the burn is rebuilt from the predicted incoming asymptote and placed at the predicted periapsis.
static int late_flyby_candidate(krpc_connection_t connection, CelestSystem *system, struct ItinStep *route,
		krpc_SpaceCenter_Orbit_t orbit, double current_ut, double soi_time, double after_date, double late_floor,
		double arr_periapsis, enum LastTransferType transfer_type,
		KspNodeCandidate *candidate, char *message, size_t message_size) {
	krpc_SpaceCenter_Orbit_t patch = orbit;
	double start_ut = current_ut;
	double end_ut = isfinite(soi_time) && soi_time > 0 ? current_ut + soi_time : INFINITY;
	for(int i = 0; i < 8; i++) {
		krpc_SpaceCenter_CelestialBody_t body;
		krpc_SpaceCenter_ReferenceFrame_t frame;
		char *name = NULL;
		double pe_ut;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_Body(connection, &body, patch), message, message_size) ||
			!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &name, body), message, message_size)) return -1;
		struct ItinStep *best = NULL;
		double best_gap = INFINITY, pe_date = 0;
		int is_central = strcmp(name, system->cb->name) == 0;
		krpc_free(name);
		name = NULL;
		if(!is_central && rpc_ok(krpc_SpaceCenter_Orbit_UTAtTrueAnomaly(connection, &pe_ut, patch, 0), message, message_size) &&
			isfinite(pe_ut) && pe_ut >= start_ut && pe_ut >= current_ut + 0.1 && pe_ut <= end_ut + 1) {
			pe_date = pe_ut / 86400.0 + system->ut0;
			char *patch_name = NULL;
			if(!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &patch_name, body), message, message_size)) return -1;
			struct ItinStep *first_future = NULL;
			for(struct ItinStep *step = get_first(route); step != NULL; step = step->next != NULL ? step->next[0] : NULL)
				if(step->date > after_date) { first_future = step; break; }
			for(struct ItinStep *step = get_first(route); step != NULL; step = step->next != NULL ? step->next[0] : NULL) {
				// Past-dated flybys that KSP still predicts, and the next flyby to come, follow the flown trajectory.
				if(step->body == NULL || step->prev == NULL || step->next == NULL || step->date <= late_floor ||
					(step->date > after_date && step != first_future) ||
					strcmp(step->body->name, patch_name) != 0) continue;
				double gap = fabs(step->date - pe_date);
				double limit = 0.5 * fmin(step->date - step->prev->date, step->next[0]->date - step->date);
				if(gap < limit && gap < best_gap) { best = step; best_gap = gap; }
			}
			krpc_free(patch_name);
		}
		if(best != NULL) {
			if(!rpc_ok(krpc_SpaceCenter_CelestialBody_NonRotatingReferenceFrame(connection, &frame, body), message, message_size)) return -1;
			return aim_flyby_burn(connection, system, best, patch, frame, pe_ut, fmax(start_ut, current_ut) + 1, end_ut - 1, arr_periapsis, transfer_type, candidate, message, message_size);
		}
		if(!isfinite(end_ut)) break;
		krpc_SpaceCenter_Orbit_t next = KRPC_NULL;
		double next_soi;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_NextOrbit(connection, &next, patch), message, message_size)) return -1;
		if(next == KRPC_NULL) break;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_TimeToSOIChange(connection, &next_soi, next), message, message_size)) return -1;
		start_ut = end_ut;
		end_ut = isfinite(next_soi) && next_soi > 0 ? current_ut + next_soi : INFINITY;
		patch = next;
	}
	return 0;
}
static int find_patch(krpc_connection_t connection, krpc_SpaceCenter_Orbit_t orbit, double current_ut, double soi_time,
		const char *body_name, double node_ut, krpc_SpaceCenter_Orbit_t *patch, krpc_SpaceCenter_ReferenceFrame_t *patch_frame,
		char *message, size_t message_size) {
	krpc_SpaceCenter_Orbit_t current = orbit;
	double start_ut = current_ut;
	double end_ut = isfinite(soi_time) && soi_time > 0 ? current_ut + soi_time : INFINITY;
	for(int i = 0; i < 8; i++) {
		krpc_SpaceCenter_CelestialBody_t body;
		char *name = NULL;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_Body(connection, &body, current), message, message_size) ||
			!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &name, body), message, message_size)) return -1;
		int same_body = strcmp(name, body_name) == 0;
		krpc_free(name);
		if(same_body && node_ut >= start_ut && node_ut <= end_ut + 1) {
			if(!rpc_ok(krpc_SpaceCenter_CelestialBody_NonRotatingReferenceFrame(connection, patch_frame, body), message, message_size)) return -1;
			*patch = current;
			return 1;
		}
		if(!isfinite(end_ut)) break;
		krpc_SpaceCenter_Orbit_t next = KRPC_NULL;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_NextOrbit(connection, &next, current), message, message_size)) return -1;
		if(next == KRPC_NULL) break;
		double next_soi;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_TimeToSOIChange(connection, &next_soi, next), message, message_size)) return -1;
		start_ut = end_ut;
		end_ut = isfinite(next_soi) && next_soi > 0 ? current_ut + next_soi : INFINITY;
		current = next;
	}
	return 0;
}
int ksp_session_add_next_maneuver_node(struct ItinStep *route, double dep_periapsis,
		double arr_periapsis, enum LastTransferType transfer_type, char *message, size_t message_size) {
	if(message != NULL && message_size > 0) message[0] = 0;
	krpc_connection_t connection;
	CelestSystem *system = NULL;
	KspVesselStart start;
	if(!ksp_session_get_connection(&connection, &system)) {
		set_message(message, message_size, "Connect to KSP first.");
		return 0;
	}
	// Only the departure burn needs the connect-time parking orbit; later burns work from the live trajectory.
	char start_error[256] = {0};
	if(!ksp_session_get_vessel_start(&start, start_error, sizeof(start_error))) {
		memset(&start, 0, sizeof(start));
		if(start_error[0] == 0) snprintf(start_error, sizeof(start_error), "No usable vessel orbit was loaded when KSP connected.");
	}
	if(route == NULL) {
		set_message(message, message_size, "Load or calculate an itinerary in the Transfer Planner first.");
		return 0;
	}
	krpc_SpaceCenter_Vessel_t vessel;
	krpc_SpaceCenter_Orbit_t orbit;
	krpc_SpaceCenter_Control_t control;
	krpc_SpaceCenter_CelestialBody_t orbit_body;
	krpc_SpaceCenter_ReferenceFrame_t frame;
	char *orbit_body_name = NULL;
	double current_ut, soi_time;
	if(!rpc_ok(krpc_SpaceCenter_ActiveVessel(connection, &vessel), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_UT(connection, &current_ut), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Vessel_Orbit(connection, &orbit, vessel), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Orbit_Body(connection, &orbit_body, orbit), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &orbit_body_name, orbit_body), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Vessel_Control(connection, &control, vessel), message, message_size) ||
		!rpc_ok(krpc_SpaceCenter_Orbit_TimeToSOIChange(connection, &soi_time, orbit), message, message_size)) goto fail;
	if(!rpc_ok(krpc_SpaceCenter_CelestialBody_NonRotatingReferenceFrame(connection, &frame, orbit_body), message, message_size)) goto fail;
	krpc_list_object_t nodes = KRPC_NULL_LIST;
	if(!rpc_ok(krpc_SpaceCenter_Control_Nodes(connection, &nodes, control), message, message_size)) goto fail;
	double after_date = current_ut / 86400.0 + system->ut0;
	double late_floor = -INFINITY;
	for(int attempt = 0; attempt < 64; attempt++) {
		KspNodeCandidate candidate;
		int candidate_result = late_flyby_candidate(connection, system, route, orbit, current_ut,
			soi_time, after_date, late_floor, arr_periapsis, transfer_type, &candidate, message, message_size);
		if(candidate_result < 0) {
			KRPC_FREE_LIST(nodes);
			goto fail;
		}
		if(candidate_result == 0) candidate_result = next_candidate(route, system, &start, dep_periapsis, arr_periapsis,
			transfer_type, after_date, start_error, &candidate, message, message_size);
		if(candidate_result <= 0) {
			KRPC_FREE_LIST(nodes);
			goto fail;
		}
		double node_ut = (candidate.date - system->ut0) * 86400.0;
		int existing_index = -1;
		for(size_t i = 0; i < nodes.size; i++) {
			double existing_ut;
			if(!rpc_ok(krpc_SpaceCenter_Node_UT(connection, &existing_ut, nodes.items[i]), message, message_size)) {
				KRPC_FREE_LIST(nodes);
				goto fail;
			}
			if(fabs(existing_ut - node_ut) < 0.5) { existing_index = (int)i; break; }
		}
		// The node may lie on a later predicted patch, such as an encounter after the current heliocentric arc.
		krpc_SpaceCenter_Orbit_t patch_orbit;
		krpc_SpaceCenter_ReferenceFrame_t patch_frame;
		int patch_result = node_ut < current_ut + 0.1 ? 0 : find_patch(connection, orbit, current_ut, soi_time,
			candidate.frame_body->name, node_ut, &patch_orbit, &patch_frame, message, message_size);
		if(patch_result < 0) {
			KRPC_FREE_LIST(nodes);
			goto fail;
		}
		// A burn that is in the past or no longer on a predicted patch has already been flown.
		if(patch_result == 0 && (existing_index >= 0 || candidate.is_departure || node_ut < current_ut + 0.1)) {
			late_floor = fmax(late_floor, candidate.step_date);
			after_date = fmax(after_date, candidate.step_date + 1e-8);
			continue;
		}
		if(patch_result == 0) {
			KRPC_FREE_LIST(nodes);
			if(message != NULL && message_size > 0)
				snprintf(message, message_size, "Next node is around %s, but no predicted KSP patch around %s covers UT %.0f (the vessel is on the %s patch). Add earlier nodes first so the encounter is predicted.",
					candidate.frame_body->name, candidate.frame_body->name, node_ut, orbit_body_name);
			goto fail;
		}
		Vector3 position, velocity;
		if(!orbit_state_at(connection, patch_orbit, patch_frame, node_ut, &position, &velocity, message, message_size)) {
			KRPC_FREE_LIST(nodes);
			goto fail;
		}		krpc_SpaceCenter_CelestialBody_t system_root;
		krpc_SpaceCenter_ReferenceFrame_t system_frame;
		// KMAT vectors are right-handed with z normal to the reference plane; kRPC frames are left-handed with y up.
		krpc_tuple_double_double_double_t system_delta_v = {
			.e0 = candidate.delta_v.x, .e1 = candidate.delta_v.z, .e2 = candidate.delta_v.y
		};
		krpc_tuple_double_double_double_t body_delta_v;
		if(!find_remote_body(connection, system->cb->name, &system_root, message, message_size) ||
			!rpc_ok(krpc_SpaceCenter_CelestialBody_NonRotatingReferenceFrame(connection, &system_frame, system_root), message, message_size) ||
			!rpc_ok(krpc_SpaceCenter_TransformDirection(connection, &body_delta_v, &system_delta_v, system_frame, patch_frame), message, message_size)) {
			KRPC_FREE_LIST(nodes);
			goto fail;
		}
		Vector3 node_components;
		Vector3 body_dv = vec3(body_delta_v.e0, body_delta_v.e2, body_delta_v.e1);
		if(!ksp_project_delta_v_to_node(vec3(position.x, position.z, position.y), vec3(velocity.x, velocity.z, velocity.y),
			body_dv, &node_components)) {
			KRPC_FREE_LIST(nodes);
			set_message(message, message_size, "Could not determine the vessel's orbital node frame.");
			goto fail;
		}
		float prograde_dv = (float)node_components.x;
		float normal_dv = (float)node_components.y;
		float radial_dv = (float)node_components.z;
		krpc_SpaceCenter_Node_t node;
		if(existing_index >= 0) {
			double old_prograde, old_normal, old_radial;
			if(!rpc_ok(krpc_SpaceCenter_Node_Prograde(connection, &old_prograde, nodes.items[existing_index]), message, message_size) ||
				!rpc_ok(krpc_SpaceCenter_Node_Normal(connection, &old_normal, nodes.items[existing_index]), message, message_size) ||
				!rpc_ok(krpc_SpaceCenter_Node_Radial(connection, &old_radial, nodes.items[existing_index]), message, message_size)) {
				KRPC_FREE_LIST(nodes);
				goto fail;
			}
			if(mag_vec3(vec3(old_prograde - prograde_dv, old_normal - normal_dv, old_radial - radial_dv)) < 0.01) {
				late_floor = fmax(late_floor, candidate.step_date);
				after_date = fmax(after_date, candidate.step_date + 1e-8);
				continue;
			}
			if(!rpc_ok(krpc_SpaceCenter_Node_Remove(connection, nodes.items[existing_index]), message, message_size)) {
				KRPC_FREE_LIST(nodes);
				goto fail;
			}
		}
		KRPC_FREE_LIST(nodes);
		if(!rpc_ok(krpc_SpaceCenter_Control_AddNode(connection, &node, control, node_ut,
			prograde_dv, normal_dv, radial_dv), message, message_size)) goto fail;
		krpc_free(orbit_body_name);
		if(candidate.is_departure || candidate.flyby_step != NULL)
			refine_node(connection, system, route, node, &start, &candidate, patch_frame, system_frame,
				position, velocity, arr_periapsis);
		if(message != NULL && message_size > 0) {
			double final_dv = mag_vec3(candidate.delta_v);
			if(!rpc_ok(krpc_SpaceCenter_Node_DeltaV(connection, &final_dv, node), NULL, 0)) final_dv = mag_vec3(candidate.delta_v);
			int length = snprintf(message, message_size, "Added maneuver node at UT %.1f (%.1f m/s, planned %.1f m/s).", node_ut,
				final_dv, mag_vec3(candidate.delta_v));
			if(length > 0 && (size_t)length < message_size)
				describe_predicted_path(connection, node, system, route, arr_periapsis, message + length, message_size - length);
		}
		return 1;
	}
	KRPC_FREE_LIST(nodes);
	set_message(message, message_size, "No remaining maneuver node could be added from the current KSP patch.");
	goto fail;

fail:
	if(orbit_body_name != NULL) krpc_free(orbit_body_name);
	return 0;
}
