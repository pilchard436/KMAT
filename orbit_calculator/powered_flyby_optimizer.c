#include "powered_flyby_optimizer.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdint.h>
#include "tools/thread_pool.h"

#define POWERED_MIN_LEG_DAYS 10.0
#define POWERED_IMPROVEMENT_THRESHOLD 0.1
#define POWERED_TIMING_START_COUNT 3
#define POWERED_MULTI_START_ROUTE_LIMIT 200
#define POWERED_OBJECTIVES_PER_ROUTE 96
#define POWERED_REPAIR_OBJECTIVES_PER_ROUTE 144
#define POWERED_REPAIR_DATE_BINS 64
#define POWERED_REFINE_PROGRESS_WEIGHT 10
#define AUDIT_SAMPLE_SIZE POWERED_AUDIT_SAMPLE_SIZE
#define AUDIT_OBJECTIVES_PER_ROUTE POWERED_AUDIT_OBJECTIVES_PER_ROUTE

static _Thread_local int rejection_audit_enabled;
static _Thread_local PoweredRejectionAudit rejection_audit;

void set_powered_rejection_audit_enabled(int enabled) { rejection_audit_enabled = enabled != 0; }
PoweredRejectionAudit get_powered_rejection_audit(void) { return rejection_audit; }

static _Thread_local int objective_budget = -1;
static _Thread_local unsigned long long objective_calls;

static double wall_seconds(void) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return now.tv_sec + now.tv_usec / 1e6;
}

static int itinerary_search_cancelled(void *context) {
	(void)context;
	return get_thread_counter(3) > 0;
}

typedef struct PoweredRouteCandidate {
	int departure_index;
	size_t num_steps;
	size_t mutable_start;
	Body **bodies;
	double *signature_dates;
	Vector3 *signature_positions;
	Vector3 *signature_v_arr;
	Vector3 *signature_v_dep;
	double *times;
	OSV *best_states;
	Lambert3 *best_legs;
	double original_total_dv;
	double best_total_dv;
} PoweredRouteCandidate;

typedef struct AuditReservoir {
	PoweredRouteCandidate *samples[AUDIT_SAMPLE_SIZE];
	unsigned long long population;
	uint64_t random;
	int count;
} AuditReservoir;

typedef struct RepairPool {
	PoweredRouteCandidate *slots[POWERED_REPAIR_SLOTS];
	unsigned long long population[POWERED_REPAIR_SLOTS];
	unsigned long long total;
	uint64_t random;
} RepairPool;

typedef struct ExcludedPools {
	AuditReservoir *challenge, *audit;
} ExcludedPools;

