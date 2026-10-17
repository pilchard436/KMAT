#include "flyby_search.h"
#include "flyby_policy.h"
#include "leg_variants.h"
#include "resonant_flybys.h"
#include "search_cancel.h"
#include "powered_seed_selection.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define POWERED_FLYBY_SEED_SUBDIVISIONS 2

static _Thread_local int extra_seed_budget;
static _Thread_local int powered_seed_budget = POWERED_SEEDS_PER_DEPARTURE;
static int seeds_per_departure = POWERED_SEEDS_PER_DEPARTURE;
static _Thread_local PoweredSeedSearchStats *seed_search_stats;

// Only for coverage measurements; call before a search starts.
void set_powered_seed_budget(int seeds) {
	seeds_per_departure = seeds;
}

void begin_powered_seed_search(PoweredSeedSearchStats *stats) {
	extra_seed_budget = POWERED_EXTRA_SEEDS_PER_DEPARTURE;
	powered_seed_budget = seeds_per_departure;
	seed_search_stats = stats;
}

#ifdef KMAT_ENABLE_SEARCH_PROFILE
static _Thread_local unsigned long long *itin_search_lambert_counter;
static _Thread_local Body **profile_bodies_tls;
static _Thread_local ItinFlybyProfileStats *profile_stats_tls;
static _Thread_local int profile_body_count_tls;
#endif

#ifdef KMAT_ENABLE_SEARCH_PROFILE
void set_itin_search_lambert_counter(unsigned long long *counter) {
	itin_search_lambert_counter = counter;
}

void set_itin_search_flyby_profile(Body **bodies, int num_bodies, ItinFlybyProfileStats *stats) {
	profile_bodies_tls = bodies;
	profile_body_count_tls = num_bodies;
	profile_stats_tls = stats;
}

static ItinFlybyProfileStats *get_flyby_profile_stats(Body *body) {
	if(profile_bodies_tls == NULL || profile_stats_tls == NULL) return NULL;
	for(int i = 0; i < profile_body_count_tls; i++) {
		if(profile_bodies_tls[i] == body) return &profile_stats_tls[i];
	}
	return NULL;
}
#endif

void calc_time_to_next_conjunction_and_opposition(Vector3 r0, OSV osv0_next, Body *cb, double *next_conjunction_dt, double *next_opposition_dt) {
	Vector3 proj_vec = proj_vec3_plane3(r0, constr_plane3(vec3(0,0,0), osv0_next.r, osv0_next.v));
	double dta_conj = angle_vec3_vec3(proj_vec, osv0_next.r);
	if(cross_vec3(osv0_next.r, proj_vec).z < 0) dta_conj = 2*M_PI-dta_conj;
	double dta_opp = dta_conj > M_PI ? dta_conj-M_PI : dta_conj+M_PI;
	
	Orbit arr0 = constr_orbit_from_osv(osv0_next.r, osv0_next.v, cb);
	double period_arr0 = calc_orbital_period(arr0);
	double tpe_arr0 = calc_orbit_time_since_periapsis(arr0);
	
	Orbit arr_conj = arr0; arr_conj.ta = pi_norm(arr_conj.ta+dta_conj);
	Orbit arr_opp = arr0; arr_opp.ta = pi_norm(arr_opp.ta+dta_opp);
	double dt_conj = calc_orbit_time_since_periapsis(arr_conj)-tpe_arr0;
	double dt_opp = calc_orbit_time_since_periapsis(arr_opp)-tpe_arr0;
	
	if(dt_conj < 0) dt_conj += period_arr0;
	if(dt_opp < 0)	dt_opp += period_arr0;
	
	*next_conjunction_dt = dt_conj;
	*next_opposition_dt = dt_opp;
}

