#include "powered_route.h"
#include "flyby_policy.h"
#include "search_cancel.h"
#include "transfer_search_internal.h"
#include <fenv.h>
#include <stdatomic.h>
#include <stdio.h>
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

static int insert_top_candidate_timed(PoweredRouteCandidate *candidate, double cost,
		int max_routes, int *count,
		PoweredRouteCandidate *candidates[POWERED_ROUTE_REFINEMENT_LIMIT],
		double costs[POWERED_ROUTE_REFINEMENT_LIMIT], ExcludedPools *excluded,
		double *ranking_seconds) {
	if(ranking_seconds == NULL)
		return insert_top_candidate(candidate, cost, max_routes, count, candidates, costs, excluded);
	double start = wall_seconds();
	int inserted = insert_top_candidate(candidate, cost, max_routes, count, candidates, costs, excluded);
	*ranking_seconds += wall_seconds() - start;
	return inserted;
}

static void retain_excluded_timed(ExcludedPools *excluded, PoweredRouteCandidate *candidate,
		double *ranking_seconds) {
	if(ranking_seconds == NULL) {
		retain_excluded(excluded, candidate);
		return;
	}
	double start = wall_seconds();
	retain_excluded(excluded, candidate);
	*ranking_seconds += wall_seconds() - start;
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
				double lower = candidate->times[i - 1] + powered_min_leg_days(calc_data);
				double upper = latest_arrival;
				if(i + 1 < candidate->num_steps)
					upper = fmin(upper, candidate->times[i + 1] - powered_min_leg_days(calc_data));
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
				double lower = trial.times[i - 1] + powered_min_leg_days(calc_data);
				double upper = latest_arrival;
				if(i + 1 < num_steps)
					upper = fmin(upper, trial.times[i + 1] - powered_min_leg_days(calc_data));
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
				double lower = candidate->times[i - 1] + powered_min_leg_days(calc_data);
				double upper = latest - (n - 1 - i) * powered_min_leg_days(calc_data);
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

typedef struct {
	PoweredRouteCandidate *candidate;
	uint64_t random;
	unsigned long long objective_evaluations;
	int processed;
} Repair_Task;

typedef struct {
	Repair_Task *tasks;
	const Itin_Calc_Data *calc_data;
	int count;
	fenv_t calculation_fp_environment;
	atomic_int next_task;
	atomic_int floating_environment_failed;
} Repair_Work;

static void repair_powered_candidate(Repair_Work *work, int task_index) {
	Repair_Task *task = &work->tasks[task_index];
	if(task->candidate == NULL) return;
	task->processed = 1;
	objective_calls = 0;
	objective_budget = POWERED_REPAIR_OBJECTIVES_PER_ROUTE - 1;
	repair_candidate_timing(task->candidate, work->calc_data, &task->random);
	objective_budget = -1;
	task->objective_evaluations = objective_calls;
	increment_calculation_progress();
}

static void *repair_powered_candidates(void *args) {
	Repair_Work *work = args;
	if(fesetenv(&work->calculation_fp_environment) != 0) {
		atomic_store(&work->floating_environment_failed, 1);
		return NULL;
	}
	set_moon_aware_flybys_enabled(work->calc_data->moon_aware_flybys);
	set_moon_assist_enabled(work->calc_data->moon_assist_arrival);
	int task_index = atomic_fetch_add(&work->next_task, 1);
	while(task_index < work->count && !itinerary_search_cancelled(NULL)) {
		repair_powered_candidate(work, task_index);
		task_index = atomic_fetch_add(&work->next_task, 1);
	}
	return NULL;
}

typedef struct {
	PoweredRouteCandidate *candidate;
	unsigned long long objective_evaluations;
	int processed;
	int considered;
} Scoring_Result;

typedef struct {
	struct ItinStep **arrivals;
	struct ItinStep **departures;
	const Itin_Calc_Data *calc_data;
	Scoring_Result *results;
	int num_departures;
	int count;
	fenv_t calculation_fp_environment;
	atomic_int next_arrival;
	atomic_int floating_environment_failed;
} Scoring_Work;

static void score_arrival_task(Scoring_Work *work, int arrival_index) {
	Scoring_Result *result = &work->results[arrival_index];
	result->processed = 1;
	if(get_num_of_itin_layers(get_first(work->arrivals[arrival_index])) < 3) {
		increment_calculation_progress();
		return;
	}
	result->considered = 1;

	objective_calls = 0;
	objective_budget = -1;
	PoweredRouteCandidate *candidate = calloc(1, sizeof(*candidate));
	if(candidate != NULL && initialize_candidate(candidate, work->arrivals[arrival_index],
		work->departures, work->num_departures, work->calc_data->seq_info.to_target.system)) {
		candidate->best_total_dv = evaluate_route(candidate, work->calc_data, candidate->times,
			candidate->best_states, candidate->best_legs);
		if(isfinite(candidate->best_total_dv) && candidate->best_total_dv < DBL_MAX) {
			candidate->original_total_dv = candidate->best_total_dv;
			result->candidate = candidate;
		} else {
			candidate->best_total_dv = DBL_MAX;
			result->candidate = candidate;
		}
	} else {
		destroy_candidate(candidate);
	}
	result->objective_evaluations = objective_calls;
	objective_budget = -1;
	increment_calculation_progress();
}

static void *score_powered_routes(void *args) {
	Scoring_Work *work = args;
	if(fesetenv(&work->calculation_fp_environment) != 0) {
		atomic_store(&work->floating_environment_failed, 1);
		return NULL;
	}
	set_moon_aware_flybys_enabled(work->calc_data->moon_aware_flybys);
	set_moon_assist_enabled(work->calc_data->moon_assist_arrival);
	int arrival_index = atomic_fetch_add(&work->next_arrival, 1);
	while(arrival_index < work->count && !itinerary_search_cancelled(NULL)) {
		score_arrival_task(work, arrival_index);
		arrival_index = atomic_fetch_add(&work->next_arrival, 1);
	}
	return NULL;
}

typedef struct {
	int processed;
	int optimized;
	int budget_exhausted;
	int dsm_optimized;
	int dsm_improved;
	double dsm_saving;
	unsigned long long objective_evaluations;
} Refinement_Result;

typedef struct {
	PoweredRouteCandidate **candidates;
	const Itin_Calc_Data *calc_data;
	const int *timing_start_counts;
	const int *dsm_slots;
	Refinement_Result *results;
	int count;
	fenv_t calculation_fp_environment;
	atomic_int next_candidate;
	atomic_int floating_environment_failed;
} Refinement_Work;

static void refine_candidate_task(Refinement_Work *work, int candidate_index) {
	PoweredRouteCandidate *candidate = work->candidates[candidate_index];
	Refinement_Result *result = &work->results[candidate_index];
	result->processed = 1;
	objective_calls = 0;
	objective_budget = POWERED_OBJECTIVES_PER_ROUTE;
	result->optimized = optimize_candidate(candidate, work->calc_data,
		work->timing_start_counts[candidate_index]);
	result->budget_exhausted = objective_budget == 0;
	objective_budget = -1;
	if(result->optimized && work->calc_data->allow_dsm && work->dsm_slots[candidate_index]) {
		double cost_without_dsm = candidate->best_total_dv;
		objective_budget = POWERED_DSM_OBJECTIVES_PER_ROUTE;
		optimize_candidate_dsm(candidate, work->calc_data);
		objective_budget = -1;
		result->dsm_optimized = 1;
		result->dsm_improved = candidate->dsms != NULL;
		if(result->dsm_improved)
			result->dsm_saving = fmax(0, cost_without_dsm - candidate->best_total_dv);
	}
	result->objective_evaluations = objective_calls;
	increment_calculation_progress();
}

static void *refine_powered_candidates(void *args) {
	Refinement_Work *work = args;
	if(fesetenv(&work->calculation_fp_environment) != 0) {
		atomic_store(&work->floating_environment_failed, 1);
		return NULL;
	}
	set_moon_aware_flybys_enabled(work->calc_data->moon_aware_flybys);
	set_moon_assist_enabled(work->calc_data->moon_assist_arrival);
	int candidate_index = atomic_fetch_add(&work->next_candidate, 1);
	while(candidate_index < work->count && !itinerary_search_cancelled(NULL)) {
		refine_candidate_task(work, candidate_index);
		candidate_index = atomic_fetch_add(&work->next_candidate, 1);
	}
	return NULL;
}

int optimize_powered_flyby_routes_with_worker_limit(struct ItinStep **departures, int num_departures,
		const Itin_Calc_Data *calc_data, int max_routes, int max_workers,
		PoweredFlybyOptimizationStats *stats) {
	SampleReservoir challenge = {.random = UINT64_C(0x2545f4914f6cdd1d)};
	RepairPool *repair_pool = calloc(1, sizeof(*repair_pool));
	if(repair_pool != NULL) repair_pool->random = UINT64_C(0x9e3779b97f4a7c15);
	double candidate_ranking_seconds = 0;
	double challenge_selection_seconds = 0;
	double *ranking_timer = stats != NULL ? &candidate_ranking_seconds : NULL;
	ExcludedPools excluded = {
		.challenge = &challenge,
		.challenge_selection_seconds = stats != NULL ? &challenge_selection_seconds : NULL
	};
	if(stats != NULL) *stats = (PoweredFlybyOptimizationStats) {
		.best_unpowered_seed_total_dv = DBL_MAX,
		.best_initial_total_dv = DBL_MAX,
		.best_refined_total_dv = DBL_MAX
	};
	if(departures == NULL || num_departures <= 0 || calc_data == NULL || max_routes <= 0 ||
		calc_data->seq_info.to_target.type != ITIN_SEQ_INFO_TO_TARGET) return 0;
	if(itinerary_search_cancelled(NULL)) return 0;
	int num_itineraries = 0;
	set_calculation_progress_phase(TRANSFER_PROGRESS_PREPARING,
		calc_data->dv_filter.last_transfer_type, 0, num_departures * 2);
	for(int i = 0; i < num_departures && !itinerary_search_cancelled(NULL); i++) {
		num_itineraries += get_number_of_itineraries(departures[i]);
		set_calculation_progress_phase(TRANSFER_PROGRESS_PREPARING,
			calc_data->dv_filter.last_transfer_type, i + 1, num_departures * 2);
	}
	if(itinerary_search_cancelled(NULL)) return 0;
	if(num_itineraries <= 0) return 0;
	struct ItinStep **arrivals = malloc((size_t)num_itineraries * sizeof(*arrivals));
	if(arrivals == NULL) return 0;
	int arrival_count = 0;
	for(int i = 0; i < num_departures; i++) {
		store_itineraries_in_array(departures[i], arrivals, &arrival_count);
		set_calculation_progress_phase(TRANSFER_PROGRESS_PREPARING,
			calc_data->dv_filter.last_transfer_type, num_departures + i + 1, num_departures * 2);
	}

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
	Scoring_Result *scoring_results = calloc((size_t)arrival_count, sizeof(*scoring_results));
	if(scoring_results == NULL) {
		fprintf(stderr, "Unable to allocate route-scoring results.\n");
		free(candidates);
		free(repair_pool);
		free(arrivals);
		return 0;
	}
	set_calculation_progress_phase(TRANSFER_PROGRESS_SCORING,
		calc_data->dv_filter.last_transfer_type, 0, arrival_count);
	double route_scoring_start = wall_seconds();
	Scoring_Work scoring_work = {
		.arrivals = arrivals,
		.departures = departures,
		.calc_data = calc_data,
		.results = scoring_results,
		.num_departures = num_departures,
		.count = arrival_count
	};
	atomic_init(&scoring_work.next_arrival, 0);
	atomic_init(&scoring_work.floating_environment_failed, 0);
	if(arrival_count > 1 && max_workers > 0 &&
		fegetenv(&scoring_work.calculation_fp_environment) == 0) {
		struct Thread_Pool pool = use_thread_pool32_preserving_counters_with_limit(
			score_powered_routes, &scoring_work, (size_t)max_workers);
		join_thread_pool(pool);
		if(atomic_load(&scoring_work.floating_environment_failed) &&
			!itinerary_search_cancelled(NULL)) {
			fprintf(stderr, "A scoring worker could not inherit the floating-point environment; finishing remaining routes on the calculation thread.\n");
			for(int arrival_index = 0; arrival_index < arrival_count &&
				!itinerary_search_cancelled(NULL); arrival_index++)
				if(!scoring_results[arrival_index].processed)
					score_arrival_task(&scoring_work, arrival_index);
		}
	} else {
		if(arrival_count > 1 && max_workers > 0)
			fprintf(stderr, "Unable to capture the floating-point environment; scoring routes sequentially.\n");
		for(int arrival_index = 0; arrival_index < arrival_count &&
			!itinerary_search_cancelled(NULL); arrival_index++)
			score_arrival_task(&scoring_work, arrival_index);
	}
	double route_scoring_seconds = wall_seconds() - route_scoring_start;
	unsigned long long scoring_objective_evaluations = 0;
	for(int point_index = 0; point_index < arrival_count; point_index++) {
		Scoring_Result *result = &scoring_results[point_index];
		if(!result->processed) continue;
		routes_considered += result->considered;
		scoring_objective_evaluations += result->objective_evaluations;
		PoweredRouteCandidate *candidate = result->candidate;
		if(candidate == NULL) continue;
		if(candidate->best_total_dv >= DBL_MAX) {
			repair_pool_retain(repair_pool, candidate, num_departures);
			continue;
		}
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
		if(!insert_top_candidate_timed(candidate, candidate->best_total_dv, route_limit,
			&top_count, candidates, top_costs, &excluded, ranking_timer))
			retain_excluded_timed(&excluded, candidate, ranking_timer);
	}
	objective_calls = scoring_objective_evaluations;
	if(!itinerary_search_cancelled(NULL))
		set_calculation_progress_phase(TRANSFER_PROGRESS_SCORING,
			calc_data->dv_filter.last_transfer_type, arrival_count, arrival_count);
	free(scoring_results);
	free(arrivals);

	/* Repaired routes compete by full-mission cost; user limits are enforced by evaluate_route. */
	double repair_start = wall_seconds();
	if(stats != NULL) stats->initial_rejections = repair_pool->total;
	int repair_count = 0;
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS; slot++)
		if(repair_pool->slots[slot] != NULL) repair_count++;
	set_calculation_progress_phase(TRANSFER_PROGRESS_REPAIRING,
		calc_data->dv_filter.last_transfer_type, 0, repair_count);
	Repair_Task repair_tasks[POWERED_REPAIR_SLOTS] = {0};
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS; slot++) {
		repair_tasks[slot].candidate = repair_pool->slots[slot];
		if(repair_tasks[slot].candidate == NULL) continue;
		repair_pool->slots[slot] = NULL;
		/* Per-slot streams make repair output independent of worker scheduling. */
		repair_tasks[slot].random = sample_random(&repair_pool->random);
	}
	Repair_Work repair_work = {
		.tasks = repair_tasks,
		.calc_data = calc_data,
		.count = POWERED_REPAIR_SLOTS
	};
	atomic_init(&repair_work.next_task, 0);
	atomic_init(&repair_work.floating_environment_failed, 0);
	unsigned long long objective_calls_before_repair = objective_calls;
	if(repair_count > 1 && max_workers > 0 &&
		fegetenv(&repair_work.calculation_fp_environment) == 0) {
		struct Thread_Pool pool = use_thread_pool32_preserving_counters_with_limit(
			repair_powered_candidates, &repair_work, (size_t)max_workers);
		join_thread_pool(pool);
		if(atomic_load(&repair_work.floating_environment_failed) &&
			!itinerary_search_cancelled(NULL)) {
			fprintf(stderr, "A repair worker could not inherit the floating-point environment; finishing remaining repairs on the calculation thread.\n");
			for(int slot = 0; slot < POWERED_REPAIR_SLOTS && !itinerary_search_cancelled(NULL); slot++)
				if(repair_tasks[slot].candidate != NULL && !repair_tasks[slot].processed)
					repair_powered_candidate(&repair_work, slot);
		}
	} else {
		if(repair_count > 1 && max_workers > 0)
			fprintf(stderr, "Unable to capture the floating-point environment; repairing routes sequentially.\n");
		for(int slot = 0; slot < POWERED_REPAIR_SLOTS && !itinerary_search_cancelled(NULL); slot++)
			repair_powered_candidate(&repair_work, slot);
	}
	unsigned long long repair_objective_evaluations = 0;
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS; slot++) {
		Repair_Task *task = &repair_tasks[slot];
		PoweredRouteCandidate *candidate = task->candidate;
		if(candidate == NULL) continue;
		if(!task->processed) {
			destroy_candidate(candidate);
			task->candidate = NULL;
			continue;
		}
		if(stats != NULL) stats->repairs_attempted++;
		repair_objective_evaluations += task->objective_evaluations;
	}
	objective_calls = objective_calls_before_repair + repair_objective_evaluations;
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS; slot++) {
		Repair_Task *task = &repair_tasks[slot];
		PoweredRouteCandidate *candidate = task->candidate;
		if(candidate == NULL) continue;
		task->candidate = NULL;
		double verified = DBL_MAX;
		if(task->processed && candidate->best_total_dv < DBL_MAX &&
			!itinerary_search_cancelled(NULL)) {
			objective_budget = 1;
			verified = evaluate_route(candidate, calc_data, candidate->times,
				candidate->best_states, candidate->best_legs);
			objective_budget = -1;
		}
		if(!task->processed || !isfinite(verified) || verified >= DBL_MAX) {
			destroy_candidate(candidate);
			continue;
		}
		candidate->best_total_dv = candidate->original_total_dv = verified;
		if(stats != NULL) stats->repairs_recovered++;
		if(insert_top_candidate_timed(candidate, verified, route_limit, &top_count,
			candidates, top_costs, &excluded, ranking_timer)) {
			if(stats != NULL) stats->repairs_competing++;
			if(stats != NULL && verified < stats->best_initial_total_dv)
				stats->best_initial_total_dv = verified;
		} else retain_excluded_timed(&excluded, candidate, ranking_timer);
	}
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS; slot++)
		destroy_candidate(repair_pool->slots[slot]);
	free(repair_pool);
	double repair_seconds = wall_seconds() - repair_start;

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
		stats->route_scoring_seconds = route_scoring_seconds;
		stats->global_ranking_seconds = fmax(0, candidate_ranking_seconds -
			challenge_selection_seconds);
		stats->challenge_selection_seconds = challenge_selection_seconds;
		stats->repair_seconds = repair_seconds;
	}
	if(itinerary_search_cancelled(NULL)) {
		for(int candidate_index = 0; candidate_index < top_count; candidate_index++)
			destroy_candidate(candidates[candidate_index]);
		free(candidates);
		return 0;
	}

	int improvements = 0;
	int candidate_index = 0;
	set_calculation_progress_phase(TRANSFER_PROGRESS_REFINING,
		calc_data->dv_filter.last_transfer_type, 0, top_count);
	double timing_refinement_start = wall_seconds();
	int multi_start_routes = 0;
	int resonant_dsm_routes = 0;
	double ranked_worst = 0;
	int planned_multi_start_routes = 0;
	int timing_start_counts[POWERED_ROUTE_REFINEMENT_LIMIT + POWERED_CHALLENGE_SLOTS] = {0};
	int dsm_slots[POWERED_ROUTE_REFINEMENT_LIMIT + POWERED_CHALLENGE_SLOTS] = {0};
	Refinement_Result refinement_results[POWERED_ROUTE_REFINEMENT_LIMIT + POWERED_CHALLENGE_SLOTS] = {0};
	for(candidate_index = 0; candidate_index < top_count; candidate_index++) {
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
		timing_start_counts[candidate_index] = extra_starts ? POWERED_TIMING_START_COUNT : 1;
		planned_multi_start_routes += extra_starts;
		multi_start_routes = planned_multi_start_routes;
		// Resonant routes need a maneuver to be useful but rarely rank in the overall top, so they get extra slots.
		dsm_slots[candidate_index] = candidate_index < POWERED_DSM_ROUTE_LIMIT;
		if(!dsm_slots[candidate_index] && candidate->leg_variants != NULL &&
			resonant_dsm_routes < POWERED_DSM_RESONANT_ROUTE_LIMIT) {
			dsm_slots[candidate_index] = 1;
			resonant_dsm_routes++;
		}
	}
	Refinement_Work refinement_work = {
		.candidates = candidates,
		.calc_data = calc_data,
		.timing_start_counts = timing_start_counts,
		.dsm_slots = dsm_slots,
		.results = refinement_results,
		.count = top_count
	};
	atomic_init(&refinement_work.floating_environment_failed, 0);
	atomic_init(&refinement_work.next_candidate, 0);
	unsigned long long objective_calls_before_refinement = objective_calls;
	if(top_count > 1 && max_workers > 0) {
		// New Windows worker threads may not inherit the calculation thread's floating-point mode.
		if(fegetenv(&refinement_work.calculation_fp_environment) == 0) {
			struct Thread_Pool pool = use_thread_pool32_preserving_counters_with_limit(
				refine_powered_candidates, &refinement_work, (size_t)max_workers);
			join_thread_pool(pool);
			if(atomic_load(&refinement_work.floating_environment_failed) &&
				!itinerary_search_cancelled(NULL)) {
				fprintf(stderr, "A refinement worker could not inherit the floating-point environment; finishing remaining candidates on the calculation thread.\n");
				for(candidate_index = 0; candidate_index < top_count; candidate_index++)
					if(!refinement_results[candidate_index].processed &&
						!itinerary_search_cancelled(NULL))
						refine_candidate_task(&refinement_work, candidate_index);
			}
		} else {
			fprintf(stderr, "Unable to capture the floating-point environment; refining candidates sequentially.\n");
			for(candidate_index = 0; candidate_index < top_count &&
				!itinerary_search_cancelled(NULL); candidate_index++)
				refine_candidate_task(&refinement_work, candidate_index);
		}
	} else {
		for(candidate_index = 0; candidate_index < top_count &&
			!itinerary_search_cancelled(NULL); candidate_index++)
			refine_candidate_task(&refinement_work, candidate_index);
	}
	multi_start_routes = 0;
	unsigned long long refinement_objective_evaluations = 0;
	for(candidate_index = 0; candidate_index < top_count; candidate_index++) {
		PoweredRouteCandidate *candidate = candidates[candidate_index];
		Refinement_Result *result = &refinement_results[candidate_index];
		if(!result->processed) continue;
		multi_start_routes += timing_start_counts[candidate_index] > 1;
		refinement_objective_evaluations += result->objective_evaluations;
		if(stats != NULL) {
			stats->refinement_budgets_exhausted += result->budget_exhausted;
			stats->dsm_routes_optimized += result->dsm_optimized;
			stats->dsm_routes_improved += result->dsm_improved;
			stats->best_dsm_saving = fmax(stats->best_dsm_saving, result->dsm_saving);
		}
		if(result->optimized &&
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
	objective_calls = objective_calls_before_refinement + refinement_objective_evaluations;
	set_calculation_progress_phase(TRANSFER_PROGRESS_FINALIZING,
		calc_data->dv_filter.last_transfer_type, 0, 0);
	for(int i = 0; i < top_count; i++)
		destroy_candidate(candidates[i]);
	free(candidates);
	if(stats != NULL) {
		stats->routes_improved = improvements;
		stats->timing_refinement_seconds = wall_seconds() - timing_refinement_start;
		stats->objective_evaluations = objective_calls;
		stats->multi_start_routes = multi_start_routes;
	}
	return improvements;
}

int optimize_powered_flyby_routes(struct ItinStep **departures, int num_departures,
		const Itin_Calc_Data *calc_data, int max_routes, PoweredFlybyOptimizationStats *stats) {
	return optimize_powered_flyby_routes_with_worker_limit(departures, num_departures,
		calc_data, max_routes, (int)get_thread_pool32_worker_capacity(), stats);
}
