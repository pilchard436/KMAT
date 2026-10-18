#include "ksp_node_internal.h"
#include "ksp_nodes.h"
#include "orbit_calculator/flyby_policy.h"
#include "tools/starting_orbit.h"
#include <krpc_cnano/memory.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Outgoing asymptote direction and periapsis radius of the hyperbola through a body-relative state.
int outgoing_asymptote(double mu, Vector3 r, Vector3 v, Vector3 *direction, double *periapsis) {
	double radius = mag_vec3(r), speed2 = dot_vec3(v, v);
	Vector3 eccentricity_vec = scale_vec3(subtract_vec3(scale_vec3(r, speed2 - mu / radius), scale_vec3(v, dot_vec3(r, v))), 1 / mu);
	Vector3 h = cross_vec3(r, v);
	double e = mag_vec3(eccentricity_vec), h_mag = mag_vec3(h);
	if(!(e > 1) || !(h_mag > 0) || !isfinite(e)) return 0;
	Vector3 p = scale_vec3(eccentricity_vec, 1 / e), q = cross_vec3(scale_vec3(h, 1 / h_mag), p);
	*direction = norm_vec3(add_vec3(scale_vec3(p, -sqrt(e * e - 1) / e), scale_vec3(q, e - 1 / e)));
	*periapsis = h_mag * h_mag / mu / (1 + e);
	return 1;
}

// Burns at state (r, v_old) that leave the body on an asymptote with the given excess velocity vector, cheapest first.
// The new orbit lies in the plane of r and the asymptote; the burn direction within it is found by search.
int burn_to_asymptote(double mu, Vector3 r, Vector3 v_old, Vector3 v_infinity, double min_periapsis,
		Vector3 *burns, int max_burns) {
	double radius = mag_vec3(r), excess = mag_vec3(v_infinity);
	if(!(radius > 0) || !(excess > 0)) return 0;
	Vector3 u = scale_vec3(v_infinity, 1 / excess), r_hat = scale_vec3(r, 1 / radius);
	Vector3 n = cross_vec3(r_hat, u);
	if(mag_vec3(n) < 1e-9) return 0;
	n = norm_vec3(n);
	Vector3 t_hat = cross_vec3(n, r_hat);
	double speed = sqrt(excess * excess + 2 * mu / radius), costs[8];
	int count = 0;
	enum { SAMPLES = 7200 };
	static double errors[SAMPLES];
	for(int i = 0; i < SAMPLES; i++) {
		double phi = 2 * M_PI * i / SAMPLES;
		Vector3 v = scale_vec3(add_vec3(scale_vec3(r_hat, cos(phi)), scale_vec3(t_hat, sin(phi))), speed), direction;
		double periapsis;
		errors[i] = outgoing_asymptote(mu, r, v, &direction, &periapsis) ?
			acos(fmax(-1, fmin(1, dot_vec3(direction, u)))) : INFINITY;
	}
	for(int i = 0; i < SAMPLES; i++) {
		double left = errors[(i + SAMPLES - 1) % SAMPLES], right = errors[(i + 1) % SAMPLES];
		if(!isfinite(errors[i]) || errors[i] > left || errors[i] > right) continue;
		double low = 2 * M_PI * (i - 1) / SAMPLES, high = 2 * M_PI * (i + 1) / SAMPLES, best_phi = 0;
		for(int iteration = 0; iteration < 60; iteration++) {
			double a = low + (high - low) / 3, b = high - (high - low) / 3, error_a, error_b;
			for(int k = 0; k < 2; k++) {
				double phi = k == 0 ? a : b;
				Vector3 v = scale_vec3(add_vec3(scale_vec3(r_hat, cos(phi)), scale_vec3(t_hat, sin(phi))), speed), direction;
				double periapsis, error = outgoing_asymptote(mu, r, v, &direction, &periapsis) ?
					acos(fmax(-1, fmin(1, dot_vec3(direction, u)))) : INFINITY;
				if(k == 0) error_a = error; else error_b = error;
			}
			if(error_a < error_b) high = b; else low = a;
			best_phi = (low + high) / 2;
		}
		Vector3 v = scale_vec3(add_vec3(scale_vec3(r_hat, cos(best_phi)), scale_vec3(t_hat, sin(best_phi))), speed), direction;
		double periapsis;
		if(!outgoing_asymptote(mu, r, v, &direction, &periapsis)) continue;
		double error = acos(fmax(-1, fmin(1, dot_vec3(direction, u))));
		Vector3 burn = subtract_vec3(v, v_old);
		double cost = mag_vec3(burn);
		// Past periapsis the planet is already cleared; the integration checks the actual path for a collision.
		if(error >= 1e-6 || (dot_vec3(r, v) < 0 && periapsis < min_periapsis * 0.999)) continue;
		int slot = count < max_burns && count < 8 ? count++ : -1;
		if(slot < 0) continue;
		costs[slot] = cost;
		burns[slot] = burn;
		for(; slot > 0 && costs[slot] < costs[slot - 1]; slot--) {
			double c = costs[slot]; costs[slot] = costs[slot - 1]; costs[slot - 1] = c;
			Vector3 b = burns[slot]; burns[slot] = burns[slot - 1]; burns[slot - 1] = b;
		}
	}
	return count;
}

