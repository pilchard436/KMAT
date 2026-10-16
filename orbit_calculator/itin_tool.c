#include "itin_tool.h"
#include "powered_seed_selection.h"
#include "tools/thread_pool.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define POWERED_FLYBY_SEED_SUBDIVISIONS 2

static _Thread_local int extra_seed_budget;
static _Thread_local int powered_seed_budget = POWERED_SEEDS_PER_DEPARTURE;
static _Thread_local PoweredSeedSearchStats *seed_search_stats;

void begin_powered_seed_search(PoweredSeedSearchStats *stats) {
	extra_seed_budget = POWERED_EXTRA_SEEDS_PER_DEPARTURE;
	powered_seed_budget = POWERED_SEEDS_PER_DEPARTURE;
	seed_search_stats = stats;
}

static int itinerary_search_cancelled(void *context) {
	(void)context;
	return get_thread_counter(3) > 0;
}


const int MIN_TRANSFER_DURATION = 10;	// days
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
	double minimum_periapsis = tf->body->radius + tf->body->atmo_alt + 1e4;
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
		tf->body, tf->body->radius + tf->body->atmo_alt + 1e4, &flyby)) return HUGE_VAL;
	return flyby.burn_delta_v;
}

Lambert3 calc_leg_variant_lambert(Body *cb, Vector3 r0, Vector3 r1, double dt, int variant,
		LambertCancelCheck cancel_check, void *cancel_context) {
	if(variant <= 0) return calc_lambert3_cancellable(r0, r1, dt, cb, cancel_check, cancel_context);
	return calc_lambert3_revs_cancellable(r0, r1, dt, cb, (variant + 1) / 2, (variant - 1) % 2,
		cancel_check, cancel_context);
}

static int is_usable_lambert(Lambert3 leg) {
	return leg.success != LAMBERT_FAIL_NAN && leg.success != LAMBERT_FAIL_ECC &&
		leg.success != LAMBERT_MAX_ITERATIONS && isfinite(leg.v0.x + leg.v0.y + leg.v0.z) &&
		isfinite(leg.v1.x + leg.v1.y + leg.v1.z);
}

int nearest_leg_variant_lambert(Body *cb, Vector3 r0, Vector3 r1, double dt, Vector3 v_ref, Lambert3 *leg, int *variant) {
	double best_distance = HUGE_VAL;
	for(int candidate_variant = 0; candidate_variant < LEG_VARIANT_COUNT; candidate_variant++) {
		Lambert3 candidate = calc_leg_variant_lambert(cb, r0, r1, dt, candidate_variant, NULL, NULL);
		if(!is_usable_lambert(candidate)) continue;
		double distance = mag_vec3(subtract_vec3(candidate.v0, v_ref));
		if(distance < best_distance) {
			best_distance = distance;
			*leg = candidate;
			if(variant != NULL) *variant = candidate_variant;
		}
	}
	return best_distance < HUGE_VAL;
}

#define RESONANT_MAX_ROOTS 12
#define RESONANT_MAX_BODY_REVS 2
#define RESONANT_SAMPLE_SECONDS (6 * 86400.0)
#define RESONANT_DEPARTURE_STEPS 16

int resonant_departure_sample_count(void) {
	return (1 + 2 * 2 * 1) * RESONANT_DEPARTURE_STEPS + (1 + 2 * 2 * 2) * RESONANT_DEPARTURE_STEPS;
}

void resonant_departure_sample(int index, double period_days, double *duration_days, int *variant) {
	int first_group = (1 + 2 * 2 * 1) * RESONANT_DEPARTURE_STEPS;
	int body_revs = index < first_group ? 1 : 2;
	int local = index < first_group ? index : index - first_group;
	*variant = local / RESONANT_DEPARTURE_STEPS;
	*duration_days = body_revs * period_days * (0.75 + 0.5 * (local % RESONANT_DEPARTURE_STEPS + 0.5) / RESONANT_DEPARTURE_STEPS);
}

// Outgoing minus incoming excess speed for a return to the same body dt seconds later on the given leg variant.
static int resonant_speed_difference(struct ItinStep *tf, CelestSystem *system, double incoming_speed,
		double dt, int variant, Lambert3 *leg, double *difference) {
	double epoch = tf->date + dt / 86400;
	OSV target = system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(tf->body->orbit, epoch) :
		osv_from_ephem(tf->body->ephem, tf->body->num_ephems, epoch, system->cb);
	if(!isfinite(target.r.x + target.r.y + target.r.z)) return 0;
	*leg = calc_leg_variant_lambert(system->cb, tf->r, target.r, dt, variant, itinerary_search_cancelled, NULL);
	if(itinerary_search_cancelled(NULL) || !is_usable_lambert(*leg)) return 0;
	*difference = mag_vec3(subtract_vec3(leg->v0, tf->v_body)) - incoming_speed;
	return isfinite(*difference);
}