static int is_finite_vector(Vector3 value) {
	return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static double arrival_delta_v(const struct Dv_Filter *filter, Body *body,
		Vector3 arrival_velocity, Vector3 body_velocity, double periapsis_altitude) {
	double vinf = mag_vec3(subtract_vec3(arrival_velocity, body_velocity));
	if(filter->last_transfer_type == TF_CAPTURE)
		return dv_capture(body, alt2radius(body, periapsis_altitude), vinf);
	if(filter->last_transfer_type == TF_CIRC)
		return dv_circ(body, alt2radius(body, periapsis_altitude), vinf);
	return 0;
}

static int body_state_at(CelestSystem *system, Body *body, double epoch, OSV *state) {
	*state = system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(body->orbit, epoch) :
		osv_from_ephem(body->ephem, body->num_ephems, epoch, system->cb);
	return is_finite_vector(state->r) && is_finite_vector(state->v);
}

static int hyperbolic_excess_velocity(OSV relative_state, Body *body, int incoming,
		Vector3 *excess_velocity) {
	double radius = mag_vec3(relative_state.r);
	Vector3 angular_momentum = cross_vec3(relative_state.r, relative_state.v);
	double angular_momentum_magnitude = mag_vec3(angular_momentum);
	if(!(radius > 0) || !(angular_momentum_magnitude > 0) || !(body->mu > 0)) return 0;
	double specific_energy = 0.5 * sq_mag_vec3(relative_state.v) - body->mu / radius;
	if(!(specific_energy > 0)) return 0;
	Vector3 eccentricity_vector = subtract_vec3(
		scale_vec3(cross_vec3(relative_state.v, angular_momentum), 1 / body->mu),
		scale_vec3(relative_state.r, 1 / radius));
	double eccentricity = mag_vec3(eccentricity_vector);
	if(!(eccentricity > 1)) return 0;
	double semi_latus_rectum = sq_mag_vec3(angular_momentum) / body->mu;
	if(!(semi_latus_rectum > 0)) return 0;

	Vector3 periapsis_direction = scale_vec3(eccentricity_vector, 1 / eccentricity);
	Vector3 orbit_normal = scale_vec3(angular_momentum, 1 / angular_momentum_magnitude);
	Vector3 transverse_direction = norm_vec3(cross_vec3(orbit_normal, periapsis_direction));
	double asymptote_anomaly = acos(-1 / eccentricity);
	double sine_anomaly = sin(asymptote_anomaly) * (incoming ? -1 : 1);
	double cosine_anomaly = -1 / eccentricity;
	double velocity_scale = sqrt(body->mu / semi_latus_rectum);
	*excess_velocity = add_vec3(
		scale_vec3(periapsis_direction, -velocity_scale * sine_anomaly),
		scale_vec3(transverse_direction, velocity_scale * (eccentricity + cosine_anomaly)));
	return is_finite_vector(*excess_velocity);
}

static double evaluate_route(const PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		const double *times, OSV *states, Lambert3 *legs) {
	if(itinerary_search_cancelled(NULL)) return DBL_MAX;
	if(objective_budget == 0) return DBL_MAX;
	if(objective_budget > 0) objective_budget--;
	objective_calls++;
	CelestSystem *system = calc_data->seq_info.to_target.system;
	double latest_arrival = fmin(calc_data->jd_max_arr, times[0] + calc_data->max_duration);
	if(times[candidate->num_steps - 1] > latest_arrival) return DBL_MAX;

	for(size_t i = 0; i < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
		Body *body = candidate->bodies[i];
		if(!body_state_at(system, body, times[i], &states[i])) return DBL_MAX;
	}
	if(itinerary_search_cancelled(NULL)) return DBL_MAX;

	for(size_t i = 0; i + 1 < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
		double duration_days = times[i + 1] - times[i];
		if(duration_days < POWERED_MIN_LEG_DAYS) return DBL_MAX;
		legs[i] = calc_lambert3_selected_cancellable(states[i].r, states[i + 1].r,
			duration_days * 86400, system->cb, itinerary_search_cancelled, NULL);
		if(itinerary_search_cancelled(NULL)) return DBL_MAX;
		if(legs[i].success == LAMBERT_FAIL_NAN || legs[i].success == LAMBERT_FAIL_ECC ||
			legs[i].success == LAMBERT_MAX_ITERATIONS || !is_finite_vector(legs[i].v0) ||
			!is_finite_vector(legs[i].v1)) return DBL_MAX;
	}

	PoweredFlybyResult *flybys = calloc(candidate->num_steps, sizeof(*flybys));
	Vector3 *incoming_excess = calloc(candidate->num_steps, sizeof(*incoming_excess));
	Vector3 *outgoing_excess = calloc(candidate->num_steps, sizeof(*outgoing_excess));
	if(flybys == NULL || incoming_excess == NULL || outgoing_excess == NULL) {
		free(flybys);
		free(incoming_excess);
		free(outgoing_excess);
		return DBL_MAX;
	}
	double total_dv = DBL_MAX;
	int valid_route = 1;
	for(size_t i = 1; i + 1 < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
		Body *body = candidate->bodies[i];
		double minimum_periapsis = body->radius + body->atmo_alt + 1e4;
		incoming_excess[i] = subtract_vec3(legs[i - 1].v1, states[i].v);
		outgoing_excess[i] = subtract_vec3(legs[i].v0, states[i].v);
		if(!calculate_powered_flyby(incoming_excess[i], outgoing_excess[i], vec3(0, 0, 0),
			body, minimum_periapsis, &flybys[i])) {
			valid_route = 0;
			break;
		}
	}
	if(itinerary_search_cancelled(NULL)) valid_route = 0;

	int converged = 0;
	for(int iteration = 0; valid_route && !itinerary_search_cancelled(NULL) && iteration < 60; iteration++) {
		for(size_t i = 0; i + 1 < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
			double departure_epoch = times[i];
			double arrival_epoch = times[i + 1];
			Vector3 departure_position = states[i].r;
			Vector3 arrival_position = states[i + 1].r;
			if(i > 0) {
				departure_epoch += flybys[i].outgoing_soi_time / 86400;
				OSV body_at_exit;
				if(!body_state_at(system, candidate->bodies[i], departure_epoch, &body_at_exit)) {
					valid_route = 0;
					break;
				}
				departure_position = add_vec3(body_at_exit.r, flybys[i].outgoing_soi_state.r);
			}
			if(i + 1 < candidate->num_steps - 1) {
				arrival_epoch -= flybys[i + 1].incoming_soi_time / 86400;
				OSV body_at_entry;
				if(!body_state_at(system, candidate->bodies[i + 1], arrival_epoch, &body_at_entry)) {
					valid_route = 0;
					break;
				}
				arrival_position = add_vec3(body_at_entry.r, flybys[i + 1].incoming_soi_state.r);
			}
			double duration_days = arrival_epoch - departure_epoch;
			if(duration_days < POWERED_MIN_LEG_DAYS) {
				valid_route = 0;
				break;
			}
			legs[i] = calc_lambert3_selected_cancellable(departure_position, arrival_position,
				duration_days * 86400, system->cb, itinerary_search_cancelled, NULL);
			if(itinerary_search_cancelled(NULL)) {
				valid_route = 0;
				break;
			}
			if(legs[i].success == LAMBERT_FAIL_NAN || legs[i].success == LAMBERT_FAIL_ECC ||
				legs[i].success == LAMBERT_MAX_ITERATIONS || !is_finite_vector(legs[i].v0) ||
				!is_finite_vector(legs[i].v1)) {
				valid_route = 0;
				break;
			}
		}
		if(!valid_route || itinerary_search_cancelled(NULL)) break;

		double maximum_time_change = 0;
		double maximum_velocity_change = 0;
		double maximum_boundary_position_change = 0;
		for(size_t i = 1; i + 1 < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
			Body *body = candidate->bodies[i];
			double entry_epoch = times[i] - flybys[i].incoming_soi_time / 86400;
			double exit_epoch = times[i] + flybys[i].outgoing_soi_time / 86400;
			OSV body_at_entry, body_at_exit;
			if(!body_state_at(system, body, entry_epoch, &body_at_entry) ||
				!body_state_at(system, body, exit_epoch, &body_at_exit)) {
				valid_route = 0;
				break;
			}
			OSV incoming_boundary = {
				subtract_vec3(legs[i - 1].r1, body_at_entry.r),
				subtract_vec3(legs[i - 1].v1, body_at_entry.v)
			};
			OSV outgoing_boundary = {
				subtract_vec3(legs[i].r0, body_at_exit.r),
				subtract_vec3(legs[i].v0, body_at_exit.v)
			};
			Vector3 new_incoming_excess, new_outgoing_excess;
			if(!hyperbolic_excess_velocity(incoming_boundary, body, 1, &new_incoming_excess) ||
				!hyperbolic_excess_velocity(outgoing_boundary, body, 0, &new_outgoing_excess)) {
				valid_route = 0;
				break;
			}
			new_incoming_excess = scale_vec3(add_vec3(incoming_excess[i], new_incoming_excess), 0.5);
			new_outgoing_excess = scale_vec3(add_vec3(outgoing_excess[i], new_outgoing_excess), 0.5);
			PoweredFlybyResult updated_flyby;
			double minimum_periapsis = body->radius + body->atmo_alt + 1e4;
			if(!calculate_powered_flyby(new_incoming_excess, new_outgoing_excess,
				vec3(0, 0, 0), body, minimum_periapsis, &updated_flyby)) {
				valid_route = 0;
				break;
			}
			double time_change = fmax(fabs(updated_flyby.incoming_soi_time - flybys[i].incoming_soi_time),
				fabs(updated_flyby.outgoing_soi_time - flybys[i].outgoing_soi_time));
			double time_scale = fmax(1, fmax(updated_flyby.incoming_soi_time,
				updated_flyby.outgoing_soi_time));
			maximum_time_change = fmax(maximum_time_change, time_change / time_scale);
			double velocity_change = fmax(mag_vec3(subtract_vec3(new_incoming_excess, incoming_excess[i])),
				mag_vec3(subtract_vec3(new_outgoing_excess, outgoing_excess[i])));
			double velocity_scale = fmax(1, fmax(mag_vec3(new_incoming_excess), mag_vec3(new_outgoing_excess)));
			double velocity_mismatch = fmax(
				mag_vec3(subtract_vec3(updated_flyby.incoming_soi_state.v, incoming_boundary.v)),
				mag_vec3(subtract_vec3(updated_flyby.outgoing_soi_state.v, outgoing_boundary.v)));
			maximum_velocity_change = fmax(maximum_velocity_change,
				fmax(velocity_change, velocity_mismatch) / velocity_scale);
			double boundary_position_change = fmax(
				mag_vec3(subtract_vec3(updated_flyby.incoming_soi_state.r, flybys[i].incoming_soi_state.r)),
				mag_vec3(subtract_vec3(updated_flyby.outgoing_soi_state.r, flybys[i].outgoing_soi_state.r)));
			double boundary_position_mismatch = fmax(
				mag_vec3(subtract_vec3(updated_flyby.incoming_soi_state.r, incoming_boundary.r)),
				mag_vec3(subtract_vec3(updated_flyby.outgoing_soi_state.r, outgoing_boundary.r)));
			maximum_boundary_position_change = fmax(maximum_boundary_position_change,
			fmax(boundary_position_change, boundary_position_mismatch) / updated_flyby.soi_radius);
			incoming_excess[i] = new_incoming_excess;
			outgoing_excess[i] = new_outgoing_excess;
			flybys[i] = updated_flyby;
		}
		if(!valid_route || itinerary_search_cancelled(NULL)) break;
		if(maximum_time_change < 1e-3 && maximum_velocity_change < 1e-3 &&
			maximum_boundary_position_change < 2e-3) {
			converged = 1;
			break;
		}
	}
	if(!valid_route || !converged || itinerary_search_cancelled(NULL)) {
		goto cleanup;
	}

	Body *departure_body = candidate->bodies[0];
	DepartureBurn3D departure_burn;
	Vector3 departure_vinf = subtract_vec3(legs[0].v0, states[0].v);
	if(!calculate_free_plane_departure_burn(departure_vinf, departure_body,
		alt2radius(departure_body, calc_data->dv_filter.dep_periapsis), &departure_burn)) {
		goto cleanup;
	}
	double departure_dv = departure_burn.burn_magnitude;
	if(departure_dv > calc_data->dv_filter.max_depdv) {
		goto cleanup;
	}

	double powered_flyby_dv = 0;
	for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
		powered_flyby_dv += flybys[i].burn_delta_v;
	}

	size_t last_leg = candidate->num_steps - 2;
	Body *arrival_body = candidate->bodies[candidate->num_steps - 1];
	double arrival_dv = arrival_delta_v(&calc_data->dv_filter, arrival_body,
		legs[last_leg].v1, states[candidate->num_steps - 1].v,
		calc_data->dv_filter.arr_periapsis);
	if(arrival_dv > calc_data->dv_filter.max_satdv) {
		goto cleanup;
	}
	total_dv = departure_dv + powered_flyby_dv + arrival_dv;
	if(total_dv > calc_data->dv_filter.max_totdv) {
		total_dv = DBL_MAX;
		goto cleanup;
	}
	for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
		legs[i - 1].v1 = add_vec3(states[i].v, incoming_excess[i]);
		legs[i].v0 = add_vec3(states[i].v, outgoing_excess[i]);
	}