// The plan aims each leg at the centre of its target body and leaves the closest approach to a later correction. A real
// vessel aimed at the centre of a planet crashes into it, so the leg is aimed at the point that gives the planned periapsis.
Vector3 planned_aim_offset(struct ItinStep *target, double arr_periapsis) {
	if(target->body == NULL || target->prev == NULL) return vec3(0, 0, 0);
	Vector3 excess = subtract_vec3(target->v_arr, target->v_body);
	double speed = mag_vec3(excess), periapsis_radius;
	if(!(speed > 0) || !(target->body->mu > 0)) return vec3(0, 0, 0);
	Vector3 incoming = scale_vec3(excess, 1 / speed), direction;
	if(target->next != NULL) {
		PoweredFlybyResult flyby;
		if(!calculate_powered_flyby(target->v_arr, target->next[0]->v_dep, target->v_body, target->body,
			flyby_minimum_periapsis(target->body), &flyby)) return vec3(0, 0, 0);
		periapsis_radius = flyby.periapsis_radius;
		direction = flyby.periapsis_direction;
	} else {
		periapsis_radius = alt2radius(target->body, arr_periapsis);
		direction = cross_vec3(vec3(0, 0, 1), incoming);
		if(mag_vec3(direction) < 1e-6) direction = vec3(1, 0, 0);
	}
	Vector3 perpendicular = subtract_vec3(direction, scale_vec3(incoming, dot_vec3(direction, incoming)));
	if(!(mag_vec3(perpendicular) > 1e-9) || !(periapsis_radius > 0)) return vec3(0, 0, 0);
	double impact = periapsis_radius * sqrt(1 + 2 * target->body->mu / (periapsis_radius * speed * speed));
	return scale_vec3(norm_vec3(perpendicular), impact);
}

// Offset of the incoming asymptote from the target body's centre, from the periapsis KSP predicts on the node's path.
int predicted_encounter_offset(krpc_connection_t connection, krpc_SpaceCenter_Node_t node,
		struct ItinStep *target, Vector3 *offset) {
	Body *body = target->body;
	krpc_SpaceCenter_Orbit_t patch;
	if(body == NULL || !(body->mu > 0) || !rpc_ok(krpc_SpaceCenter_Node_Orbit(connection, &patch, node), NULL, 0)) return 0;
	Vector3 excess = subtract_vec3(target->v_arr, target->v_body);
	double speed = mag_vec3(excess);
	if(!(speed > 0)) return 0;
	for(int i = 0; i < 8; i++) {
		krpc_SpaceCenter_CelestialBody_t remote_body;
		char *name = NULL;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_Body(connection, &remote_body, patch), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &name, remote_body), NULL, 0)) return 0;
		int match = i > 0 && strcmp(name, body->name) == 0;
		krpc_free(name);
		if(match) {
			krpc_SpaceCenter_ReferenceFrame_t frame;
			krpc_tuple_double_double_double_t at;
			double pe_ut;
			if(!rpc_ok(krpc_SpaceCenter_CelestialBody_NonRotatingReferenceFrame(connection, &frame, remote_body), NULL, 0) ||
				!rpc_ok(krpc_SpaceCenter_Orbit_UTAtTrueAnomaly(connection, &pe_ut, patch, 0), NULL, 0) || !isfinite(pe_ut) ||
				!rpc_ok(krpc_SpaceCenter_Orbit_PositionAt(connection, &at, patch, pe_ut, frame), NULL, 0)) return 0;
			Vector3 periapsis = vec3(at.e0, at.e2, at.e1), incoming = scale_vec3(excess, 1 / speed);
			double radius = mag_vec3(periapsis);
			Vector3 perpendicular = subtract_vec3(periapsis, scale_vec3(incoming, dot_vec3(periapsis, incoming)));
			if(!(radius > 0) || !(mag_vec3(perpendicular) > 0)) return 0;
			double impact = radius * sqrt(1 + 2 * body->mu / (radius * speed * speed));
			*offset = scale_vec3(norm_vec3(perpendicular), impact);
			return 1;
		}
		krpc_SpaceCenter_Orbit_t next = KRPC_NULL;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_NextOrbit(connection, &next, patch), NULL, 0) || next == KRPC_NULL) return 0;
		patch = next;
	}
	return 0;
}