static int evaluate_powered_flyby_seed(struct ItinStep *tf, CelestSystem *system,
		Body *next_body, double dt, int right_side, PoweredFlybySeed *seed) {
	double epoch = tf->date + dt / 86400;
	seed->dt = dt;
	seed->estimated_burn = HUGE_VAL;
	seed->target_state = system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(next_body->orbit, epoch) :
		osv_from_ephem(next_body->ephem, next_body->num_ephems, epoch, system->cb);
	if(!isfinite(seed->target_state.r.x) || !isfinite(seed->target_state.r.y) ||
		!isfinite(seed->target_state.r.z) || !isfinite(seed->target_state.v.x) ||
		!isfinite(seed->target_state.v.y) || !isfinite(seed->target_state.v.z)) return 0;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	ItinFlybyProfileStats *profile_stats = get_flyby_profile_stats(next_body);
	if(profile_stats != NULL) {
		if(right_side) profile_stats->right_evaluations++;
		else profile_stats->left_evaluations++;
	}
	if(itin_search_lambert_counter != NULL) (*itin_search_lambert_counter)++;
#else
	(void)right_side;
#endif
	seed->transfer = calc_lambert3_cancellable(tf->r, seed->target_state.r,
		dt, system->cb, itinerary_search_cancelled, NULL);
	if(itinerary_search_cancelled(NULL) ||
		seed->transfer.success == LAMBERT_FAIL_NAN ||
		seed->transfer.success == LAMBERT_FAIL_ECC ||
		seed->transfer.success == LAMBERT_MAX_ITERATIONS ||
		!isfinite(seed->transfer.v0.x) || !isfinite(seed->transfer.v0.y) ||
		!isfinite(seed->transfer.v0.z) || !isfinite(seed->transfer.v1.x) ||
		!isfinite(seed->transfer.v1.y) || !isfinite(seed->transfer.v1.z)) return 0;

	Vector3 incoming = subtract_vec3(tf->v_arr, tf->v_body);
	Vector3 outgoing = subtract_vec3(seed->transfer.v0, tf->v_body);
	double incoming_speed = mag_vec3(incoming);
	double outgoing_speed = mag_vec3(outgoing);
	seed->diff_vinf = outgoing_speed - incoming_speed;
	double minimum_periapsis = flyby_minimum_periapsis(tf->body);
	if(!(incoming_speed > 0) || !(outgoing_speed > 0) || !isfinite(seed->diff_vinf) ||
		!(tf->body->mu > 0) ||
		!(minimum_periapsis > 0)) return 0;
	double periapsis_in_speed = sqrt(incoming_speed * incoming_speed +
		2 * tf->body->mu / minimum_periapsis);
	double periapsis_out_speed = sqrt(outgoing_speed * outgoing_speed +
		2 * tf->body->mu / minimum_periapsis);
	double direction_cosine = dot_vec3(incoming, outgoing) / (incoming_speed * outgoing_speed);
	direction_cosine = fmax(-1, fmin(1, direction_cosine));
	double estimate_squared = periapsis_in_speed * periapsis_in_speed +
		periapsis_out_speed * periapsis_out_speed -
		2 * periapsis_in_speed * periapsis_out_speed * direction_cosine;
	seed->estimated_burn = sqrt(fmax(0, estimate_squared));
	return isfinite(seed->estimated_burn);
}

static double feasible_seed_cost(const PoweredFlybySeed *seed, void *context) {
	struct ItinStep *tf = context;
	if(itinerary_search_cancelled(NULL)) return HUGE_VAL;
	PoweredFlybyResult flyby;
	if(!calculate_powered_flyby(tf->v_arr, seed->transfer.v0, tf->v_body,
		tf->body, flyby_minimum_periapsis(tf->body), &flyby)) return HUGE_VAL;
	return flyby.burn_delta_v;
}