cleanup:
	free(flybys);
	free(incoming_excess);
	free(outgoing_excess);
	return total_dv;
}

double evaluate_powered_flyby_route_objective(const Itin_Calc_Data *calc_data,
		Body **bodies, const double *times, size_t num_steps) {
	if(calc_data == NULL || bodies == NULL || times == NULL || num_steps < 3) return DBL_MAX;
	for(size_t i = 0; i < num_steps; i++)
		if(bodies[i] == NULL || !isfinite(times[i])) return DBL_MAX;
	OSV *states = calloc(num_steps, sizeof(*states));
	Lambert3 *legs = calloc(num_steps - 1, sizeof(*legs));
	if(states == NULL || legs == NULL) {
		free(states);
		free(legs);
		return DBL_MAX;
	}
	PoweredRouteCandidate candidate = {.num_steps = num_steps, .bodies = bodies};
	double objective = evaluate_route(&candidate, calc_data, times, states, legs);
	free(states);
	free(legs);
	return objective;
}

static int initialize_candidate(PoweredRouteCandidate *candidate, struct ItinStep *arrival,
		struct ItinStep **departures, int num_departures) {
	size_t num_steps = 0;
	for(struct ItinStep *step = arrival; step != NULL; step = step->prev) {
		if(step->body == NULL) return 0;
		num_steps++;
	}
	if(num_steps < 3) return 0;

	candidate->num_steps = num_steps;
	candidate->bodies = malloc(num_steps * sizeof(*candidate->bodies));
	candidate->signature_dates = malloc(num_steps * sizeof(*candidate->signature_dates));
	candidate->signature_positions = malloc(num_steps * sizeof(*candidate->signature_positions));
	candidate->signature_v_arr = malloc(num_steps * sizeof(*candidate->signature_v_arr));
	candidate->signature_v_dep = malloc(num_steps * sizeof(*candidate->signature_v_dep));
	candidate->times = malloc(num_steps * sizeof(*candidate->times));
	candidate->best_states = malloc(num_steps * sizeof(*candidate->best_states));
	candidate->best_legs = malloc((num_steps - 1) * sizeof(*candidate->best_legs));
	if(candidate->bodies == NULL || candidate->signature_dates == NULL ||
		candidate->signature_positions == NULL || candidate->signature_v_arr == NULL ||
		candidate->signature_v_dep == NULL || candidate->times == NULL ||
		candidate->best_states == NULL || candidate->best_legs == NULL) return 0;

	struct ItinStep *nodes[num_steps];
	struct ItinStep *step = arrival;
	for(size_t reverse_index = num_steps; reverse_index > 0; reverse_index--) {
		size_t i = reverse_index - 1;
		nodes[i] = step;
		candidate->bodies[i] = step->body;
		candidate->signature_dates[i] = step->date;
		candidate->signature_positions[i] = step->r;
		candidate->signature_v_arr[i] = step->v_arr;
		candidate->signature_v_dep[i] = step->v_dep;
		candidate->times[i] = step->date;
		step = step->prev;
	}
	struct ItinStep *root = nodes[0];
	candidate->departure_index = -1;
	for(int i = 0; i < num_departures; i++) {
		if(departures[i] == root) {
			candidate->departure_index = i;
			break;
		}
	}
	if(candidate->departure_index < 0) return 0;

	candidate->mutable_start = 1;
	for(size_t i = 0; i + 1 < num_steps; i++)
		if(nodes[i]->num_next_nodes > 1) candidate->mutable_start = i + 1;
	candidate->best_total_dv = DBL_MAX;
	return candidate->mutable_start < num_steps;
}

static void free_candidate(PoweredRouteCandidate *candidate) {
	free(candidate->bodies);
	free(candidate->signature_dates);
	free(candidate->signature_positions);
	free(candidate->signature_v_arr);
	free(candidate->signature_v_dep);
	free(candidate->times);
	free(candidate->best_states);
	free(candidate->best_legs);
	memset(candidate, 0, sizeof(*candidate));
}

static void destroy_candidate(PoweredRouteCandidate *candidate) {
	if(candidate == NULL) return;
	free_candidate(candidate);
	free(candidate);
}

static uint64_t audit_random(uint64_t *state) {
	*state ^= *state >> 12;
	*state ^= *state << 25;
	*state ^= *state >> 27;
	return *state * UINT64_C(2685821657736338717);
}

