#include "powered_route.h"
#include "search_cancel.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define POWERED_TIMING_START_COUNT 3
#define POWERED_MULTI_START_ROUTE_LIMIT 200
#define POWERED_OBJECTIVES_PER_ROUTE 96
#define POWERED_REPAIR_OBJECTIVES_PER_ROUTE 144


static double wall_seconds(void) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return now.tv_sec + now.tv_usec / 1e6;
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
		uint64_t *random) {
	size_t n = candidate->num_steps;
	double *best_times = malloc(n * sizeof(*best_times));
	OSV *trial_states = calloc(n, sizeof(*trial_states));
	Lambert3 *trial_legs = calloc(n - 1, sizeof(*trial_legs));
	if(best_times == NULL || trial_states == NULL || trial_legs == NULL) goto cleanup;
	memcpy(best_times, candidate->times, n * sizeof(*best_times));
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
				double fraction = (sample_random(random) >> 11) * (1.0 / 9007199254740992.0);
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


int optimize_powered_flyby_routes(struct ItinStep **departures, int num_departures,
		const Itin_Calc_Data *calc_data, int max_routes, PoweredFlybyOptimizationStats *stats) {
	SampleReservoir challenge = {.random = UINT64_C(0x2545f4914f6cdd1d)};
	RepairPool *repair_pool = calloc(1, sizeof(*repair_pool));
	if(repair_pool != NULL) repair_pool->random = UINT64_C(0x9e3779b97f4a7c15);
	ExcludedPools excluded = {.challenge = &challenge};
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
		if(!initialize_candidate(candidate, arrivals[point_index], departures, num_departures,
			calc_data->seq_info.to_target.system)) {
			destroy_candidate(candidate);
			continue;
		}
		candidate->best_total_dv = evaluate_route(candidate, calc_data, candidate->times,
			candidate->best_states, candidate->best_legs);
		if(!isfinite(candidate->best_total_dv) || candidate->best_total_dv >= DBL_MAX) {
			candidate->best_total_dv = DBL_MAX;
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
		repair_candidate_timing(candidate, calc_data, &repair_pool->random);
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
		for(int candidate_index = 0; candidate_index < top_count; candidate_index++)
			destroy_candidate(candidates[candidate_index]);
		free(candidates);
		return 0;
	}

	int improvements = 0;
	int candidate_index = 0;
	double timing_refinement_start = wall_seconds();
	int multi_start_routes = 0;
	int resonant_dsm_routes = 0;
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
		// Resonant routes need a maneuver to be useful but rarely rank in the overall top, so they get extra slots.
		int dsm_slot = candidate_index < POWERED_DSM_ROUTE_LIMIT;
		if(!dsm_slot && candidate->leg_variants != NULL && resonant_dsm_routes < POWERED_DSM_RESONANT_ROUTE_LIMIT) {
			dsm_slot = 1;
			resonant_dsm_routes++;
		}
		if(optimized && calc_data->allow_dsm && dsm_slot) {
			double cost_without_dsm = candidate->best_total_dv;
			objective_budget = POWERED_DSM_OBJECTIVES_PER_ROUTE;
			optimize_candidate_dsm(candidate, calc_data);
			objective_budget = -1;
			if(stats != NULL) {
				stats->dsm_routes_optimized++;
				if(candidate->dsms != NULL) {
					stats->dsm_routes_improved++;
					stats->best_dsm_saving = fmax(stats->best_dsm_saving,
						cost_without_dsm - candidate->best_total_dv);
				}
			}
		}
		if(optimized &&
			replace_candidate_route(departures, candidate)) {
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
	return improvements;
}
