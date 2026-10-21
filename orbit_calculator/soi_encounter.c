#include "soi_encounter.h"

#include <float.h>
#include <math.h>

#define SOI_MIN_INTERVAL_SECONDS 0.1
#define SOI_MAX_RECURSION_DEPTH 48

typedef struct SoiSample {
	OSV spacecraft;
	OSV body;
	double distance;
} SoiSample;

static int relative_body_state_at(CelestSystem *system, Body *body, double jd, OSV *state) {
	if(system->prop_method == ORB_ELEMENTS || body->ephem == NULL || body->num_ephems <= 0) {
		if(body->orbit.cb != system->cb || !isfinite(body->orbit.a) || body->orbit.a == 0) return 0;
		*state = osv_from_elements(body->orbit, jd);
	} else {
		*state = osv_from_ephem(body->ephem, body->num_ephems, jd, system->cb);
	}
	return isfinite(state->r.x) && isfinite(state->r.y) && isfinite(state->r.z) &&
		isfinite(state->v.x) && isfinite(state->v.y) && isfinite(state->v.z);
}

static int body_global_state_at(CelestSystem *system, OSV parent_state, double jd,
		Body *target, OSV *state) {
	for(int i = 0; i < system->num_bodies; i++) {
		Body *body = system->bodies[i];
		if(body == NULL) return 0;
		OSV relative, global;
		if(relative_body_state_at(system, body, jd, &relative)) {
			global = (OSV){add_vec3(parent_state.r, relative.r), add_vec3(parent_state.v, relative.v)};
			if(body == target) {
				*state = global;
				return 1;
			}
			if(body->system != NULL && body_global_state_at(body->system, global, jd, target, state))
				return 1;
		}
	}
	return 0;
}

static int sample_coast(CelestSystem *system, OSV initial, double start_jd, double elapsed_seconds,
		Body *body, SoiSample *sample) {
	sample->spacecraft = propagate_osv_time(initial, system->cb, elapsed_seconds);
	OSV body_state;
	if(!body_global_state_at(system, (OSV){0}, start_jd + elapsed_seconds / 86400, body, &body_state) ||
		!isfinite(sample->spacecraft.r.x) || !isfinite(sample->spacecraft.r.y) ||
		!isfinite(sample->spacecraft.r.z) || !isfinite(sample->spacecraft.v.x) ||
		!isfinite(sample->spacecraft.v.y) || !isfinite(sample->spacecraft.v.z)) return 0;
	sample->body = body_state;
	sample->distance = mag_vec3(subtract_vec3(sample->spacecraft.r, body_state.r));
	return isfinite(sample->distance);
}

static double speed_bound(Vector3 position0, Vector3 position1, Vector3 position2,
		Vector3 velocity0, Vector3 velocity1, Vector3 velocity2, double mu, double duration) {
	double speed = fmax(mag_vec3(velocity0), fmax(mag_vec3(velocity1), mag_vec3(velocity2)));
	double minimum_radius = fmin(mag_vec3(position0),
		fmin(mag_vec3(position1), mag_vec3(position2)));
	for(int i = 0; i < 8; i++) {
		double lower_radius = minimum_radius - speed * duration / 4;
		if(!(lower_radius > 0)) return HUGE_VAL;
		double acceleration = mu / (lower_radius * lower_radius);
		double updated = fmax(mag_vec3(velocity0),
			fmax(mag_vec3(velocity1), mag_vec3(velocity2))) + acceleration * duration / 4;
		if(!isfinite(updated)) return HUGE_VAL;
		if(updated <= speed * (1 + 1e-12) + 1e-9) return updated;
		speed = updated;
	}
	return HUGE_VAL;
}