/* Algorithm R: every rejected route in this stratum has equal sampling probability. */
static void audit_retain(AuditReservoir *pool, PoweredRouteCandidate *candidate) {
	if(pool == NULL) { destroy_candidate(candidate); return; }
	pool->population++;
	unsigned long long slot;
	if(pool->count < AUDIT_SAMPLE_SIZE) slot = (unsigned long long)pool->count++;
	else {
		uint64_t limit = UINT64_MAX - UINT64_MAX % pool->population, draw;
		do { draw = audit_random(&pool->random); } while(draw >= limit);
		slot = draw % pool->population;
	}
	if(slot >= AUDIT_SAMPLE_SIZE) { destroy_candidate(candidate); return; }
	destroy_candidate(pool->samples[slot]);
	pool->samples[slot] = candidate;
}

static PoweredRouteCandidate *copy_candidate(const PoweredRouteCandidate *source) {
	PoweredRouteCandidate *copy = calloc(1, sizeof(*copy));
	if(copy == NULL) return NULL;
	size_t n = source->num_steps;
	*copy = *source;
	copy->bodies = malloc(n * sizeof(*copy->bodies));
	copy->signature_dates = malloc(n * sizeof(*copy->signature_dates));
	copy->signature_positions = malloc(n * sizeof(*copy->signature_positions));
	copy->signature_v_arr = malloc(n * sizeof(*copy->signature_v_arr));
	copy->signature_v_dep = malloc(n * sizeof(*copy->signature_v_dep));
	copy->times = malloc(n * sizeof(*copy->times));
	copy->best_states = malloc(n * sizeof(*copy->best_states));
	copy->best_legs = malloc((n - 1) * sizeof(*copy->best_legs));
	if(copy->bodies == NULL || copy->signature_dates == NULL || copy->signature_positions == NULL ||
		copy->signature_v_arr == NULL || copy->signature_v_dep == NULL || copy->times == NULL ||
		copy->best_states == NULL || copy->best_legs == NULL) {
		destroy_candidate(copy);
		return NULL;
	}
	memcpy(copy->bodies, source->bodies, n * sizeof(*copy->bodies));
	memcpy(copy->signature_dates, source->signature_dates, n * sizeof(*copy->signature_dates));
	memcpy(copy->signature_positions, source->signature_positions, n * sizeof(*copy->signature_positions));
	memcpy(copy->signature_v_arr, source->signature_v_arr, n * sizeof(*copy->signature_v_arr));
	memcpy(copy->signature_v_dep, source->signature_v_dep, n * sizeof(*copy->signature_v_dep));
	memcpy(copy->times, source->times, n * sizeof(*copy->times));
	memcpy(copy->best_states, source->best_states, n * sizeof(*copy->best_states));
	memcpy(copy->best_legs, source->best_legs, (n - 1) * sizeof(*copy->best_legs));
	return copy;
}

static void retain_excluded(ExcludedPools *pools, PoweredRouteCandidate *candidate) {
	if(pools->audit != NULL) {
		PoweredRouteCandidate *copy = copy_candidate(candidate);
		if(copy != NULL) audit_retain(pools->audit, copy);
	}
	audit_retain(pools->challenge, candidate);
}

/* Deterministic stratified sampling: one reservoir slot per departure-date/body-sequence hash. */
static void repair_pool_retain(RepairPool *pool, PoweredRouteCandidate *candidate, int num_departures) {
	uint64_t sequence_hash = 1469598103934665603ULL;
	for(size_t i = 0; i < candidate->num_steps; i++)
		for(const char *name = candidate->bodies[i]->name; *name; name++)
			sequence_hash = (sequence_hash ^ (unsigned char)*name) * 1099511628211ULL;
	uint64_t date_bin = (uint64_t)candidate->departure_index * POWERED_REPAIR_DATE_BINS / (uint64_t)num_departures;
	size_t slot = (size_t)((date_bin * 17 + sequence_hash % POWERED_REPAIR_SLOTS) % POWERED_REPAIR_SLOTS);
	pool->total++;
	unsigned long long seen = ++pool->population[slot];
	if(seen == 1 || audit_random(&pool->random) % seen == 0) {
		destroy_candidate(pool->slots[slot]);
		pool->slots[slot] = candidate;
	} else {
		destroy_candidate(candidate);
	}
}

static int refine_candidate_from_start(PoweredRouteCandidate *candidate,
		const Itin_Calc_Data *calc_data) {
	OSV *trial_states = malloc(candidate->num_steps * sizeof(*trial_states));
	Lambert3 *trial_legs = malloc((candidate->num_steps - 1) * sizeof(*trial_legs));
	if(trial_states == NULL || trial_legs == NULL) {
		free(trial_states);
		free(trial_legs);
		return 0;
	}
	if(!isfinite(candidate->best_total_dv) || candidate->best_total_dv >= DBL_MAX) {
		candidate->best_total_dv = evaluate_route(candidate, calc_data, candidate->times,
			candidate->best_states, candidate->best_legs);
	}
	if(!isfinite(candidate->best_total_dv) || candidate->best_total_dv >= DBL_MAX) {
		free(trial_states);
		free(trial_legs);
		return 0;
	}
	candidate->original_total_dv = candidate->best_total_dv;

	static const double time_steps_days[] = {180, 60, 20, 7, 2, 0.5};
	double latest_arrival = fmin(calc_data->jd_max_arr,
		candidate->times[0] + calc_data->max_duration);
	for(size_t step_index = 0; step_index < sizeof(time_steps_days) / sizeof(time_steps_days[0]) &&
		objective_budget != 0 && !itinerary_search_cancelled(NULL); step_index++) {
		for(int sweep = 0; sweep < 2 && !itinerary_search_cancelled(NULL); sweep++) {
			int improved_in_sweep = 0;
			for(size_t i = candidate->mutable_start; i < candidate->num_steps &&
				!itinerary_search_cancelled(NULL); i++) {
				double lower = candidate->times[i - 1] + POWERED_MIN_LEG_DAYS;
				double upper = latest_arrival;
				if(i + 1 < candidate->num_steps)
					upper = fmin(upper, candidate->times[i + 1] - POWERED_MIN_LEG_DAYS);
				for(int direction = -1; direction <= 1; direction += 2) {
					double old_time = candidate->times[i];
					double trial_time = old_time + direction * time_steps_days[step_index];
					if(trial_time < lower || trial_time > upper) continue;
					candidate->times[i] = trial_time;
					double trial_dv = evaluate_route(candidate, calc_data, candidate->times,
						trial_states, trial_legs);
					if(trial_dv + POWERED_IMPROVEMENT_THRESHOLD < candidate->best_total_dv) {
						candidate->best_total_dv = trial_dv;
						memcpy(candidate->best_states, trial_states,
							candidate->num_steps * sizeof(*trial_states));
						memcpy(candidate->best_legs, trial_legs,
							(candidate->num_steps - 1) * sizeof(*trial_legs));
						improved_in_sweep = 1;
					} else {
						candidate->times[i] = old_time;
					}
				}
			}
			if(!improved_in_sweep) break;
		}
	}
	free(trial_states);
	free(trial_legs);
	return !itinerary_search_cancelled(NULL);
}

