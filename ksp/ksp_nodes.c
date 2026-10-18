#include "ksp_nodes.h"
#include "ksp_session.h"
#include "orbit_calculator/flyby_policy.h"
#include "orbit_calculator/moon_flyby.h"
#include "tools/starting_orbit.h"
#include <krpc_cnano/memory.h>
#include <krpc_cnano/services/space_center.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static void set_message(char *message, size_t message_size, const char *text) {
	if(message != NULL && message_size > 0) snprintf(message, message_size, "%s", text);
}

static int rpc_ok(krpc_error_t result, char *message, size_t message_size) {
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

static int find_remote_body(krpc_connection_t connection, const char *name,
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

static int orbit_state_at(krpc_connection_t connection, krpc_SpaceCenter_Orbit_t orbit,
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

// Outgoing asymptote direction and periapsis radius of the hyperbola through a body-relative state.
static int outgoing_asymptote(double mu, Vector3 r, Vector3 v, Vector3 *direction, double *periapsis) {
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
static int burn_to_asymptote(double mu, Vector3 r, Vector3 v_old, Vector3 v_infinity, double min_periapsis,
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
		errors[i] = outgoing_asymptote(mu, r, v, &direction, &periapsis) ? acos(fmax(-1, fmin(1, dot_vec3(direction, u)))) : INFINITY;
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
static Vector3 planned_aim_offset(struct ItinStep *target, double arr_periapsis) {
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
	// Impact parameter of the hyperbola with this periapsis, offset toward the side the periapsis lies on.
	Vector3 perpendicular = subtract_vec3(direction, scale_vec3(incoming, dot_vec3(direction, incoming)));
	if(!(mag_vec3(perpendicular) > 1e-9) || !(periapsis_radius > 0)) return vec3(0, 0, 0);
	double impact = periapsis_radius * sqrt(1 + 2 * target->body->mu / (periapsis_radius * speed * speed));
	return scale_vec3(norm_vec3(perpendicular), impact);
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
// Offset of the incoming asymptote from the target body's centre, from the periapsis KSP predicts on the node's path.
static int predicted_encounter_offset(krpc_connection_t connection, krpc_SpaceCenter_Node_t node,
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

// The itinerary assumes the vessel leaves the departure body's centre at the planned date, but the real escape takes hours
// and the planet moves meanwhile. This re-aims the departure node at the heliocentric state KSP predicts at the sphere-of-
// influence exit, so the leg reaches the next planned encounter.
static void refine_node(krpc_connection_t connection, CelestSystem *system, struct ItinStep *route,
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
		// Start from the excess velocity the planned burn gives, then correct it.
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
			// The burn reaches the aim point; KSP's predicted periapsis may still differ because its patched conics
			// see the approach from the sphere-of-influence entry, so shift the aim point by the remaining miss.
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
static void describe_predicted_path(krpc_connection_t connection, krpc_SpaceCenter_Node_t node, CelestSystem *system,
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
		// The first patch is the one the node sits on, and the central body's patch has no periapsis worth reporting.
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