void find_viable_flybys(struct ItinStep *tf, CelestSystem *system, Body *next_body, double min_dt, double max_dt) {
	if(itinerary_search_cancelled(NULL)) return;
	if(next_body == tf->body) {
		find_resonant_flybys(tf, system, min_dt, max_dt);
		return;
	}
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	ItinFlybyProfileStats *profile_stats = get_flyby_profile_stats(next_body);
	if(profile_stats != NULL) profile_stats->searches++;
#endif
	OSV osv_dep = {tf->r, tf->v_body};
	OSV osv_arr0 = system->prop_method == ORB_ELEMENTS ?
				   osv_from_elements(next_body->orbit, tf->date) :
				   osv_from_ephem(next_body->ephem, next_body->num_ephems, tf->date, system->cb);
	double next_conjunction_dt, next_opposition_dt;
	calc_time_to_next_conjunction_and_opposition(osv_dep.r, osv_arr0, system->cb, &next_conjunction_dt, &next_opposition_dt);
	Orbit arr0 = constr_orbit_from_osv(osv_arr0.r, osv_arr0.v, system->cb);
	double period_arr0 = calc_orbital_period(arr0);
	
	double dt0, dt1;
	if(next_conjunction_dt < next_opposition_dt) {
		dt0 = next_opposition_dt - period_arr0;
		dt1 = next_conjunction_dt;
	} else {
		dt0 = next_conjunction_dt - period_arr0;
		dt1 = next_opposition_dt;
	}
	
	// x: dt, y: diff_vinf
	DataArray2 *data = data_array2_create();
	/* Preserve the existing 99 root candidates even when an extra seed is allowed. */
	int max_root_steps = 99;
	int max_new_steps = max_root_steps + 2;
	struct ItinStep *new_steps[max_new_steps];
	int counter = 0;
	int seed_budget = extra_seed_budget > 0 ? 2 : 1;
	if(seed_budget > powered_seed_budget) seed_budget = powered_seed_budget;
	if(seed_budget == 0 && seed_search_stats) seed_search_stats->searches_budget_skipped++;
	int interval_subdivisions = POWERED_FLYBY_SEED_SUBDIVISIONS;
	PoweredSeedBucket seed_buckets[POWERED_SEED_BUCKETS] = {0};
	double seed_time_range = max_dt - min_dt;

	double t0 = tf->date;
	double last_dt, dt, t1, diff_vinf;

	while(dt0 < max_dt && counter < max_root_steps &&
		!itinerary_search_cancelled(NULL)) {
		int right_side = 0;	// 0 = left, 1 = right
		double interval_start = fmax(dt0, min_dt);
		double interval_end = fmin(dt1, max_dt);
		PoweredFlybySeed interval_samples[POWERED_FLYBY_SEED_SUBDIVISIONS + 1] = {0};
		int interval_sample_valid[POWERED_FLYBY_SEED_SUBDIVISIONS + 1] = {0};
		if(seed_budget > 0 && tf->prev != NULL && interval_end >= interval_start && seed_time_range > 0) {
			for(int sample_index = 0; sample_index <= interval_subdivisions &&
				!itinerary_search_cancelled(NULL); sample_index++) {
				double fraction = (double)sample_index / interval_subdivisions;
				double sample_dt = interval_start + fraction * (interval_end - interval_start);
				PoweredFlybySeed *seed = &interval_samples[sample_index];
				if(!evaluate_powered_flyby_seed(tf, system, next_body, sample_dt,
				sample_index >= interval_subdivisions / 2, seed)) continue;
				interval_sample_valid[sample_index] = 1;
				int bucket = (int)(((sample_dt - min_dt) / seed_time_range) *
					POWERED_SEED_BUCKETS);
				if(bucket < 0) bucket = 0;
				if(bucket >= POWERED_SEED_BUCKETS) bucket = POWERED_SEED_BUCKETS - 1;
				retain_powered_seed(&seed_buckets[bucket], *seed);
			}
		}

		for(int i = 0; i < 100 && counter < max_root_steps &&
			!itinerary_search_cancelled(NULL); i++) {
			if(i == 0) dt = dt0;
			if(dt < min_dt) dt = min_dt;
			if(dt > max_dt) dt = max_dt;
			t1 = t0 + dt / 86400;
			PoweredFlybySeed root_sample;
			int cached_sample = 0;
			for(int sample_index = 0; sample_index <= interval_subdivisions; sample_index++) {
				if(interval_sample_valid[sample_index] &&
					fabs(interval_samples[sample_index].dt - dt) < 1e-9) {
					root_sample = interval_samples[sample_index];
					cached_sample = 1;
					break;
				}
			}
			if(!cached_sample && !evaluate_powered_flyby_seed(tf, system, next_body, dt,
				right_side, &root_sample)) break;
			t1 = tf->date + root_sample.dt / 86400;
			OSV osv_arr = root_sample.target_state;
			Lambert3 new_transfer = root_sample.transfer;
			diff_vinf = root_sample.diff_vinf;

			if(fabs(diff_vinf) < 1) {
				PoweredFlybyResult flyby_result;
				double minimum_periapsis = flyby_minimum_periapsis(tf->body);
				// Moon hits are rejected when the finished route is scored, not for every candidate.
				if(calculate_powered_flyby(tf->v_arr, new_transfer.v0, tf->v_body,
					tf->body, minimum_periapsis, &flyby_result)) {
					new_steps[counter] = (struct ItinStep*) malloc(sizeof(struct ItinStep));
					new_steps[counter]->body = next_body;
					new_steps[counter]->date = t1;
					new_steps[counter]->r = osv_arr.r;
					new_steps[counter]->v_dep = new_transfer.v0;
					new_steps[counter]->v_arr = new_transfer.v1;
					new_steps[counter]->v_body = osv_arr.v;
					new_steps[counter]->num_next_nodes = 0;
					new_steps[counter]->prev = tf;
					new_steps[counter]->next = NULL;
					counter++;
				}

				if(!right_side) right_side = 1;
				else break;
			}


			data_array2_insert_new(data, vec2(dt, diff_vinf));

			if(!can_be_negative_monot_deriv(data)) break;
			last_dt = dt;
			if(i == 0) dt = dt1;
			else dt = root_finder_monot_deriv_next_x(data, right_side ? 1 : 0);
			if(i > 3 && dt == last_dt) break;	// step size 0 (imprecision)
			if(isnan(dt) || isinf(dt)) break;
		}

		data_array2_clear(data);

		double temp = dt1;
		dt1 = dt0 + period_arr0;
		dt0 = temp;
	}
	
	data_array2_free(data);

	int seed_count = 0;
	PoweredFlybySeed seed_candidates[POWERED_SEED_BUCKETS];
	unsigned long long evaluations = 0;
	for(int bucket = 0; bucket < POWERED_SEED_BUCKETS && !itinerary_search_cancelled(NULL); bucket++) {
		/* Skip duplicate root dates before feasibility selection so a fallback gets a turn. */
		PoweredSeedBucket distinct = {0};
		for(int s = 0; s < seed_buckets[bucket].count; s++) {
			PoweredFlybySeed seed = seed_buckets[bucket].seeds[s];
			int duplicate = 0;
			for(int i = 0; i < counter; i++)
				if(fabs(new_steps[i]->date - (tf->date + seed.dt / 86400)) < 1e-9) duplicate = 1;
			if(!duplicate) retain_powered_seed(&distinct, seed);
		}
		double actual_cost;
		if(select_powered_seed(&distinct, feasible_seed_cost, tf,
			&seed_candidates[seed_count], &actual_cost, &evaluations)) {
			seed_candidates[seed_count++].actual_burn = actual_cost;
		}
	}
	if(seed_search_stats) seed_search_stats->evaluations += evaluations;
	for(int i = 1; i < seed_count; i++) {
		PoweredFlybySeed seed = seed_candidates[i];
		int insert_index = i;
		while(insert_index > 0 && seed.actual_burn < seed_candidates[insert_index - 1].actual_burn) {
			seed_candidates[insert_index] = seed_candidates[insert_index - 1];
			insert_index--;
		}
		seed_candidates[insert_index] = seed;
	}
	int powered_seeds_added = 0;
	for(int seed_index = 0; seed_index < seed_count && powered_seeds_added < seed_budget &&
		counter < max_new_steps && !itinerary_search_cancelled(NULL); seed_index++) {
		double epoch = tf->date + seed_candidates[seed_index].dt / 86400;
		int duplicate = 0;
		for(int i = 0; i < counter; i++)
			if(fabs(new_steps[i]->date - epoch) < 1e-9) duplicate = 1;
		if(duplicate) continue;
		struct ItinStep *new_step = calloc(1, sizeof(*new_step));
		if(new_step == NULL) continue;
		new_step->body = next_body;
		new_step->date = epoch;
		new_step->r = seed_candidates[seed_index].target_state.r;
		new_step->v_dep = seed_candidates[seed_index].transfer.v0;
		new_step->v_arr = seed_candidates[seed_index].transfer.v1;
		new_step->v_body = seed_candidates[seed_index].target_state.v;
		new_step->prev = tf;
		new_steps[counter++] = new_step;
		powered_seeds_added++;
	}
	if(powered_seeds_added > 1) {
		extra_seed_budget -= powered_seeds_added - 1;
		if(seed_search_stats) seed_search_stats->alternatives_added += powered_seeds_added - 1;
	}
	powered_seed_budget -= powered_seeds_added;
	if(seed_search_stats) seed_search_stats->seeds_added += powered_seeds_added;
	if(seed_search_stats && seed_count > powered_seeds_added)
		seed_search_stats->alternatives_skipped += seed_count - powered_seeds_added;

	if(counter > 0) {
		if(tf->num_next_nodes == 0) tf->next = (struct ItinStep **) malloc(counter * sizeof(struct ItinStep *));
		else {
			struct ItinStep ** new_next = realloc(tf->next, (counter+tf->num_next_nodes) * sizeof(struct ItinStep *));
			if(new_next == NULL) {
				printf("\nERROR WHILE REALLOCATING ITINERARIES DURING TRANSFER CALCULATION!!!\n");
				return;
			}
			tf->next = new_next;
		}
		for(int i = 0; i < counter; i++) tf->next[i+tf->num_next_nodes] = new_steps[i];

		tf->num_next_nodes += counter;
	}
}