static int optimize_candidate(PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		int timing_start_count) {
	size_t num_steps = candidate->num_steps;
	double *seed_times = malloc(num_steps * sizeof(*seed_times));
	double *best_times = malloc(num_steps * sizeof(*best_times));
	OSV *best_states = malloc(num_steps * sizeof(*best_states));
	Lambert3 *best_legs = malloc((num_steps - 1) * sizeof(*best_legs));
	PoweredRouteCandidate trial = *candidate;
	trial.times = malloc(num_steps * sizeof(*trial.times));
	trial.best_states = malloc(num_steps * sizeof(*trial.best_states));
	trial.best_legs = malloc((num_steps - 1) * sizeof(*trial.best_legs));
	if(seed_times == NULL || best_times == NULL || best_states == NULL || best_legs == NULL ||
		trial.times == NULL || trial.best_states == NULL || trial.best_legs == NULL) {
		free(seed_times);
		free(best_times);
		free(best_states);
		free(best_legs);
		free(trial.times);
		free(trial.best_states);
		free(trial.best_legs);
		return 0;
	}

	memcpy(seed_times, candidate->times, num_steps * sizeof(*seed_times));
	memcpy(best_times, candidate->times, num_steps * sizeof(*best_times));
	memcpy(best_states, candidate->best_states, num_steps * sizeof(*best_states));
	memcpy(best_legs, candidate->best_legs, (num_steps - 1) * sizeof(*best_legs));
	double best_total_dv = candidate->best_total_dv;
	double latest_arrival = fmin(calc_data->jd_max_arr,
		seed_times[0] + calc_data->max_duration);
	int valid_start_found = 0;

	for(int start_index = 0; start_index < timing_start_count &&
		objective_budget != 0 && !itinerary_search_cancelled(NULL); start_index++) {
		memcpy(trial.times, seed_times, num_steps * sizeof(*trial.times));
		if(start_index > 0) {
			double fraction = (double)start_index / (timing_start_count - 1);
			int valid_start = 1;
			for(size_t i = candidate->mutable_start; i < num_steps; i++) {
				double lower = trial.times[i - 1] + POWERED_MIN_LEG_DAYS;
				double upper = latest_arrival;
				if(i + 1 < num_steps)
					upper = fmin(upper, trial.times[i + 1] - POWERED_MIN_LEG_DAYS);
				if(upper < lower) {
					valid_start = 0;
					break;
				}
				trial.times[i] = lower + fraction * (upper - lower);
			}
			if(!valid_start) continue;
		}

		if(start_index == 0) {
			trial.best_total_dv = best_total_dv;
			memcpy(trial.best_states, best_states, num_steps * sizeof(*best_states));
			memcpy(trial.best_legs, best_legs, (num_steps - 1) * sizeof(*best_legs));
		} else {
			trial.best_total_dv = DBL_MAX;
		}
		if(!refine_candidate_from_start(&trial, calc_data)) {
			if(itinerary_search_cancelled(NULL)) break;
			continue;
		}
		if(!isfinite(trial.best_total_dv) || trial.best_total_dv >= DBL_MAX) continue;
		valid_start_found = 1;
		if(trial.best_total_dv >= best_total_dv) continue;
		best_total_dv = trial.best_total_dv;
		memcpy(best_times, trial.times, num_steps * sizeof(*best_times));
		memcpy(best_states, trial.best_states, num_steps * sizeof(*best_states));
		memcpy(best_legs, trial.best_legs, (num_steps - 1) * sizeof(*best_legs));
		valid_start_found = 1;
	}

	if(best_total_dv < candidate->best_total_dv) {
		candidate->best_total_dv = best_total_dv;
		memcpy(candidate->times, best_times, num_steps * sizeof(*candidate->times));
		memcpy(candidate->best_states, best_states, num_steps * sizeof(*candidate->best_states));
		memcpy(candidate->best_legs, best_legs, (num_steps - 1) * sizeof(*candidate->best_legs));
		valid_start_found = 1;
	}
	free(seed_times);
	free(best_times);
	free(best_states);
	free(best_legs);
	free(trial.times);
	free(trial.best_states);
	free(trial.best_legs);
	return valid_start_found && !itinerary_search_cancelled(NULL);
}

static void repair_candidate_timing(PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		uint64_t *random, int move_shared_prefix) {
	size_t n = candidate->num_steps;
	double *best_times = malloc(n * sizeof(*best_times));
	OSV *trial_states = calloc(n, sizeof(*trial_states));
	Lambert3 *trial_legs = calloc(n - 1, sizeof(*trial_legs));
	if(best_times == NULL || trial_states == NULL || trial_legs == NULL) goto cleanup;
	memcpy(best_times, candidate->times, n * sizeof(*best_times));
	/* Audit copies may move shared-prefix dates; production repair keeps them tied to the tree. */
	if(move_shared_prefix) candidate->mutable_start = 1;
	size_t first = candidate->mutable_start;
	int offset_attempts = 12 * (int)(n - first < 2 ? n - first : 2);
	double latest = fmin(calc_data->jd_max_arr, candidate->times[0] + calc_data->max_duration);
	static const double offsets[] = {-180, -60, -20, -7, -2, -0.5, 0.5, 2, 7, 20, 60, 180};
	for(int attempt = 0; attempt < 48 && objective_budget > 0 && !itinerary_search_cancelled(NULL); attempt++) {
		memcpy(candidate->times, candidate->signature_dates, n * sizeof(*candidate->times));
		if(attempt > 0 && attempt <= offset_attempts) {
			size_t coordinate = first + ((attempt - 1) / 12) % (n - first);
			candidate->times[coordinate] += offsets[(attempt - 1) % 12];
		} else if(attempt > offset_attempts) {
			for(size_t i = first; i < n; i++) {
				double lower = candidate->times[i - 1] + POWERED_MIN_LEG_DAYS;
				double upper = latest - (n - 1 - i) * POWERED_MIN_LEG_DAYS;
				if(upper < lower) break;
				double fraction = (audit_random(random) >> 11) * (1.0 / 9007199254740992.0);
				candidate->times[i] = attempt % 2 ? lower + fraction * (upper - lower) :
					fmax(lower, fmin(upper, candidate->signature_dates[i] + (fraction * 2 - 1) * 60));
			}
		}
		double cost = evaluate_route(candidate, calc_data, candidate->times, trial_states, trial_legs);
		if(cost < candidate->best_total_dv) {
			candidate->best_total_dv = cost;
			memcpy(best_times, candidate->times, n * sizeof(*best_times));
			memcpy(candidate->best_states, trial_states, n * sizeof(*trial_states));
			memcpy(candidate->best_legs, trial_legs, (n - 1) * sizeof(*trial_legs));
		}
	}
	memcpy(candidate->times, best_times, n * sizeof(*best_times));
	if(candidate->best_total_dv < DBL_MAX && !itinerary_search_cancelled(NULL))
		refine_candidate_from_start(candidate, calc_data);
cleanup:
	free(best_times);
	free(trial_states);
	free(trial_legs);
}