// Re-aims departure and flyby nodes against the next encounter predicted by KSP.
void refine_node(krpc_connection_t connection, CelestSystem *system, struct ItinStep *route,
		krpc_SpaceCenter_Node_t node, const KspVesselStart *start, const KspNodeCandidate *candidate,
		krpc_SpaceCenter_ReferenceFrame_t patch_frame, krpc_SpaceCenter_ReferenceFrame_t system_frame,
		Vector3 position, Vector3 velocity, double arr_periapsis) {
	struct ItinStep *leaving = candidate->is_departure ? get_first(route) : candidate->flyby_step;
	if(leaving == NULL || leaving->next == NULL) return;
	struct ItinStep *following = leaving->next[0];
	Vector3 base_offset = planned_aim_offset(following, arr_periapsis), correction = vec3(0, 0, 0);
	Vector3 aim_point = add_vec3(following->r, base_offset);
	int outer_corrections = 0;
	StartingOrbit orbit_at_departure = start->orbit;
	orbit_at_departure.true_anomaly = candidate->departure_anomaly;
	Vector3 request = candidate->excess, current_burn = candidate->delta_v;
	Body *planet = leaving->body;
	if(!candidate->is_departure) {
		Vector3 old_velocity = vec3(velocity.x, velocity.z, velocity.y), direction, new_velocity;
		double periapsis;
		new_velocity = add_vec3(old_velocity, current_burn);
		double excess_speed2 = dot_vec3(new_velocity, new_velocity) - 2 * planet->mu / mag_vec3(vec3(position.x, position.z, position.y));
		if(!outgoing_asymptote(planet->mu, vec3(position.x, position.z, position.y), new_velocity, &direction, &periapsis) ||
			!(excess_speed2 > 0)) return;
		request = scale_vec3(direction, sqrt(excess_speed2));
	}
	for(int iteration = 0; iteration < 24; iteration++) {
		krpc_SpaceCenter_Orbit_t patch, next = KRPC_NULL;
		double now_ut, time_to_exit;
		if(!rpc_ok(krpc_SpaceCenter_Node_Orbit(connection, &patch, node), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_UT(connection, &now_ut), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_Orbit_TimeToSOIChange(connection, &time_to_exit, patch), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_Orbit_NextOrbit(connection, &next, patch), NULL, 0) || next == KRPC_NULL ||
			!isfinite(time_to_exit)) return;
		double exit_ut = now_ut + time_to_exit + 1;
		krpc_SpaceCenter_CelestialBody_t next_body;
		char *next_name = NULL;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_Body(connection, &next_body, next), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &next_name, next_body), NULL, 0)) return;
		int leaves_to_central = strcmp(next_name, system->cb->name) == 0;
		krpc_free(next_name);
		if(!leaves_to_central) return;
		Vector3 exit_position, exit_velocity;
		if(!orbit_state_at(connection, next, system_frame, exit_ut, &exit_position, &exit_velocity, NULL, 0)) return;
		exit_position = vec3(exit_position.x, exit_position.z, exit_position.y);
		exit_velocity = vec3(exit_velocity.x, exit_velocity.z, exit_velocity.y);
		double leg_seconds = (following->date - (exit_ut / 86400.0 + system->ut0)) * 86400.0, closest = INFINITY;
		if(leg_seconds <= 3600) return;
		Vector3 v_leg = vec3(0, 0, 0);
		for(int revolutions = 0; revolutions <= 6; revolutions++) {
			for(int branch = 0; branch < (revolutions == 0 ? 1 : 2); branch++) {
				Lambert3 leg = calc_lambert3_revs(exit_position, aim_point, leg_seconds, system->cb, revolutions, branch);
				if(leg.success != LAMBERT_SUCCESS || !isfinite(leg.v0.x + leg.v0.y + leg.v0.z)) continue;
				double difference = mag_vec3(subtract_vec3(leg.v0, following->v_dep));
				if(difference < closest) { closest = difference; v_leg = leg.v0; }
			}
		}
		if(!isfinite(closest)) return;
		Vector3 error = subtract_vec3(v_leg, exit_velocity);
		if(mag_vec3(error) < 0.005) {
			Vector3 predicted;
			if(following->body == NULL || !predicted_encounter_offset(connection, node, following, &predicted)) return;
			Vector3 wanted = base_offset;
			if(following->next == NULL && mag_vec3(predicted) > 0) wanted = scale_vec3(norm_vec3(predicted), mag_vec3(base_offset));
			Vector3 miss = subtract_vec3(wanted, predicted);
			if(mag_vec3(miss) < 2000 || ++outer_corrections > 6) return;
			correction = add_vec3(correction, miss);
			aim_point = add_vec3(following->r, add_vec3(base_offset, correction));
			continue;
		}
		request = add_vec3(request, error);
		double magnitude;
		Vector3 burn;
		if(candidate->is_departure) {
			if(!starting_orbit_departure_burn_at(&orbit_at_departure, start->orbiting_body, candidate->departure_anomaly,
				request, &magnitude, &burn)) return;
		} else {
			Vector3 burns[4], planet_position = vec3(position.x, position.z, position.y),
				planet_velocity = vec3(velocity.x, velocity.z, velocity.y);
			int count = burn_to_asymptote(planet->mu, planet_position, planet_velocity, request,
				flyby_minimum_periapsis(planet), burns, 4), best = -1;
			double best_gap = INFINITY;
			for(int i = 0; i < count; i++) {
				double gap = mag_vec3(subtract_vec3(burns[i], current_burn));
				if(gap < best_gap) { best_gap = gap; best = i; }
			}
			if(best < 0) return;
			burn = burns[best];
			current_burn = burn;
		}
		krpc_tuple_double_double_double_t system_delta_v = {.e0 = burn.x, .e1 = burn.z, .e2 = burn.y}, body_delta_v;
		Vector3 components;
		if(!rpc_ok(krpc_SpaceCenter_TransformDirection(connection, &body_delta_v, &system_delta_v, system_frame, patch_frame), NULL, 0) ||
			!ksp_project_delta_v_to_node(vec3(position.x, position.z, position.y), vec3(velocity.x, velocity.z, velocity.y),
				vec3(body_delta_v.e0, body_delta_v.e2, body_delta_v.e1), &components)) return;
		if(!rpc_ok(krpc_SpaceCenter_Node_set_Prograde(connection, node, components.x), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_Node_set_Normal(connection, node, components.y), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_Node_set_Radial(connection, node, components.z), NULL, 0)) return;
	}
}