static void find_resonant_flybys(struct ItinStep *tf, CelestSystem *system, double min_dt, double max_dt) {
	if(tf->prev == NULL || tf->body == NULL) return;
	double incoming_speed = mag_vec3(subtract_vec3(tf->v_arr, tf->v_body));
	double period = calc_orbital_period(constr_orbit_from_osv(tf->r, tf->v_body, system->cb));
	if(!(incoming_speed > 0) || !(period > 0) || !isfinite(period)) return;
	double minimum_periapsis = tf->body->radius + tf->body->atmo_alt + 1e4;
	struct ItinStep *new_steps[RESONANT_MAX_ROOTS];
	int counter = 0;
	for(int body_revs = 1; body_revs <= RESONANT_MAX_BODY_REVS && counter < RESONANT_MAX_ROOTS; body_revs++) {
		double window_start = fmax(min_dt, body_revs * period * 0.75);
		double window_end = fmin(max_dt, body_revs * period * 1.25);
		if(window_end <= window_start) continue;
		int variants = 1 + 2 * 2 * body_revs;
		if(variants > LEG_VARIANT_COUNT) variants = LEG_VARIANT_COUNT;
		for(int variant = 0; variant < variants && counter < RESONANT_MAX_ROOTS &&
			!itinerary_search_cancelled(NULL); variant++) {
			double previous_dt = 0, previous_difference = 0;
			int has_previous = 0;
			for(double dt = window_start; counter < RESONANT_MAX_ROOTS; dt += RESONANT_SAMPLE_SECONDS) {
				if(dt > window_end) dt = window_end;
				Lambert3 leg;
				double difference;
				int valid = resonant_speed_difference(tf, system, incoming_speed, dt, variant, &leg, &difference);
				if(valid && has_previous && previous_difference * difference < 0) {
					double low = previous_dt, high = dt, low_difference = previous_difference;
					Lambert3 root_leg = leg;
					double root_difference = difference;
					for(int iteration = 0; iteration < 40; iteration++) {
						double middle = (low + high) / 2;
						Lambert3 middle_leg;
						double middle_difference;
						if(!resonant_speed_difference(tf, system, incoming_speed, middle, variant, &middle_leg, &middle_difference)) break;
						root_leg = middle_leg;
						root_difference = middle_difference;
						if(fabs(middle_difference) < 1e-3) { low = high = middle; break; }
						if(low_difference * middle_difference < 0) high = middle;
						else { low = middle; low_difference = middle_difference; }
					}
					double root_dt = (low + high) / 2;
					PoweredFlybyResult flyby;
					Orbit leg_orbit = constr_orbit_from_osv(tf->r, root_leg.v0, system->cb);
					// A leg that dips into the central body, or a bisection that did not settle, is not a flyby.
					if(fabs(root_difference) < 1 && leg_orbit.e < 1 && leg_orbit.a * (1 - leg_orbit.e) > 2 * system->cb->radius &&
						calculate_powered_flyby(tf->v_arr, root_leg.v0, tf->v_body, tf->body, minimum_periapsis, &flyby) &&
						flyby.burn_delta_v < 1) {
						double epoch = tf->date + root_dt / 86400;
						int duplicate = 0;
						for(int i = 0; i < counter; i++) if(fabs(new_steps[i]->date - epoch) < 1e-6) duplicate = 1;
						struct ItinStep *new_step = duplicate ? NULL : calloc(1, sizeof(*new_step));
						if(new_step != NULL) {
							OSV target = system->prop_method == ORB_ELEMENTS ?
								osv_from_elements(tf->body->orbit, epoch) :
								osv_from_ephem(tf->body->ephem, tf->body->num_ephems, epoch, system->cb);
							new_step->body = tf->body;
							new_step->date = epoch;
							new_step->r = target.r;
							new_step->v_body = target.v;
							new_step->v_dep = root_leg.v0;
							new_step->v_arr = root_leg.v1;
							new_step->prev = tf;
							new_steps[counter++] = new_step;
						}
					}
				}
				previous_dt = dt;
				previous_difference = difference;
				has_previous = valid;
				if(dt >= window_end) break;
			}
		}
	}
	if(counter == 0) return;
	struct ItinStep **next = realloc(tf->next, (size_t)(tf->num_next_nodes + counter) * sizeof(*next));
	if(next == NULL) {
		for(int i = 0; i < counter; i++) free(new_steps[i]);
		return;
	}
	tf->next = next;
	for(int i = 0; i < counter; i++) tf->next[tf->num_next_nodes + i] = new_steps[i];
	tf->num_next_nodes += counter;
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
				double minimum_periapsis = tf->body->radius + tf->body->atmo_alt + 1e4;
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





struct ItinStep * get_first(struct ItinStep *tf) {
	if(tf == NULL) return NULL;
	while(tf->prev != NULL) tf = tf->prev;
	return tf;
}

struct ItinStep * get_last(struct ItinStep *tf) {
	if(tf == NULL) return NULL;
	while(tf->next != NULL) tf = tf->next[0];
	return tf;
}


void print_itinerary(struct ItinStep *itin) {
	if(itin->prev != NULL) {
		print_itinerary(itin->prev);
		printf(" - ");
	}
	print_date(convert_JD_date(itin->date, DATE_ISO), 0);
}

int get_number_of_itineraries(struct ItinStep *itin) {
	if(itin == NULL || (itin->prev == NULL && itin->num_next_nodes == 0)) return 0;
	if(itin->num_next_nodes == 0) return 1;
	int counter = 0;
	for(int i = 0; i < itin->num_next_nodes; i++) {
		counter += get_number_of_itineraries(itin->next[i]);
	}
	return counter;
}

int get_total_number_of_stored_steps(struct ItinStep *itin) {
	if(itin == NULL) return 0;
	if(itin->num_next_nodes == 0) return 1;
	int counter = 1;
	for(int i = 0; i < itin->num_next_nodes; i++) {
		counter += get_total_number_of_stored_steps(itin->next[i]);
	}
	return counter;
}

void store_itineraries_in_array(struct ItinStep *itin, struct ItinStep **array, int *index) {
	if(itin->num_next_nodes == 0) {
		array[*index] = itin;
		(*index)++;
	}

	for(int i = 0; i < itin->num_next_nodes; i++) {
		store_itineraries_in_array(itin->next[i], array, index);
	}
}

double get_itinerary_duration(struct ItinStep *itin) {
	double jd1 = itin->date;
	while(itin->prev != NULL) itin = itin->prev;
	double jd0 = itin->date;
	return jd1-jd0;
}

struct PorkchopPoint *create_porkchop_array_from_departures(struct ItinStep **departures, int num_deps, double dep_periapsis, double arr_periapsis) {
	int num_itins = 0;
	for(int i = 0; i < num_deps; i++) num_itins += get_number_of_itineraries(departures[i]);

	struct ItinStep **arrivals = (struct ItinStep**) malloc(num_itins * sizeof(struct ItinStep*));
	int index = 0;
	for(int i = 0; i < num_deps; i++) store_itineraries_in_array(departures[i], arrivals, &index);
	struct PorkchopPoint *porkchop_points = malloc(num_itins * sizeof(struct PorkchopPoint));
	for(int i = 0; i < num_itins; i++) {
		porkchop_points[i] = create_porkchop_point(arrivals[i], dep_periapsis, arr_periapsis);
	}
	free(arrivals);

	return porkchop_points;
}

struct PorkchopPoint create_porkchop_point(struct ItinStep *itin, double dep_periapsis, double arr_periapsis) {
	struct PorkchopPoint pp;
	pp.arrival = itin;
	pp.dur = get_itinerary_duration(itin);

	double vinf = mag_vec3(subtract_vec3(itin->v_arr, itin->v_body));
	pp.dv_arr_cap = dv_capture(itin->body, alt2radius(itin->body, arr_periapsis), vinf);
	pp.dv_arr_circ = dv_circ(itin->body, alt2radius(itin->body, arr_periapsis), vinf);

	pp.dv_dsm = 0;
	pp.dv_flyby = 0;
	while(itin->prev->prev != NULL) {
		if(itin->body == NULL)
			pp.dv_dsm += mag_vec3(subtract_vec3(itin->next[0]->v_dep, itin->v_arr));
		// A DSM right after a flyby must not hide that flyby's burn.
		if(itin->prev->body != NULL && itin->prev->prev != NULL) {
			struct ItinStep *flyby = itin->prev;
			PoweredFlybyResult flyby_result;
			double minimum_periapsis = flyby->body->radius + flyby->body->atmo_alt + 1e4;
			if(calculate_powered_flyby(flyby->v_arr, itin->v_dep, flyby->v_body,
				flyby->body, minimum_periapsis, &flyby_result))
				pp.dv_flyby += flyby_result.burn_delta_v;
			else
				pp.dv_flyby = HUGE_VAL;
		}
		itin = itin->prev;
	}
	// The loop stops at the first-leg node, so a DSM on the first leg is counted here.
	if(itin->body == NULL && itin->next != NULL && itin->num_next_nodes > 0)
		pp.dv_dsm += mag_vec3(subtract_vec3(itin->next[0]->v_dep, itin->v_arr));

	pp.dep_date = itin->prev->date;
	Vector3 departure_vinf = subtract_vec3(itin->v_dep, itin->prev->v_body);
	DepartureBurn3D departure_burn;
	if(calculate_free_plane_departure_burn(departure_vinf, itin->prev->body,
		alt2radius(itin->prev->body, dep_periapsis), &departure_burn))
		pp.dv_dep = departure_burn.burn_magnitude;
	else
		pp.dv_dep = dv_circ(itin->prev->body, alt2radius(itin->prev->body, dep_periapsis),
			mag_vec3(departure_vinf));
	return pp;
}

int calc_next_spec_itin_step(struct ItinStep *curr_step, CelestSystem *system, Body **bodies, double jd_max_arr, struct Dv_Filter *dv_filter, int num_steps, int step) {
	if(itinerary_search_cancelled(NULL)) return curr_step->num_next_nodes > 0;
	double max_duration = jd_max_arr-curr_step->date;
	double min_duration = MIN_TRANSFER_DURATION;
	if(max_duration > min_duration && get_thread_counter(3) == 0) {
		if(bodies[step] != bodies[step - 1]) find_viable_flybys(curr_step, system, bodies[step], min_duration * 86400, max_duration * 86400);
		else {
			printf("DSB not yet reimplemented!\n");
			//		find_viable_dsb_flybys(curr_step, &bodies[step+1]->ephem, bodies[step+1],
			//							   min_duration[step-1]*86400, max_duration[step-1]*86400, min_duration[step]*86400, max_duration[step]*86400);
		}

		for(int i = 0; i < curr_step->num_next_nodes && !itinerary_search_cancelled(NULL); i++) {
			struct ItinStep *next = bodies[step] != bodies[step - 1] ? curr_step->next[i] : curr_step->next[i]->next[0]->next[0];
			if(step == num_steps - 1) {
				struct PorkchopPoint porkchop_point = create_porkchop_point(next, dv_filter->dep_periapsis, dv_filter->arr_periapsis);
				double dv_sat = porkchop_point.dv_dsm;
				if(dv_filter->last_transfer_type == 1) dv_sat += porkchop_point.dv_arr_cap;
				if(dv_filter->last_transfer_type == 2) dv_sat += porkchop_point.dv_arr_circ;
						if(dv_sat + porkchop_point.dv_flyby > dv_filter->max_satdv ||
							porkchop_point.dv_dep + dv_sat + porkchop_point.dv_flyby > dv_filter->max_totdv) {
					if(curr_step->num_next_nodes <= 1) {
						remove_step_from_itinerary(next);
						return 0;
					} else {
						remove_step_from_itinerary(next);
					}
					i--;
				}
			}
		}
	}

	if(curr_step->num_next_nodes == 0) {
		remove_step_from_itinerary(curr_step);
		return 0;
	}

	int num_valid = 0;
	int init_num_nodes = curr_step->num_next_nodes;
	int step_del = 0;
	int result;

	if(step < num_steps-1) {
		for(int i = 0; i < init_num_nodes && !itinerary_search_cancelled(NULL); i++) {
			if(bodies[step] != bodies[step-1]) result = calc_next_spec_itin_step(curr_step->next[i - step_del], system, bodies, jd_max_arr, dv_filter, num_steps, step + 1);
			else result = calc_next_spec_itin_step(curr_step->next[i - step_del]->next[0]->next[0], system, bodies, jd_max_arr, dv_filter, num_steps, step + 2);
			num_valid += result;
			if(!result) step_del++;
		}
	} else return 1;
	if(itinerary_search_cancelled(NULL)) return 1;

	return num_valid > 0;
}

int calc_next_itin_to_target_step(struct ItinStep *curr_step, struct ItinSequenceInfoToTarget *seq_info, double jd_max_arr, double max_total_duration, struct Dv_Filter *dv_filter) {
	if(itinerary_search_cancelled(NULL)) return curr_step->num_next_nodes > 0;
	for(int i = 0; i < seq_info->num_flyby_bodies && !itinerary_search_cancelled(NULL); i++) {
		if(get_thread_counter(3) > 0) break;
		if(seq_info->flyby_bodies[i] == curr_step->body) {
			// Only one resonant return per visit, and none once the route has reached its target.
			if(!seq_info->allow_resonant || curr_step->prev == NULL || curr_step->prev->body == curr_step->body ||
				curr_step->body == seq_info->arr_body) continue;
		}
		double jd_max;
		if(get_first(curr_step)->date + max_total_duration < jd_max_arr)
			jd_max = get_first(curr_step)->date + max_total_duration;
		else
			jd_max = jd_max_arr;
		double max_duration = jd_max-curr_step->date;
		double min_duration = MIN_TRANSFER_DURATION;
		if(max_duration > min_duration)
			find_viable_flybys(curr_step, seq_info->system, seq_info->flyby_bodies[i], min_duration*86400, max_duration*86400);
	}


	if(itinerary_search_cancelled(NULL)) return curr_step->num_next_nodes > 0;
	if(curr_step->num_next_nodes == 0) {
		remove_step_from_itinerary(curr_step);
		return 0;
	}

	int num_of_end_nodes = find_copy_and_store_end_nodes(curr_step, seq_info->arr_body);

	// remove end nodes that do not satisfy dv requirements
	num_of_end_nodes = remove_end_nodes_that_do_not_satisfy_dv_requirements(curr_step, num_of_end_nodes, dv_filter);
	if(curr_step->num_next_nodes <= 0) return 0;

	// returns 1 if has valid steps (0 otherwise)
	return continue_to_next_steps_and_check_for_valid_itins(curr_step, num_of_end_nodes, seq_info, jd_max_arr, max_total_duration, dv_filter);
}

int continue_to_next_steps_and_check_for_valid_itins(struct ItinStep *curr_step, int num_of_end_nodes, struct ItinSequenceInfoToTarget *seq_info, double jd_max_arr, double max_total_duration, struct Dv_Filter *dv_filter) {
	int num_valid = num_of_end_nodes;
	int init_num_nodes = curr_step->num_next_nodes;
	int step_del = 0;
	int has_valid_init;

	for(int i = 0; i < init_num_nodes-num_of_end_nodes && !itinerary_search_cancelled(NULL); i++) {
		int idx = i - step_del;
		has_valid_init = calc_next_itin_to_target_step(curr_step->next[idx], seq_info, jd_max_arr, max_total_duration, dv_filter);
		num_valid += has_valid_init;
		if(!has_valid_init) step_del++;
	}

	return itinerary_search_cancelled(NULL) || num_valid > 0;
}

int find_copy_and_store_end_nodes(struct ItinStep *curr_step, struct Body *arr_body) {
	int num_of_end_nodes = 0;
	for(int i = 0; i < curr_step->num_next_nodes; i++) {
		if(curr_step->next[i]->body == arr_body) num_of_end_nodes++;
	}
	// if there were end nodes found, copy them and store them at the end
	if(num_of_end_nodes > 0) {
		struct ItinStep ** new_next = realloc(curr_step->next, (num_of_end_nodes+curr_step->num_next_nodes) * sizeof(struct ItinStep *));
		if(new_next == NULL) {
			printf("\nERROR WHILE REALLOCATING ITINERARIES DURING END NODE COPYING!!!\n");
			return 0;
		}
		curr_step->next = new_next;

		int end_node_idx = 0;

		for(int i = 0; i < curr_step->num_next_nodes; i++) {
			if(curr_step->next[i]->body == arr_body) {
				struct ItinStep *end_node = malloc(num_of_end_nodes * sizeof(struct ItinStep));
				end_node->body 	= curr_step->next[i]->body;
				end_node->date 	= curr_step->next[i]->date;
				end_node->r 	= curr_step->next[i]->r;
				end_node->v_dep = curr_step->next[i]->v_dep;
				end_node->v_arr = curr_step->next[i]->v_arr;
				end_node->v_body= curr_step->next[i]->v_body;
				end_node->num_next_nodes = 0;
				end_node->prev 	= curr_step->next[i]->prev;
				end_node->next 	= NULL;

				curr_step->next[curr_step->num_next_nodes + end_node_idx] = end_node;
				end_node_idx++;
			}
		}
	}

	curr_step->num_next_nodes += num_of_end_nodes;
	return num_of_end_nodes;
}

int remove_end_nodes_that_do_not_satisfy_dv_requirements(struct ItinStep *curr_step, int num_of_end_nodes, struct Dv_Filter *dv_filter) {
	for(int i = curr_step->num_next_nodes-num_of_end_nodes; i < curr_step->num_next_nodes; i++) {
		struct ItinStep *next = curr_step->next[i];
		struct PorkchopPoint porkchop_point = create_porkchop_point(next, dv_filter->dep_periapsis, dv_filter->arr_periapsis);
			double dv_sat = porkchop_point.dv_dsm;
		if(dv_filter->last_transfer_type == 1) dv_sat += porkchop_point.dv_arr_cap;
		if(dv_filter->last_transfer_type == 2) dv_sat += porkchop_point.dv_arr_circ;
			if(dv_sat + porkchop_point.dv_flyby > dv_filter->max_satdv ||
				porkchop_point.dv_dep + dv_sat + porkchop_point.dv_flyby > dv_filter->max_totdv) {
			if(curr_step->num_next_nodes <= 1) {
				remove_step_from_itinerary(next);
				curr_step->num_next_nodes = 0;
			} else {
				remove_step_from_itinerary(next);
			}
			i--;
			num_of_end_nodes--;
		}
	}
	return num_of_end_nodes;
}

int get_num_of_itin_layers(struct ItinStep *step) {
	int counter = 0;
	while(step != NULL) {
		counter++;
		if(step->next != NULL) step = step->next[0];
		else break;
	}
	return counter;
}

void update_itin_body_osvs(struct ItinStep *step, CelestSystem *system) {
	OSV body_osv;
	while(step != NULL) {
		if(step->body != NULL) {
			body_osv = system->prop_method == ORB_ELEMENTS ?
					osv_from_elements(step->body->orbit, step->date) :
					osv_from_ephem(step->body->ephem, step->body->num_ephems, step->date, system->cb);
			step->r = body_osv.r;
			step->v_body = body_osv.v;
		} else step->v_body = vec3(1, 0, 0);	// non-zero marks a valid maneuver node so its trajectory is drawn
		step = step->next != NULL ? step->next[0] : NULL;
	}
}

void calc_itin_v_vectors_from_dates_and_r(struct ItinStep *step, CelestSystem *system) {
	if(step == NULL) return;
	struct ItinStep *next;
	while(step->next != NULL) {
		next = step->next[0];
		// A deep-space maneuver node keeps its stored position, so its two arcs are ordinary Lambert legs.
		double dt = (next->date - step->date) * 86400;
		Lambert3 transfer = calc_lambert3(step->r, next->r, dt, system->cb);
		next->v_dep = transfer.v0;
		next->v_arr = transfer.v1;
		step = next;
	}
}

void copy_step_body_vectors_and_date(struct ItinStep *orig_step, struct ItinStep *step_copy) {
	step_copy->body = orig_step->body;
	step_copy->r = orig_step->r;
	step_copy->v_body = orig_step->v_body;
	step_copy->v_arr = orig_step->v_arr;
	step_copy->v_dep = orig_step->v_dep;
	step_copy->date = orig_step->date;
}

struct ItinStep * create_itin_copy(struct ItinStep *step) {
	struct ItinStep *new_step = (struct ItinStep*) malloc(sizeof(struct ItinStep));
	copy_step_body_vectors_and_date(step, new_step);
	new_step->prev = NULL;
	new_step->num_next_nodes = step->num_next_nodes;
	if(step->num_next_nodes > 0) {
		new_step->next = (struct ItinStep **) malloc(step->num_next_nodes * sizeof(struct ItinStep *));
		for(int i = 0; i < new_step->num_next_nodes; i++) {
			new_step->next[i] = create_itin_copy(step->next[i]);
			new_step->next[i]->prev = new_step;
		}
	} else new_step->next = NULL;
	return new_step;
}

struct ItinStep * create_itin_copy_from_arrival(struct ItinStep *step) {
	struct ItinStep *step_copy = (struct ItinStep*) malloc(sizeof(struct ItinStep));
	copy_step_body_vectors_and_date(step, step_copy);
	step_copy->next = NULL;
	step_copy->num_next_nodes = 0;

	while(step->prev != NULL) {
		step_copy->prev = (struct ItinStep*) malloc(sizeof(struct ItinStep));
		step_copy->prev->next = (struct ItinStep**) malloc(sizeof(struct ItinStep*));
		step_copy->prev->next[0] = step_copy;
		step_copy->prev->num_next_nodes = 1;

		step = step->prev;
		step_copy = step_copy->prev;
		copy_step_body_vectors_and_date(step, step_copy);
	}


	step_copy->prev = NULL;
	return get_last(step_copy);
}

int is_valid_itinerary(struct ItinStep *step) {
	if(step == NULL || step->body == NULL || step->prev == NULL) return 0;
	while(step != NULL) {
		if(step->body == NULL && step->v_body.x == 0) return 0;
		step = step->prev;
	}

	return 1;
}

void store_step_in_file(struct ItinStep *step, FILE *file, int layer, int variation) {
	fprintf(file, "#%d#%d\n", layer, variation);
	fprintf(file, "Date: %f\n", step->date);
	fprintf(file, "r: %lf, %lf, %lf\n", step->r.x, step->r.y, step->r.z);
	fprintf(file, "v_dep: %f, %f, %f\n", step->v_dep.x, step->v_dep.y, step->v_dep.z);
	fprintf(file, "v_arr: %f, %f, %f\n", step->v_arr.x, step->v_arr.y, step->v_arr.z);
	fprintf(file, "v_body: %f, %f, %f\n", step->v_body.x, step->v_body.y, step->v_body.z);
	fprintf(file, "Next Steps: %d\n", step->num_next_nodes);

	for(int i = 0; i < step->num_next_nodes; i++) {
		store_step_in_file(step->next[i], file, layer + 1, i);
	}
}

void store_itineraries_in_file(struct ItinStep **departures, int num_nodes, int num_deps) {
	char filename[50];  // 14 for date + 4 for .csv + 1 for string terminator
	sprintf(filename, "./Itineraries/test.transfer");
	int num_steps = get_num_of_itin_layers(departures[0]);
	printf("Filesize: ~%.3f MB\n", (double)num_nodes*240/1e6);

	FILE *file;
	file = fopen(filename,"w");

	fprintf(file, "Number of stored nodes: %d\n", num_nodes);
	fprintf(file, "Number of Departures: %d, Number of Steps: %d\n", num_deps, num_steps);

	struct ItinStep *ptr = departures[0];

	fprintf(file, "Bodies: ");
	while(ptr != NULL) {
		if(ptr->body != NULL) fprintf(file, "%d", ptr->body->id);
		else fprintf(file, "-");
		if(ptr->next != NULL) {
			fprintf(file, ",");
			ptr = ptr->next[0];
		} else {
			fprintf(file, "#\n");
			break;
		}
	}

	for(int i = 0; i < num_deps; i++) {
		store_step_in_file(departures[i], file, 0, i);
	}

	fclose(file);
}

// Heliocentric radial (outward), tangential (prograde) and normal components of a maneuver node's burn.
static Vector3 dsm_burn_rtn(const struct ItinStep *step) {
	if(step->next == NULL || step->prev == NULL) return vec3(0, 0, 0);
	Vector3 burn = subtract_vec3(step->next[0]->v_dep, step->v_arr);
	Vector3 radial = norm_vec3(step->r);
	Vector3 normal = norm_vec3(cross_vec3(step->r, step->v_arr));
	Vector3 tangential = cross_vec3(normal, radial);
	return vec3(dot_vec3(burn, radial), dot_vec3(burn, tangential), dot_vec3(burn, normal));
}

void itinerary_short_overview_to_string(struct ItinStep *step, enum DateType date_type, double dep_periapsis, double arr_periapsis, char *string) {
	sprintf(string, "");
	if(step == NULL) return;
	step = get_first(step);
	if(step->next == NULL) return;
	char date_string[32];
	while(step != NULL) {
		if(step->prev != NULL) sprintf(string, "%s - ", string);
		sprintf(string, "%s%s", string, step->body != NULL ? step->body->name : "DSM");
		if(step->next != NULL) step = step->next[0];
		else break;
	}
	
	date_to_string(convert_JD_date(get_first(step)->date, date_type), date_string, 1);
	sprintf(string, "%s\nDeparture: %s\n", string, date_string);
	date_to_string(convert_JD_date(get_last(step)->date, date_type), date_string, 1);
	double dt = get_last(step)->date-get_first(step)->date;
	if(date_type == DATE_KERBAL) dt *= 4;
	sprintf(string, "%sArrival: %s\nDuration: %.2f days\n", string, date_string, dt);
	
	struct PorkchopPoint pp = create_porkchop_point(get_last(step), dep_periapsis, arr_periapsis);
	
	sprintf(string, "%s\nDeparture dv: %.2f m/s (Periapsis: %.2fkm)\n", string, pp.dv_dep, dep_periapsis/1e3);
	sprintf(string, "%sArrival dv: %.2f m/s  (Capture; Periapsis: %.2fkm)\n", string, pp.dv_arr_cap, arr_periapsis/1e3);
	sprintf(string, "%sArrival dv: %.2f m/s  (Circularization; Periapsis: %.2fkm)\n", string, pp.dv_arr_circ, arr_periapsis/1e3);
	sprintf(string, "%sPowered flyby burns: %.2f m/s\n", string, pp.dv_flyby);
	sprintf(string, "%sDSM burns: %.2f m/s\n", string, pp.dv_dsm);
	sprintf(string, "%sTotal dv: %.2f m/s  (Fly-By)\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby);
	sprintf(string, "%sTotal dv: %.2f m/s  (Capture)\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_cap);
	sprintf(string, "%sTotal dv: %.2f m/s  (Circularization)\n\n--\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_circ);
	
	step = get_first(step);
	int step_id = 0;
	
	while(step != NULL) {
		date_to_string(convert_JD_date(step->date, date_type), date_string, 1);
		if(step->body == NULL) {
			double maneuver_dv = step->next != NULL ? mag_vec3(subtract_vec3(step->next[0]->v_dep, step->v_arr)) : 0;
			Vector3 rtn = dsm_burn_rtn(step);
			sprintf(string, "%sDeep-Space Maneuver: | %s | Burn: %.2f m/s (radial %.2f, tangential %.2f, normal %.2f)\n", string, date_string, maneuver_dv, rtn.x, rtn.y, rtn.z);
			step = step->next != NULL ? step->next[0] : NULL;
			continue;
		}
		if(step_id == 0) sprintf(string, "%sDeparture", string);
		else if(step->next == NULL) sprintf(string, "%sArrival", string);
		else sprintf(string, "%sFly-By %d", string, step_id);
		sprintf(string, "%s (%s): | %s", string, step->body->name, date_string);
		if(step_id == 0) {
//			sprintf(string, "%sPeriapsis: %.2fkm\n", string, dep_periapsis/1e3);
			sprintf(string, "%s\n", string);
		} else if(step->next == NULL) {
//			sprintf(string, "%sPeriapsis: %.2fkm\n", string, arr_periapsis/1e3);
			sprintf(string, "%s\n", string);
		} else {
			Vector3 v_arr = step->v_arr;
			Vector3 v_dep = step->next[0]->v_dep;
			Vector3 v_body = step->v_body;
			PoweredFlybyResult flyby;
			double min_periapsis = step->body->radius + step->body->atmo_alt + 1e4;
			int valid_flyby = calculate_powered_flyby(v_arr, v_dep, v_body, step->body, min_periapsis, &flyby);
			double periapsis = valid_flyby ? flyby.periapsis_radius : get_flyby_periapsis(v_arr, v_dep, v_body, step->body);
			double burn = valid_flyby ? flyby.burn_delta_v : 0;
			sprintf(string, "%s | Periapsis: %.2fkm | Burn: %.2f m/s\n", string,
				(periapsis-step->body->radius)/1e3, burn);
		}
		if(step->next != NULL) step = step->next[0];
		else step = NULL;
		step_id++;
	}
}

void itinerary_detailed_overview_to_string(struct ItinStep *step, enum DateType date_type, double dep_periapsis, double arr_periapsis, char *string) {
	sprintf(string, "");
	if(step == NULL) return;
	step = get_first(step);
	if(step->next == NULL) return;
	char date_string[32];
	while(step != NULL) {
		if(step->prev != NULL) sprintf(string, "%s - ", string);
		sprintf(string, "%s%s", string, step->body != NULL ? step->body->name : "DSM");
		if(step->next != NULL) step = step->next[0];
		else break;
	}
	
	date_to_string(convert_JD_date(get_first(step)->date, date_type), date_string, 1);
	sprintf(string, "%s\nDeparture: %s\n", string, date_string);
	date_to_string(convert_JD_date(get_last(step)->date, date_type), date_string, 1);
	double dt = get_last(step)->date-get_first(step)->date;
	if(date_type == DATE_KERBAL) dt *= 4;
	sprintf(string, "%sArrival: %s\nDuration: %.2f days\n", string, date_string, dt);
	
	struct PorkchopPoint pp = create_porkchop_point(get_last(step), dep_periapsis, arr_periapsis);
	
	sprintf(string, "%s\nDeparture dv: %.2f m/s\n", string, pp.dv_dep);
	struct ItinStep *departure = get_first(step);
	DepartureBurn3D departure_burn;
	Vector3 departure_vinf = subtract_vec3(departure->next[0]->v_dep, departure->v_body);
	if(calculate_free_plane_departure_burn(departure_vinf, departure->body,
		alt2radius(departure->body, dep_periapsis), &departure_burn)) {
		Vector3 radial = departure_burn.periapsis_direction;
		Vector3 tangential = departure_burn.tangential_direction;
		Vector3 normal = norm_vec3(cross_vec3(radial, tangential));
		double burn_radial = dot_vec3(departure_burn.burn_delta_v, radial);
		double burn_tangential = dot_vec3(departure_burn.burn_delta_v, tangential);
		double burn_normal = dot_vec3(departure_burn.burn_delta_v, normal);
		sprintf(string, "%sInitial burn: %.2f m/s (radial %.2f, tangential %.2f, normal %.2f) "
			"[parking-orbit RTN]\n", string, departure_burn.burn_magnitude,
			burn_radial, burn_tangential, burn_normal);
	}
	sprintf(string, "%sArrival dv: %.2f m/s  (Capture)\n", string, pp.dv_arr_cap);
	sprintf(string, "%sArrival dv: %.2f m/s  (Circularization)\n", string, pp.dv_arr_circ);
	sprintf(string, "%sPowered flyby burns: %.2f m/s\n", string, pp.dv_flyby);
	sprintf(string, "%sDSM burns: %.2f m/s\n", string, pp.dv_dsm);
	sprintf(string, "%sTotal dv: %.2f m/s  (Fly-By)\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby);
	sprintf(string, "%sTotal dv: %.2f m/s  (Capture)\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_cap);
	sprintf(string, "%sTotal dv: %.2f m/s  (Circularization)\n\n--\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_circ);
	
	step = get_first(step);
	int step_id = 0;
	
	while(step != NULL) {
		date_to_string(convert_JD_date(step->date, date_type), date_string, 1);
		if(step->body == NULL) {
			double maneuver_dv = step->next != NULL ? mag_vec3(subtract_vec3(step->next[0]->v_dep, step->v_arr)) : 0;
			Vector3 rtn = dsm_burn_rtn(step);
			sprintf(string, "%s\nDeep-Space Maneuver:\nDate: %s  (T+%.2f days)\nBurn: %.2f m/s (radial %.2f, tangential %.2f, normal %.2f)\n", string,
				date_string, step->date-get_first(step)->date, maneuver_dv, rtn.x, rtn.y, rtn.z);
			step = step->next != NULL ? step->next[0] : NULL;
			continue;
		}
		if(step_id == 0) sprintf(string, "%s\nDeparture", string);
		else if(step->next == NULL) sprintf(string, "%s\nArrival", string);
		else sprintf(string, "%s\nFly-By %d", string, step_id);
		sprintf(string, "%s (%s):\nDate: %s  (T+%.2f days)\n", string, step->body->name, date_string, step->date-get_first(step)->date);
		if(step_id == 0) {
			sprintf(string, "%sPeriapsis: %.2fkm\n", string, dep_periapsis/1e3);
		} else if(step->next == NULL) {
			sprintf(string, "%sPeriapsis: %.2fkm\n", string, arr_periapsis/1e3);
		} else {
			Vector3 v_arr = step->v_arr;
			Vector3 v_dep = step->next[0]->v_dep;
			Vector3 v_body = step->v_body;
			double incl = get_flyby_inclination(v_arr, v_dep, v_body, get_body_equatorial_plane(step->body));
			PoweredFlybyResult flyby;
			double min_periapsis = step->body->radius + step->body->atmo_alt + 1e4;
			int valid_flyby = calculate_powered_flyby(v_arr, v_dep, v_body, step->body, min_periapsis, &flyby);
			double periapsis = valid_flyby ? flyby.periapsis_radius : get_flyby_periapsis(v_arr, v_dep, v_body, step->body);
			double burn = valid_flyby ? flyby.burn_delta_v : 0;
			Vector3 burn_normal = valid_flyby ?
				cross_vec3(flyby.periapsis_direction, flyby.incoming_tangent_direction) : vec3(0, 0, 0);
			double burn_radial = valid_flyby ? dot_vec3(flyby.burn_delta_v_vector, flyby.periapsis_direction) : 0;
			double burn_tangential = valid_flyby ? dot_vec3(flyby.burn_delta_v_vector, flyby.incoming_tangent_direction) : 0;
			double burn_normal_component = valid_flyby ? dot_vec3(flyby.burn_delta_v_vector, burn_normal) : 0;
			sprintf(string, "%sPeriapsis: %.2fkm\nInclination: %.2f°\n"
				"Powered burn: %.2f m/s (radial %.2f, tangential %.2f, normal %.2f)\n",
				string, (periapsis-step->body->radius)/1e3, rad2deg(incl), burn,
				burn_radial, burn_tangential, burn_normal_component);
		}
		if(step->next != NULL) step = step->next[0];
		else step = NULL;
		step_id++;
	}
}

void itinerary_step_parameters_to_string(char *s_labels, char *s_values, enum DateType date_type, double dep_periapsis, double arr_periapsis, struct ItinStep *step) {
	if(step == NULL || (step->prev == NULL && step->next == NULL)) {sprintf(s_labels,""); sprintf(s_values,""); return;}
	if(step->body == NULL) {
		double dt_in_days = step->date - get_first(step)->date;
		if(date_type == DATE_KERBAL) dt_in_days *= 4;
		Vector3 dv_vec = step->next != NULL ? subtract_vec3(step->next[0]->v_dep, step->v_arr) : vec3(0, 0, 0);
		// Components in the spacecraft frame: prograde, radial in, orbit normal.
		Vector3 prograde = norm_vec3(step->v_arr);
		Vector3 normal = norm_vec3(cross_vec3(step->r, step->v_arr));
		Vector3 radial_in = norm_vec3(cross_vec3(normal, prograde));
		sprintf(s_labels, "Deep-Space Maneuver\nT+:\nDistance to the Sun:\nDv Prograde:\nDv Radial:\nDv Normal:\nTotal:");
		sprintf(s_values, "\n%.2f days\n%.3f AU\n%.3f m/s\n%.3f m/s\n%.3f m/s\n%.3f m/s",
			dt_in_days, mag_vec3(step->r) / 1.495978707e11, dot_vec3(dv_vec, prograde),
			dot_vec3(dv_vec, radial_in), dot_vec3(dv_vec, normal), mag_vec3(dv_vec));
		return;
	}
	HyperbolaParams hyp_params;

	// is departure step
	if(step->prev == NULL) {
		hyp_params = get_hyperbola_params(vec3(0,0,0), step->next[0]->v_dep, step->v_body, step->body, dep_periapsis, HYP_DEPARTURE);
		sprintf(s_labels, "Departure\n"
						  "T+:\n"
						  "RadPer:\n"
						  "AltPer:\n"
						  "Min Incl:\n\n"
						  "C3Energy:\n"
						  "RHA (out):\n"
						  "DHA (out):");

		sprintf(s_values, "\n0 days\n"
						  "%.0f km\n"
						  "%.0f km\n"
						  "%.2f°\n\n"
						  "%.2f km²/s²\n"
						  "%.2f°\n"
						  "%.2f°",
				hyp_params.rp / 1000, (hyp_params.rp-step->body->radius) / 1000, rad2deg(angle_plane3_vec3(get_body_equatorial_plane(step->body), subtract_vec3(step->next[0]->v_dep, step->v_body))), hyp_params.c3_energy / 1e6,
				rad2deg(hyp_params.outgoing.bplane_angle), rad2deg(hyp_params.outgoing.decl));

	} else if(step->num_next_nodes == 0) {
		double dt_in_days = step->date - get_first(step)->date;
		if(date_type == DATE_KERBAL) dt_in_days *= 4;
		hyp_params = get_hyperbola_params(step->v_arr, vec3(0,0,0), step->v_body, step->body, arr_periapsis, HYP_ARRIVAL);
		sprintf(s_labels, "Arrival\n"
						  "T+:\n"
						  "RadPer:\n"
						  "AltPer:\n"
						  "Min Incl:\n"
						  "Max Incl:\n\n"
						  "C3Energy:\n"
						  "RHA (in):\n"
						  "DHA (in):");

		sprintf(s_values, "\n%.2f days\n"
						  "%.0f km\n"
						  "%.0f km\n"
						  "%.2f°\n"
						  "%.2f°\n\n"
						  "%.2f km²/s²\n"
						  "%.2f°\n"
						  "%.2f°",
				dt_in_days, hyp_params.rp / 1000, (hyp_params.rp-step->body->radius) / 1000, rad2deg(angle_plane3_vec3(get_body_equatorial_plane(step->body), subtract_vec3(step->v_arr, step->v_body))), 180.0 - fabs(rad2deg(angle_plane3_vec3(get_body_equatorial_plane(step->body), subtract_vec3(step->v_arr, step->v_body)))), hyp_params.c3_energy / 1e6,
				rad2deg(hyp_params.incoming.bplane_angle), rad2deg(hyp_params.incoming.decl));

	} else {
		if(step->body != NULL) {
			Vector3 v_arr = step->v_arr;
			Vector3 v_dep = step->next[0]->v_dep;
			Vector3 v_body = step->v_body;
			double incl = get_flyby_inclination(v_arr, v_dep, v_body, get_body_equatorial_plane(step->body));

			hyp_params = get_hyperbola_params(step->v_arr, step->next[0]->v_dep,
											  step->v_body, step->body, 0, HYP_FLYBY);
			// The unpowered periapsis can lie below the surface; the route actually flies the powered one.
			PoweredFlybyResult powered_flyby;
			int powered_valid = calculate_powered_flyby(v_arr, v_dep, v_body, step->body,
				step->body->radius + step->body->atmo_alt + 1e4, &powered_flyby);
			double periapsis_radius = powered_valid ? powered_flyby.periapsis_radius : hyp_params.rp;
			double flyby_burn = powered_valid ? powered_flyby.burn_delta_v : 0;
			double dt_in_days = step->date - get_first(step)->date;
			if(date_type == DATE_KERBAL) dt_in_days *= 4;

			sprintf(s_labels, "Hyperbola\n"
							  "T+:\n"
							  "RadPer:\n"
							  "AltPer:\n"
							  "Burn:\n"
							  "Unpowered AltPer:\n"
							  "Inclination:\n\n"
							  "C3Energy:\n"
							  "RHA (in):\n"
							  "DHA (in):\n"
							  "BVAZI (in):\n"
							  "RHA (out):\n"
							  "DHA (out):\n"
							  "BVAZI (out):");

			sprintf(s_values, "\n%.2f days\n"
							  "%.0f km\n"
							  "%.0f km\n"
							  "%.1f m/s\n"
							  "%.0f km%s\n"
							  "%.2f°\n\n"
							  "%.2f km²/s²\n"
							  "%.2f°\n"
							  "%.2f°\n"
							  "%.2f°\n"
							  "%.2f°\n"
							  "%.2f°\n"
							  "%.2f°",
					dt_in_days, periapsis_radius / 1000, (periapsis_radius-step->body->radius) / 1000, flyby_burn, (hyp_params.rp - step->body->radius) / 1000, flyby_periapsis_shortfall(step) > 0 ? " (!)" : "", rad2deg(incl),
					hyp_params.c3_energy / 1e6,
					rad2deg(hyp_params.incoming.bplane_angle), rad2deg(hyp_params.incoming.decl),
					rad2deg(hyp_params.incoming.bvazi),
					rad2deg(hyp_params.outgoing.bplane_angle), rad2deg(hyp_params.outgoing.decl),
					rad2deg(hyp_params.outgoing.bvazi));
		} else {
			double dt_in_days = step->date - get_first(step)->date;
			if(date_type == DATE_KERBAL) dt_in_days *= 4;
			double dist_to_sun = mag_vec3(step->r);

			Vector3 orbit_prograde = step->v_arr;
			Vector3 orbit_normal = cross_vec3(step->r, step->v_arr);
			Vector3 orbit_radialin = cross_vec3(orbit_normal, step->v_arr);
			Vector3 dv_vec = subtract_vec3(step->next[0]->v_dep, step->v_arr);

			// dv vector in S/C coordinate system (prograde, radial in, normal) (sign it if projected vector more than 90° from target vector / pointing in opposite direction)
			Vector3 dv_vec_sc = {
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_prograde)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_prograde), orbit_prograde) < M_PI/2 ? 1 : -1),
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_radialin)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_radialin), orbit_radialin) < M_PI/2 ? 1 : -1),
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_normal)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_normal), orbit_normal) < M_PI/2 ? 1 : -1)
			};

			sprintf(s_labels, "DSM\n"
							  "T+:\n"
							  "Distance to the Sun:\n"
							  "Dv Prograde:\n"
							  "Dv Radial:\n"
							  "Dv Normal:\n"
							  "Total:");

			sprintf(s_values, "\n%.2f days\n"
							  "%.3f AU\n"
							  "%.3f m/s\n"
							  "%.3f m/s\n"
							  "%.3f m/s\n"
							  "%.3f m/s",
					dt_in_days, dist_to_sun / 1.495978707e11, dv_vec_sc.x, dv_vec_sc.y, dv_vec_sc.z, mag_vec3(dv_vec_sc));
		}
	}
}