static void audit_group(AuditReservoir *pool, PoweredRejectionAuditGroup *result,
		const Itin_Calc_Data *calc_data, const char *label) {
	*result = (PoweredRejectionAuditGroup){.population = pool->population, .best_delta_v = DBL_MAX};
	uint64_t random = UINT64_C(0x9e3779b97f4a7c15);
	int best_index = -1;
	for(int i = 0; i < pool->count && !itinerary_search_cancelled(NULL); i++) {
		PoweredRouteCandidate *candidate = pool->samples[i];
		double original = candidate->best_total_dv;
		unsigned long long calls_before = objective_calls;
		/* Reserve one independent re-evaluation of the repaired dates. */
		objective_budget = AUDIT_OBJECTIVES_PER_ROUTE - 1;
		repair_candidate_timing(candidate, calc_data, &random, 1);
		if(candidate->best_total_dv < DBL_MAX && !itinerary_search_cancelled(NULL)) {
			objective_budget = 1;
			double verified = evaluate_route(candidate, calc_data, candidate->times,
				candidate->best_states, candidate->best_legs);
			candidate->best_total_dv = isfinite(verified) ? verified : DBL_MAX;
		}
		objective_budget = -1;
		result->evaluations += objective_calls - calls_before;
		result->sampled++;
		double cost = candidate->best_total_dv;
		if(!isfinite(cost) || cost >= DBL_MAX) continue;
		result->feasible++;
		if(original < DBL_MAX && cost + POWERED_IMPROVEMENT_THRESHOLD < original) result->improved++;
		if(rejection_audit.retained_best < DBL_MAX && cost + POWERED_IMPROVEMENT_THRESHOLD < rejection_audit.retained_best)
			result->beats_retained_best++;
		if(rejection_audit.retained_worst < DBL_MAX && cost + POWERED_IMPROVEMENT_THRESHOLD < rejection_audit.retained_worst)
			result->beats_retained_worst++;
		if(cost < result->best_delta_v) { result->best_delta_v = cost; best_index = i; }
	}
	printf("Rejection audit %s: population %llu, sampled %d, feasible after repair %d, improved %d, beats retained best %d / worst %d, %llu evaluations\n",
		label, result->population, result->sampled, result->feasible, result->improved,
		result->beats_retained_best, result->beats_retained_worst, result->evaluations);
	if(best_index >= 0) {
		PoweredRouteCandidate *best = pool->samples[best_index];
		printf("Audit witness %s: %.6f m/s;", label, best->best_total_dv);
		for(size_t i = 0; i < best->num_steps; i++) printf(" %s@%.9f", best->bodies[i]->name, best->times[i]);
		printf("\n");
	}
	for(int i = 0; i < pool->count; i++) destroy_candidate(pool->samples[i]);
}

static int vector_equal(Vector3 first, Vector3 second) {
	return mag_vec3(subtract_vec3(first, second)) < 1e-6;
}

static int route_node_matches(const PoweredRouteCandidate *candidate,
		const struct ItinStep *step, size_t index) {
	return step != NULL && step->body == candidate->bodies[index] &&
		fabs(step->date - candidate->signature_dates[index]) < 1e-9 &&
		vector_equal(step->r, candidate->signature_positions[index]) &&
		vector_equal(step->v_arr, candidate->signature_v_arr[index]) &&
		vector_equal(step->v_dep, candidate->signature_v_dep[index]);
}

static struct ItinStep *find_matching_leaf(struct ItinStep *step,
		const PoweredRouteCandidate *candidate, size_t index) {
	if(index >= candidate->num_steps || !route_node_matches(candidate, step, index)) return NULL;
	if(index + 1 == candidate->num_steps) return step->num_next_nodes == 0 ? step : NULL;
	for(int child = 0; child < step->num_next_nodes; child++) {
		struct ItinStep *match = find_matching_leaf(step->next[child], candidate, index + 1);
		if(match != NULL) return match;
	}
	return NULL;
}

static struct ItinStep *find_matching_prefix(struct ItinStep *step,
		const PoweredRouteCandidate *candidate, size_t prefix_length, size_t index) {
	if(index >= prefix_length || !route_node_matches(candidate, step, index)) return NULL;
	if(index + 1 == prefix_length) return step;
	for(int child = 0; child < step->num_next_nodes; child++) {
		struct ItinStep *match = find_matching_prefix(step->next[child], candidate, prefix_length, index + 1);
		if(match != NULL) return match;
	}
	return NULL;
}

/* Flyby corrections change inbound-leg velocities on every node after the departure. */
static struct ItinStep *make_optimized_suffix(const PoweredRouteCandidate *candidate) {
	size_t suffix_count = candidate->num_steps - 1;
	struct ItinStep **nodes = calloc(suffix_count, sizeof(*nodes));
	if(nodes == NULL) return NULL;
	for(size_t i = 0; i < suffix_count; i++) {
		nodes[i] = calloc(1, sizeof(*nodes[i]));
		if(nodes[i] == NULL) {
			for(size_t j = 0; j < i; j++) free(nodes[j]);
			free(nodes);
			return NULL;
		}
	}
	for(size_t i = 0; i < suffix_count; i++) {
		size_t route_index = 1 + i;
		struct ItinStep *node = nodes[i];
		node->body = candidate->bodies[route_index];
		node->date = candidate->times[route_index];
		node->r = candidate->best_states[route_index].r;
		node->v_body = candidate->best_states[route_index].v;
		node->v_arr = candidate->best_legs[route_index - 1].v1;
		node->v_dep = candidate->best_legs[route_index - 1].v0;
		if(route_index + 1 < candidate->num_steps) {
			node->next = malloc(sizeof(*node->next));
			if(node->next == NULL) {
				for(size_t j = 0; j < suffix_count; j++) {
					free(nodes[j]->next);
					free(nodes[j]);
				}
				free(nodes);
				return NULL;
			}
			node->num_next_nodes = 1;
			node->next[0] = nodes[i + 1];
		}
	}
	for(size_t i = 0; i < suffix_count; i++)
		nodes[i]->prev = i == 0 ? NULL : nodes[i - 1];
	struct ItinStep *first = nodes[0];
	free(nodes);
	return first;
}