static int interval_crosses_soi(CelestSystem *system, OSV initial, double start_jd,
		Body *body, double soi_radius, double start_seconds, double end_seconds,
		const SoiSample *left, const SoiSample *right, int depth) {
	if(left->distance <= soi_radius || right->distance <= soi_radius) return 1;
	double duration = end_seconds - start_seconds;
	if(!(duration > 0) || !isfinite(duration)) return 1;

	double middle_seconds = start_seconds + duration / 2;
	SoiSample middle;
	if(!sample_coast(system, initial, start_jd, middle_seconds, body, &middle)) return 1;
	if(middle.distance <= soi_radius) return 1;

	double spacecraft_speed = speed_bound(left->spacecraft.r, middle.spacecraft.r, right->spacecraft.r,
		left->spacecraft.v, middle.spacecraft.v, right->spacecraft.v, system->cb->mu, duration);
	double body_speed = speed_bound(left->body.r, middle.body.r, right->body.r,
		left->body.v, middle.body.v, right->body.v, system->cb->mu, duration);
	double minimum_sample_distance = fmin(left->distance, fmin(middle.distance, right->distance));
	double relative_speed_bound = spacecraft_speed + body_speed;
	double distance_lower_bound = minimum_sample_distance - relative_speed_bound * duration / 4;
	if(isfinite(distance_lower_bound) && distance_lower_bound > soi_radius) return 0;

	if(duration <= SOI_MIN_INTERVAL_SECONDS || depth >= SOI_MAX_RECURSION_DEPTH) return 1;
	int left_crosses = interval_crosses_soi(system, initial, start_jd, body, soi_radius,
		start_seconds, middle_seconds, left, &middle, depth + 1);
	if(left_crosses) return 1;
	return interval_crosses_soi(system, initial, start_jd, body, soi_radius,
		middle_seconds, end_seconds, &middle, right, depth + 1);
}

static int coast_crosses_body_soi(CelestSystem *system, Body *departure_body, Body *arrival_body,
		OSV departure_state, double departure_jd, double arrival_jd, Body *body) {
	if(body == departure_body || body == arrival_body) return 0;
	Body *parent = body->orbit.cb;
	if(!(body->mu > 0) || parent == NULL || !(parent->mu > 0)) return 0;
	double semi_major_axis = body->orbit.a;
	if(!isfinite(semi_major_axis) || semi_major_axis == 0) {
		OSV state = {0}, parent_state = {0};
		if(!body_global_state_at(system, (OSV){0}, departure_jd, body, &state) ||
			(parent != system->cb && !body_global_state_at(system, (OSV){0}, departure_jd, parent, &parent_state)))
			return 1;
		Vector3 relative_position = subtract_vec3(state.r, parent_state.r);
		Vector3 relative_velocity = subtract_vec3(state.v, parent_state.v);
		double radius = mag_vec3(relative_position), speed_squared = sq_mag_vec3(relative_velocity);
		double specific_energy = 0.5 * speed_squared - parent->mu / radius;
		if(!(radius > 0) || !(specific_energy < 0)) return 1;
		semi_major_axis = -parent->mu / (2 * specific_energy);
	}
	double soi_radius = fabs(semi_major_axis) * pow(body->mu / parent->mu, 0.4);
	if(!(soi_radius > 0) || !isfinite(soi_radius)) return 0;

	double duration = (arrival_jd - departure_jd) * 86400;
	if(!(duration > 0) || !isfinite(duration)) return 1;
	SoiSample start, end;
	if(!sample_coast(system, departure_state, departure_jd, 0, body, &start) ||
		!sample_coast(system, departure_state, departure_jd, duration, body, &end)) return 1;
	return interval_crosses_soi(system, departure_state, departure_jd, body, soi_radius,
		0, duration, &start, &end, 0);
}

static int system_tree_crosses_soi(CelestSystem *system, CelestSystem *root, Body *departure_body,
		Body *arrival_body, OSV departure_state, double departure_jd, double arrival_jd) {
	for(int i = 0; i < system->num_bodies; i++) {
		Body *body = system->bodies[i];
		if(body == NULL) return 1;
		if(coast_crosses_body_soi(root, departure_body, arrival_body, departure_state,
			departure_jd, arrival_jd, body)) return 1;
		if(body->system != NULL && system_tree_crosses_soi(body->system, root,
			departure_body, arrival_body, departure_state, departure_jd, arrival_jd)) return 1;
	}
	return 0;
}

int coast_leg_crosses_unmodeled_soi(CelestSystem *system, Body *departure_body, Body *arrival_body,
		OSV departure_state, double departure_jd, double arrival_jd) {
	if(system == NULL || system->cb == NULL || system->bodies == NULL || system->cb->mu <= 0 ||
		!isfinite(departure_jd) || !isfinite(arrival_jd) || arrival_jd <= departure_jd ||
		!isfinite(departure_state.r.x) || !isfinite(departure_state.r.y) ||
		!isfinite(departure_state.r.z) || !isfinite(departure_state.v.x) ||
		!isfinite(departure_state.v.y) || !isfinite(departure_state.v.z)) return 1;
	return system_tree_crosses_soi(system, system, departure_body, arrival_body,
		departure_state, departure_jd, arrival_jd);
}