double flyby_periapsis_shortfall(struct ItinStep *step) {
	if(step == NULL || step->body == NULL || step->prev == NULL || step->next == NULL) return 0;
	double minimum_radius = step->body->radius + step->body->atmo_alt;
	double unpowered = get_flyby_periapsis(step->v_arr, step->next[0]->v_dep, step->v_body, step->body);
	if(!isfinite(unpowered)) return 0;
	return unpowered < minimum_radius ? minimum_radius - unpowered : 0;
}

int flyby_below_min_safe(struct ItinStep *step) {
	if(step == NULL || step->body == NULL || step->prev == NULL || step->next == NULL) return 0;
	// A burn can raise the periapsis, so the flown periapsis is below safe only when the burn cannot fix it.
	PoweredFlybyResult flyby;
	if(calculate_powered_flyby(step->v_arr, step->next[0]->v_dep, step->v_body, step->body,
			step->body->radius + step->body->atmo_alt + 1e4, &flyby)) return 0;
	double unpowered = get_flyby_periapsis(step->v_arr, step->next[0]->v_dep, step->v_body, step->body);
	return isfinite(unpowered) && unpowered < step->body->radius + step->body->atmo_alt;
}

int count_flybys_below_min_safe(struct ItinStep *step_in_route) {
	int count = 0;
	for(struct ItinStep *step = get_first(step_in_route); step != NULL; step = step->next != NULL ? step->next[0] : NULL)
		count += flyby_below_min_safe(step);
	return count;
}