static int replace_candidate_route(struct ItinStep **departures, PoweredRouteCandidate *candidate) {
	struct ItinStep *original_root = departures[candidate->departure_index];
	struct ItinStep *root = create_itin_copy(original_root);
	if(root == NULL) return 0;
	struct ItinStep *leaf = find_matching_leaf(root, candidate, 0);
	if(leaf == NULL) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep *old_suffix = leaf;
	for(size_t i = candidate->num_steps - 1; i > candidate->mutable_start; i--)
		old_suffix = old_suffix->prev;
	struct ItinStep *parent = old_suffix->prev;
	if(parent == NULL) {
		free_itinerary(root);
		return 0;
	}
	int child_index = -1;
	for(int i = 0; i < parent->num_next_nodes; i++) {
		if(parent->next[i] == old_suffix) {
			child_index = i;
			break;
		}
	}
	if(child_index < 0) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep *suffix = make_optimized_suffix(candidate);
	if(suffix == NULL) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep **expanded_children = realloc(root->next,
		(root->num_next_nodes + 1) * sizeof(*root->next));
	if(expanded_children == NULL) {
		free_itinerary(suffix);
		free_itinerary(root);
		return 0;
	}
	root->next = expanded_children;
	for(int i = child_index; i + 1 < parent->num_next_nodes; i++)
		parent->next[i] = parent->next[i + 1];
	parent->num_next_nodes--;
	free_itinerary(old_suffix);
	suffix->prev = root;
	root->next[root->num_next_nodes++] = suffix;
	departures[candidate->departure_index] = root;
	free_itinerary(original_root);
	return 1;
}

static int insert_top_candidate(PoweredRouteCandidate *candidate, double cost,
		int max_routes, int *count,
		PoweredRouteCandidate *candidates[POWERED_ROUTE_REFINEMENT_LIMIT],
		double costs[POWERED_ROUTE_REFINEMENT_LIMIT], ExcludedPools *excluded) {
	if(*count == max_routes && cost >= costs[*count - 1]) return 0;
	int insert = *count < max_routes ? (*count)++ : max_routes - 1;
	if(*count == max_routes && candidates[insert] != NULL)
		retain_excluded(excluded, candidates[insert]);
	while(insert > 0 && cost < costs[insert - 1]) {
		candidates[insert] = candidates[insert - 1];
		costs[insert] = costs[insert - 1];
		insert--;
	}
	candidates[insert] = candidate;
	costs[insert] = cost;
	return 1;
}