// Lists the encounters KSP predicts after the new node, with periapsis altitudes, so the aim can be checked.
void describe_predicted_path(krpc_connection_t connection, krpc_SpaceCenter_Node_t node, CelestSystem *system,
		struct ItinStep *route, double arr_periapsis, char *text, size_t text_size) {
	struct ItinStep *last = get_first(route);
	while(last != NULL && last->next != NULL) last = last->next[0];
	krpc_SpaceCenter_Orbit_t patch;
	if(text_size == 0 || !rpc_ok(krpc_SpaceCenter_Node_Orbit(connection, &patch, node), NULL, 0)) return;
	size_t used = 0;
	char moon_name[64] = {0};
	for(int i = 0; i < 6; i++) {
		krpc_SpaceCenter_CelestialBody_t body;
		char *name = NULL;
		double altitude;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_Body(connection, &body, patch), NULL, 0) ||
			!rpc_ok(krpc_SpaceCenter_CelestialBody_Name(connection, &name, body), NULL, 0)) break;
		if(i > 0 && strcmp(name, system->cb->name) != 0 &&
			rpc_ok(krpc_SpaceCenter_Orbit_PeriapsisAltitude(connection, &altitude, patch), NULL, 0)) {
			int planned = last != NULL && last->body != NULL && strcmp(last->body->name, name) == 0;
			int on_route = 0;
			for(struct ItinStep *step = get_first(route); step != NULL; step = step->next != NULL ? step->next[0] : NULL)
				if(step->body != NULL && strcmp(step->body->name, name) == 0) on_route = 1;
			if(!on_route && moon_name[0] == 0) snprintf(moon_name, sizeof(moon_name), "%s", name);
			int written = snprintf(text + used, text_size - used, "\nPredicted %s periapsis altitude: %.0f km", name, altitude / 1e3);
			if(written > 0) used += (size_t)written < text_size - used ? (size_t)written : text_size - used - 1;
			if(planned && used < text_size) {
				written = snprintf(text + used, text_size - used, " (planned %.0f km)", arr_periapsis / 1e3);
				if(written > 0) used += (size_t)written < text_size - used ? (size_t)written : text_size - used - 1;
			}
		}
		krpc_free(name);
		krpc_SpaceCenter_Orbit_t next = KRPC_NULL;
		if(!rpc_ok(krpc_SpaceCenter_Orbit_NextOrbit(connection, &next, patch), NULL, 0) || next == KRPC_NULL) break;
		patch = next;
	}
	if(moon_name[0] != 0 && used < text_size)
		snprintf(text + used, text_size - used, "\nNote: the path passes %s, whose gravity changes the later periapsis, so the planned periapsis may not be reachable.", moon_name);
}