int count_flybys_needing_burn(struct ItinStep *step_in_route, double *largest_shortfall) {
	int count = 0;
	double largest = 0;
	for(struct ItinStep *step = get_first(step_in_route); step != NULL; step = step->next != NULL ? step->next[0] : NULL) {
		double shortfall = flyby_periapsis_shortfall(step);
		if(shortfall > 0) {
			count++;
			largest = fmax(largest, shortfall);
		}
	}
	if(largest_shortfall != NULL) *largest_shortfall = largest;
	return count;
}

double itinerary_step_dv(struct ItinStep *step, double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type) {
	if(step == NULL || (step->prev == NULL && step->next == NULL)) return 0;
	if(step->body == NULL) {
		if(step->next == NULL || step->prev == NULL) return 0;
		return mag_vec3(subtract_vec3(step->next[0]->v_dep, step->v_arr));
	}
	if(step->prev == NULL) {
		Vector3 vinf = subtract_vec3(step->next[0]->v_dep, step->v_body);
		DepartureBurn3D burn;
		if(calculate_free_plane_departure_burn(vinf, step->body, alt2radius(step->body, dep_periapsis), &burn))
			return burn.burn_magnitude;
		return dv_circ(step->body, alt2radius(step->body, dep_periapsis), mag_vec3(vinf));
	}
	if(step->next == NULL) {
		double vinf = mag_vec3(subtract_vec3(step->v_arr, step->v_body));
		if(last_transfer_type == TF_CAPTURE) return dv_capture(step->body, alt2radius(step->body, arr_periapsis), vinf);
		if(last_transfer_type == TF_CIRC) return dv_circ(step->body, alt2radius(step->body, arr_periapsis), vinf);
		return 0;
	}
	PoweredFlybyResult flyby;
	double minimum_periapsis = step->body->radius + step->body->atmo_alt + 1e4;
	if(calculate_powered_flyby(step->v_arr, step->next[0]->v_dep, step->v_body, step->body, minimum_periapsis, &flyby))
		return flyby.burn_delta_v;
	return HUGE_VAL;
}