int optimize_powered_flyby_routes(struct ItinStep **departures, int num_departures,
		const Itin_Calc_Data *calc_data, int max_routes, PoweredFlybyOptimizationStats *stats) {
	rejection_audit = (PoweredRejectionAudit){.retained_best = DBL_MAX};
	AuditReservoir failed = {.random = UINT64_C(0x123456789abcdef)},
		discarded = {.random = UINT64_C(0xfedcba987654321)},
		challenge = {.random = UINT64_C(0x2545f4914f6cdd1d)};
	RepairPool *repair_pool = calloc(1, sizeof(*repair_pool));
	if(repair_pool != NULL) repair_pool->random = UINT64_C(0x9e3779b97f4a7c15);
	ExcludedPools excluded = {.challenge = &challenge, .audit = rejection_audit_enabled ? &discarded : NULL};
	if(stats != NULL) *stats = (PoweredFlybyOptimizationStats) {
		.best_unpowered_seed_total_dv = DBL_MAX,
		.best_initial_total_dv = DBL_MAX,
		.best_refined_total_dv = DBL_MAX
	};
	if(departures == NULL || num_departures <= 0 || calc_data == NULL || max_routes <= 0 ||
		calc_data->seq_info.to_target.type != ITIN_SEQ_INFO_TO_TARGET) return 0;
	if(itinerary_search_cancelled(NULL)) return 0;
	int num_itineraries = 0;
	for(int i = 0; i < num_departures && !itinerary_search_cancelled(NULL); i++)
		num_itineraries += get_number_of_itineraries(departures[i]);
	if(itinerary_search_cancelled(NULL)) return 0;
	if(num_itineraries <= 0) return 0;
	struct ItinStep **arrivals = malloc((size_t)num_itineraries * sizeof(*arrivals));
	if(arrivals == NULL) return 0;
	int arrival_count = 0;
	for(int i = 0; i < num_departures; i++)
		store_itineraries_in_array(departures[i], arrivals, &arrival_count);

	PoweredRouteCandidate **candidates = calloc((size_t)POWERED_ROUTE_REFINEMENT_LIMIT +
		POWERED_CHALLENGE_SLOTS, sizeof(*candidates));
	double top_costs[POWERED_ROUTE_REFINEMENT_LIMIT + POWERED_CHALLENGE_SLOTS];
	int top_count = 0;
	int route_limit = max_routes < POWERED_ROUTE_REFINEMENT_LIMIT ?
		max_routes : POWERED_ROUTE_REFINEMENT_LIMIT;
	if(candidates == NULL || repair_pool == NULL) {
		free(candidates);
		free(repair_pool);
		free(arrivals);
		return 0;
	}
	int routes_considered = 0;
	int routes_valid = 0;
	objective_calls = 0;
	int progress_total = arrival_count + POWERED_REFINE_PROGRESS_WEIGHT *
		(POWERED_REPAIR_SLOTS + route_limit + POWERED_CHALLENGE_SLOTS);
	incr_thread_counter_by_amount(POWERED_PROGRESS_TOTAL_COUNTER, progress_total);
	double exact_scoring_start = wall_seconds();
	for(int point_index = 0; point_index < arrival_count && !itinerary_search_cancelled(NULL); point_index++) {
		if((point_index & 255) == 255) incr_thread_counter_by_amount(POWERED_PROGRESS_DONE_COUNTER, 256);
		if(get_num_of_itin_layers(get_first(arrivals[point_index])) < 3) continue;
		routes_considered++;
		PoweredRouteCandidate *candidate = calloc(1, sizeof(*candidate));
		if(candidate == NULL) continue;
		if(!initialize_candidate(candidate, arrivals[point_index], departures, num_departures)) {
			destroy_candidate(candidate);
			continue;
		}
		candidate->best_total_dv = evaluate_route(candidate, calc_data, candidate->times,
			candidate->best_states, candidate->best_legs);
		if(!isfinite(candidate->best_total_dv) || candidate->best_total_dv >= DBL_MAX) {
			candidate->best_total_dv = DBL_MAX;
			if(rejection_audit_enabled) {
				PoweredRouteCandidate *copy = copy_candidate(candidate);
				if(copy != NULL) audit_retain(&failed, copy);
			}
			repair_pool_retain(repair_pool, candidate, num_departures);
			continue;
		}
		candidate->original_total_dv = candidate->best_total_dv;
		routes_valid++;
		if(stats != NULL) {
			struct PorkchopPoint seed_point = create_porkchop_point(arrivals[point_index],
				calc_data->dv_filter.dep_periapsis, calc_data->dv_filter.arr_periapsis);
			if(isfinite(seed_point.dv_flyby) && seed_point.dv_flyby <= 1.0 &&
				candidate->original_total_dv < stats->best_unpowered_seed_total_dv)
				stats->best_unpowered_seed_total_dv = candidate->original_total_dv;
		}
		if(stats != NULL && candidate->original_total_dv < stats->best_initial_total_dv)
			stats->best_initial_total_dv = candidate->original_total_dv;
		if(!insert_top_candidate(candidate, candidate->best_total_dv, route_limit,
			&top_count, candidates, top_costs, &excluded))
			retain_excluded(&excluded, candidate);
	}
	free(arrivals);

	/* Repaired routes compete by full-mission cost; user limits are enforced by evaluate_route. */
	if(stats != NULL) stats->initial_rejections = repair_pool->total;
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS && !itinerary_search_cancelled(NULL); slot++) {
		PoweredRouteCandidate *candidate = repair_pool->slots[slot];
		if(candidate == NULL) continue;
		repair_pool->slots[slot] = NULL;
		incr_thread_counter_by_amount(POWERED_PROGRESS_DONE_COUNTER, POWERED_REFINE_PROGRESS_WEIGHT);
		if(stats != NULL) stats->repairs_attempted++;
		objective_budget = POWERED_REPAIR_OBJECTIVES_PER_ROUTE - 1;
		repair_candidate_timing(candidate, calc_data, &repair_pool->random, 0);
		double verified = DBL_MAX;
		if(candidate->best_total_dv < DBL_MAX && !itinerary_search_cancelled(NULL)) {
			objective_budget = 1;
			verified = evaluate_route(candidate, calc_data, candidate->times,
				candidate->best_states, candidate->best_legs);
		}
		objective_budget = -1;
		if(!isfinite(verified) || verified >= DBL_MAX) {
			destroy_candidate(candidate);
			continue;
		}
		candidate->best_total_dv = candidate->original_total_dv = verified;
		if(stats != NULL) stats->repairs_recovered++;
		if(insert_top_candidate(candidate, verified, route_limit, &top_count,
			candidates, top_costs, &excluded)) {
			if(stats != NULL) stats->repairs_competing++;
			if(stats != NULL && verified < stats->best_initial_total_dv)
				stats->best_initial_total_dv = verified;
		} else retain_excluded(&excluded, candidate);
	}
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS; slot++)
		destroy_candidate(repair_pool->slots[slot]);
	free(repair_pool);

	/* Sampled cutoff losers get a refinement slot beyond the ranked set. */
	int ranked_count = top_count;
	if(stats != NULL) stats->challenge_population = challenge.population;
	for(int i = 0; i < challenge.count; i++) {
		candidates[top_count] = challenge.samples[i];
		top_costs[top_count++] = challenge.samples[i]->best_total_dv;
	}
	challenge.count = 0;
	if(stats != NULL) {
		stats->routes_considered = routes_considered;
		stats->routes_valid = routes_valid;
		stats->exact_scoring_seconds = wall_seconds() - exact_scoring_start;
	}
	if(itinerary_search_cancelled(NULL)) {
		for(int i = 0; i < failed.count; i++) destroy_candidate(failed.samples[i]);
		for(int i = 0; i < discarded.count; i++) destroy_candidate(discarded.samples[i]);
		for(int candidate_index = 0; candidate_index < top_count; candidate_index++)
			destroy_candidate(candidates[candidate_index]);
		free(candidates);
		return 0;
	}

	int improvements = 0;
	int candidate_index = 0;
	double timing_refinement_start = wall_seconds();
	int multi_start_routes = 0;
	double ranked_worst = 0;
	for(; candidate_index < top_count && !itinerary_search_cancelled(NULL); candidate_index++) {
		PoweredRouteCandidate *candidate = candidates[candidate_index];
		/* Spend extra starts on competitive routes or the first of a body sequence. */
		int new_sequence = 1;
		for(int earlier = 0; earlier < candidate_index && new_sequence; earlier++) {
			PoweredRouteCandidate *other = candidates[earlier];
			if(other->num_steps != candidate->num_steps) continue;
			new_sequence = 0;
			for(size_t i = 0; i < candidate->num_steps; i++)
				if(other->bodies[i] != candidate->bodies[i]) { new_sequence = 1; break; }
		}
		int extra_starts = multi_start_routes < POWERED_MULTI_START_ROUTE_LIMIT &&
			(new_sequence || candidate->best_total_dv <= top_costs[0] * 1.10);
		int timing_start_count = extra_starts ? POWERED_TIMING_START_COUNT : 1;
		multi_start_routes += extra_starts;
		objective_budget = POWERED_OBJECTIVES_PER_ROUTE;
		int optimized = optimize_candidate(candidate, calc_data, timing_start_count);
		incr_thread_counter_by_amount(POWERED_PROGRESS_DONE_COUNTER, POWERED_REFINE_PROGRESS_WEIGHT);
		if(stats && objective_budget == 0) stats->refinement_budgets_exhausted++;
		objective_budget = -1;
		if(optimized &&
			replace_candidate_route(departures, candidate)) {
			rejection_audit.retained_best = fmin(rejection_audit.retained_best, candidate->best_total_dv);
			rejection_audit.retained_worst = fmax(rejection_audit.retained_worst, candidate->best_total_dv);
			if(stats != NULL) {
				stats->routes_corrected++;
				if(candidate_index < ranked_count)
					ranked_worst = fmax(ranked_worst, candidate->best_total_dv);
				else {
					stats->challenges_refined++;
					if(candidate->best_total_dv < ranked_worst) stats->challenges_beat_cutoff++;
				}
				if(candidate->best_total_dv < stats->best_refined_total_dv)
					stats->best_refined_total_dv = candidate->best_total_dv;
			}
			double saving = candidate->original_total_dv - candidate->best_total_dv;
			if(saving > POWERED_IMPROVEMENT_THRESHOLD) {
				improvements++;
				if(stats != NULL && saving > stats->best_delta_v_saving)
					stats->best_delta_v_saving = saving;
			}
		}
	}
	for(int i = 0; i < top_count; i++)
		destroy_candidate(candidates[i]);
	free(candidates);
	if(progress_total > get_thread_counter(POWERED_PROGRESS_DONE_COUNTER))
		incr_thread_counter_by_amount(POWERED_PROGRESS_DONE_COUNTER,
			progress_total - get_thread_counter(POWERED_PROGRESS_DONE_COUNTER));
	if(stats != NULL) {
		stats->routes_improved = improvements;
		stats->timing_refinement_seconds = wall_seconds() - timing_refinement_start;
		stats->objective_evaluations = objective_calls;
		stats->multi_start_routes = multi_start_routes;
	}
	if(rejection_audit_enabled) {
		double audit_start = wall_seconds();
		unsigned long long production_calls = objective_calls;
		audit_group(&failed, &rejection_audit.initial_failure, calc_data, "initial-failure");
		audit_group(&discarded, &rejection_audit.refinement_cutoff, calc_data, "refinement-cutoff");
		objective_calls = production_calls;
		rejection_audit.seconds = wall_seconds() - audit_start;
		rejection_audit.completed = !itinerary_search_cancelled(NULL);
		printf("Rejection audit: %.3f wall seconds, retained corrected objective range %.6f..%.6f m/s; timing repair only, no DSM\n",
			rejection_audit.seconds, rejection_audit.retained_best, rejection_audit.retained_worst);
	}
	return improvements;
}