void set_itinerary_step_date(struct ItinStep *step, CelestSystem *system, double date) {
	if(step->body == NULL && step->prev != NULL)
		step->r = propagate_osv_time((OSV){step->prev->r, step->v_dep}, system->cb, (date - step->prev->date) * 86400).r;
	step->date = date;
	for(struct ItinStep *node = step; node->next != NULL; node = node->next[0])
		if(node->next[0]->date <= node->date) node->next[0]->date = node->date + 1;
}

void remove_step_from_itinerary(struct ItinStep *step) {
	struct ItinStep *prev = step->prev;

	while(prev->num_next_nodes == 1 && step->prev->prev != NULL) {
		step = prev;
		prev = step->prev;
	}

	int index = 0;
	for(int i = 0; i < prev->num_next_nodes; i++) {
		if(prev->next[i] == step) { index = i; break; }
	}
	prev->num_next_nodes--;
	for(int i = index; i < prev->num_next_nodes; i++) {
		prev->next[i] = prev->next[i+1];
	}
	free_itinerary(step);
}

void free_itinerary(struct ItinStep *step) {
	if(step == NULL) return;
	if(step->next != NULL) {
		for(int i = 0; i < step->num_next_nodes; i++) {
			free_itinerary(step->next[i]);
		}
		free(step->next);
	}
	free(step);
}
