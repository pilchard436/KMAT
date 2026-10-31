#include "transfer_search_internal.h"
#include "search_cancel.h"
#include "search_rejections.h"
#include "tools/tool_funcs.h"
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/time.h>
#include <limits.h>
#include <fenv.h>

static atomic_int routes_blocked_by_moons;

typedef struct {
	struct ItinStep **departures;
	int num_departures;
	const Itin_Calc_Data *calc_data;
	// Lowers the candidate count the progress window shows as routes are removed.
	int count_removed_routes;
	atomic_int next_departure;
} Itinerary_Filter_Work;

typedef struct {
	struct ItinStep **departures;
	int num_departures;
	const Itin_Calc_Data *calc_data;
	int max_workers;
	int improvements;
	fenv_t calculation_fp_environment;
	int floating_environment_failed;
	PoweredFlybyOptimizationStats stats;
} Powered_Mode_Optimization_Work;

typedef struct {
	Powered_Mode_Optimization_Work modes[2];
	atomic_int next_mode;
} Combined_Mode_Optimization_Work;

static void *optimize_powered_mode(void *args) {
	Combined_Mode_Optimization_Work *combined_work = args;
	int mode_index = atomic_fetch_add(&combined_work->next_mode, 1);
	if(mode_index >= 2) return NULL;
	Powered_Mode_Optimization_Work *work = &combined_work->modes[mode_index];
	if(fesetenv(&work->calculation_fp_environment) != 0) {
		work->floating_environment_failed = 1;
		return NULL;
	}
	int previous_moon_aware = set_moon_aware_flybys_enabled(work->calc_data->moon_aware_flybys);
	int previous_moon_assist = set_moon_assist_enabled(work->calc_data->moon_assist_arrival);
	work->improvements = optimize_powered_flyby_routes_with_worker_limit(work->departures,
		work->num_departures, work->calc_data, POWERED_ROUTE_REFINEMENT_LIMIT,
		work->max_workers, &work->stats);
	set_moon_assist_enabled(previous_moon_assist);
	set_moon_aware_flybys_enabled(previous_moon_aware);
	complete_combined_optimization_progress();
	return NULL;
}

int get_num_departure_dates(double jd_min_dep, double jd_max_dep, double step_dep_date) {
	if(!isfinite(jd_min_dep) || !isfinite(jd_max_dep) || !isfinite(step_dep_date) ||
		jd_max_dep < jd_min_dep || step_dep_date <= 0) return 0;
	double intervals = floor((jd_max_dep - jd_min_dep) / step_dep_date + 1e-9);
	if(intervals >= INT_MAX) return 0;
	return (int)intervals + 1;
}

// Whether the route visits exactly the bodies of a fixed sequence, in order; maneuver nodes have no body and are skipped.
static int route_follows_sequence(struct ItinStep *arrival, const struct ItinSequenceInfoSpecItin *sequence) {
	int step = sequence->num_steps - 1;
	for(struct ItinStep *node = arrival; node != NULL; node = node->prev) {
		if(node->body == NULL) continue;
		if(step < 0 || node->body != sequence->bodies[step]) return 0;
		step--;
	}
	return step == -1;
}

static int route_passes_filters(struct ItinStep *arrival, const Itin_Calc_Data *calc_data) {
	increment_calculation_progress();
	if(calc_data->seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET &&
		arrival->body != calc_data->seq_info.to_target.arr_body) return 0;
	if(calc_data->seq_info.spec_seq.type == ITIN_SEQ_INFO_SPEC_SEQ &&
		!route_follows_sequence(arrival, &calc_data->seq_info.spec_seq)) return 0;
	struct PorkchopPoint point = create_porkchop_point(arrival,
		calc_data->dv_filter.dep_periapsis, calc_data->dv_filter.arr_periapsis);
	struct ItinStep *departure = get_first(arrival);
	if(!isfinite(point.dv_dep) || !isfinite(point.dv_dsm) || !isfinite(point.dv_flyby)) return 0;
	if(arrival->date > calc_data->jd_max_arr || departure->date < calc_data->jd_min_dep ||
		departure->date > calc_data->jd_max_dep) {
		count_complete_route_rejection(REJECT_TIME_LIMIT);
		return 0;
	}
	double satellite_dv = point.dv_dsm;
	if(calc_data->dv_filter.last_transfer_type == TF_CAPTURE) satellite_dv += point.dv_arr_cap;
	if(calc_data->dv_filter.last_transfer_type == TF_CIRC) satellite_dv += point.dv_arr_circ;
	if(point.dv_dep > calc_data->dv_filter.max_depdv) {
		count_complete_route_rejection(REJECT_DEPARTURE_DV);
		return 0;
	}
	if(satellite_dv + point.dv_flyby > calc_data->dv_filter.max_satdv) {
		count_complete_route_rejection(REJECT_SATELLITE_DV);
		return 0;
	}
	if(point.dv_dep + satellite_dv + point.dv_flyby > calc_data->dv_filter.max_totdv) {
		count_complete_route_rejection(REJECT_TOTAL_DV);
		return 0;
	}
	if(route_blocked_by_moons(arrival)) {
		atomic_fetch_add(&routes_blocked_by_moons, 1);
		return 0;
	}
	int crosses_unmodeled_soi = 0;
	if(powered_route_has_unmodeled_soi_crossing(calc_data, arrival, &crosses_unmodeled_soi) &&
		crosses_unmodeled_soi) return 0;
	return 1;
}

static void detach_itinerary_child(struct ItinStep *parent, int child_index) {
	free_itinerary(parent->next[child_index]);
	parent->num_next_nodes--;
	for(int i = child_index; i < parent->num_next_nodes; i++)
		parent->next[i] = parent->next[i + 1];
}

static int prune_invalid_route_leaves(struct ItinStep *step, const Itin_Calc_Data *calc_data, int count_removed_routes) {
	for(int child_index = 0; child_index < step->num_next_nodes;) {
		struct ItinStep *child = step->next[child_index];
		int valid;
		if(child->num_next_nodes == 0) {
			valid = route_passes_filters(child, calc_data);
			if(!valid && count_removed_routes) incr_thread_counter_by_amount(2, -1);
		} else valid = prune_invalid_route_leaves(child, calc_data, count_removed_routes);
		if(valid) child_index++;
		else detach_itinerary_child(step, child_index);
	}
	return step->num_next_nodes > 0;
}

static void *filter_itinerary_departures(void *args) {
	Itinerary_Filter_Work *work = args;
	set_moon_aware_flybys_enabled(work->calc_data->moon_aware_flybys);
	set_moon_assist_enabled(work->calc_data->moon_assist_arrival);
	int departure_index = atomic_fetch_add(&work->next_departure, 1);
	while(departure_index < work->num_departures) {
		struct ItinStep *departure = work->departures[departure_index];
		if(!prune_invalid_route_leaves(departure, work->calc_data, work->count_removed_routes)) {
			free_itinerary(departure);
			work->departures[departure_index] = NULL;
		}
		increment_calculation_progress();
		departure_index = atomic_fetch_add(&work->next_departure, 1);
	}
	return NULL;
}

static int filter_itinerary_departure_array(struct ItinStep **departures, int num_departures,
		const Itin_Calc_Data *calc_data, int count_removed_routes) {
	Itinerary_Filter_Work work = {
		.departures = departures,
		.num_departures = num_departures,
		.calc_data = calc_data,
		.count_removed_routes = count_removed_routes
	};
	atomic_init(&work.next_departure, 0);
	if(num_departures > 1) {
		struct Thread_Pool pool = use_thread_pool32_preserving_counters(
			filter_itinerary_departures, &work);
		join_thread_pool(pool);
	} else if(num_departures == 1) {
		filter_itinerary_departures(&work);
	}
	int valid_departures = 0;
	for(int i = 0; i < num_departures; i++) {
		if(departures[i] != NULL) departures[valid_departures++] = departures[i];
	}
	return valid_departures;
}

static void cache_arrival_dvs_in_tree(struct ItinStep *step, double dep_periapsis, double arr_periapsis) {
	if(step == NULL) return;
	if(step->num_next_nodes == 0) {
		if(step->prev != NULL && step->body != NULL)
			create_porkchop_point(step, dep_periapsis, arr_periapsis);
		return;
	}
	for(int i = 0; i < step->num_next_nodes; i++)
		cache_arrival_dvs_in_tree(step->next[i], dep_periapsis, arr_periapsis);
}

static struct ItinStep *copy_itinerary_tree_checked(const struct ItinStep *source) {
	if(source == NULL) return NULL;
	struct ItinStep *copy = calloc(1, sizeof(*copy));
	if(copy == NULL) return NULL;
	copy_step_body_vectors_and_date((struct ItinStep *)source, copy);
	copy->num_next_nodes = source->num_next_nodes;
	if(copy->num_next_nodes > 0) {
		copy->next = calloc((size_t)copy->num_next_nodes, sizeof(*copy->next));
		if(copy->next == NULL) {
			free(copy);
			return NULL;
		}
		for(int i = 0; i < copy->num_next_nodes; i++) {
			copy->next[i] = copy_itinerary_tree_checked(source->next[i]);
			if(copy->next[i] == NULL) {
				copy->num_next_nodes = i;
				free_itinerary(copy);
				return NULL;
			}
			copy->next[i]->prev = copy;
		}
	}
	return copy;
}

static struct Itin_Calc_Results search_for_itineraries_internal(Itin_Calc_Data calc_data, int calculate_both_modes) {
	if(calculate_both_modes) calc_data.dv_filter.last_transfer_type = TF_FLYBY;
	int previous_moon_aware = set_moon_aware_flybys_enabled(calc_data.moon_aware_flybys);
	int previous_moon_assist = set_moon_assist_enabled(calc_data.moon_assist_arrival);
	struct Itin_Calc_Results results = {
		.best_unpowered_seed_total_dv = HUGE_VAL,
		.best_powered_initial_total_dv = HUGE_VAL,
		.best_powered_refined_total_dv = HUGE_VAL
	};

	reset_search_rejections();
	struct timeval start, end;
	double elapsed_time;
	gettimeofday(&start, NULL);  // Record the ending time
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	double phase_start = progress_clock();
	double initialization_seconds = 0;
	double search_seconds = 0;
	double optimization_seconds = 0;
	double filtering_seconds = 0;
#endif

	if(!isfinite(calc_data.step_dep_date) || calc_data.step_dep_date <= 0)
		calc_data.step_dep_date = 1;
	int num_deps = get_num_departure_dates(
		calc_data.jd_min_dep, calc_data.jd_max_dep, calc_data.step_dep_date);
	if(num_deps == 0) {
		set_moon_assist_enabled(previous_moon_assist);
		set_moon_aware_flybys_enabled(previous_moon_aware);
		return results;
	}

	struct ItinStep **departures = (struct ItinStep**) malloc(num_deps * sizeof(struct ItinStep*));
	for(int i = 0; i < num_deps; i++) departures[i] = calloc(1, sizeof(struct ItinStep));


#ifdef KMAT_ENABLE_SEARCH_PROFILE
	Body **profile_bodies;
	int num_profile_bodies;
	if(calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
		profile_bodies = calc_data.seq_info.to_target.flyby_bodies;
		num_profile_bodies = calc_data.seq_info.to_target.num_flyby_bodies;
	} else {
		profile_bodies = calc_data.seq_info.spec_seq.bodies;
		num_profile_bodies = calc_data.seq_info.spec_seq.num_steps;
	}
	ItinFlybyProfileStats *flyby_profiles = calloc((size_t)num_deps * num_profile_bodies, sizeof(*flyby_profiles));
	Itin_Search_Profile *profiles = calloc(num_deps, sizeof(*profiles));
	for(int i = 0; i < num_deps; i++)
		profiles[i].flyby_stats = flyby_profiles + (size_t)i * num_profile_bodies;
#endif
	Itin_Calc_Thread_Args thread_args = {
		.departures = departures,
		.seed_stats = calloc(num_deps, sizeof(PoweredSeedSearchStats)),
		.calc_data = &calc_data
	};
	if(thread_args.seed_stats == NULL) {
		for(int i = 0; i < num_deps; i++) free_itinerary(departures[i]);
		free(departures);
#ifdef KMAT_ENABLE_SEARCH_PROFILE
		free(profiles);
		free(flyby_profiles);
#endif
		set_moon_assist_enabled(previous_moon_assist);
		set_moon_aware_flybys_enabled(previous_moon_aware);
		return results;
	}
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	thread_args.profiles = profiles;
	thread_args.profile_bodies = profile_bodies;
	thread_args.num_profile_bodies = num_profile_bodies;
#endif

	thread_progress_and_prop_info = (struct Thread_Progress_and_Prop_Info) {
		.num_departures = num_deps,
		.phase = TRANSFER_PROGRESS_SEARCH,
		.phase_transfer_type = calc_data.dv_filter.last_transfer_type
	};
	printf("Search window: departure JD %.2f..%.2f, max arrival JD %.2f, max duration %.2f days\n",
		calc_data.jd_min_dep, calc_data.jd_max_dep, calc_data.jd_max_arr, calc_data.max_duration);
	if(calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
		struct ItinSequenceInfoToTarget *target = &calc_data.seq_info.to_target;
		printf("Search bodies: %s -> %s; flyby candidates (%d):",
			target->dep_body->name, target->arr_body->name, target->num_flyby_bodies);
		for(int i = 0; i < target->num_flyby_bodies; i++)
			printf(" %s", target->flyby_bodies[i]->name);
		printf("\n");
	}

	show_progress("Transfer Calculation progress", 0, 1);
	struct Thread_Pool thread_pool;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	initialization_seconds = progress_clock() - phase_start;
	phase_start = progress_clock();
#endif
	thread_pool = use_thread_pool32(calc_itins_from_departure, &thread_args);
	join_thread_pool(thread_pool);
	results.num_candidate_routes = get_thread_counter(2);
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	search_seconds = progress_clock() - phase_start;
#endif
	PoweredSeedSearchStats seed_stats = {0};
	for(int i = 0; i < num_deps; i++) {
		seed_stats.evaluations += thread_args.seed_stats[i].evaluations;
		seed_stats.alternatives_added += thread_args.seed_stats[i].alternatives_added;
		seed_stats.alternatives_skipped += thread_args.seed_stats[i].alternatives_skipped;
		seed_stats.seeds_added += thread_args.seed_stats[i].seeds_added;
		seed_stats.searches_budget_skipped += thread_args.seed_stats[i].searches_budget_skipped;
	}
	free(thread_args.seed_stats);
	results.powered_seed_stats = seed_stats;
	printf("Powered seeds: %llu feasibility evaluations, %llu extra branches, %llu feasible alternatives skipped by budget\n",
		seed_stats.evaluations, seed_stats.alternatives_added, seed_stats.alternatives_skipped);
	printf("Powered expansion budget: %llu seeds added, %llu encounters skipped powered sampling after budget exhaustion\n",
		seed_stats.seeds_added, seed_stats.searches_budget_skipped);
	struct Transfer_Calc_Status final_status = get_current_transfer_calc_status();
	show_progress(get_thread_counter(3) > 0 ? "Transfer Calculation canceled" :
		"Transfer Calculation progress", final_status.progress, 1);
	printf("\n");
	finish_progress();
	#ifdef KMAT_ENABLE_SEARCH_PROFILE
	double initial_grid_seconds = 0;
	double recursive_search_seconds = 0;
	double recursive_end_node_seconds = 0;
	double recursive_dv_pruning_seconds = 0;
	double flyby_search_seconds = 0;
	unsigned long long initial_lambert_calls = 0;
	unsigned long long recursive_lambert_calls = 0;
	for(int i = 0; i < num_deps; i++) {
		initial_grid_seconds += profiles[i].initial_grid_seconds;
		recursive_search_seconds += profiles[i].recursive_search_seconds;
		recursive_end_node_seconds += profiles[i].recursive_end_node_seconds;
		recursive_dv_pruning_seconds += profiles[i].recursive_dv_pruning_seconds;
		initial_lambert_calls += profiles[i].initial_lambert_calls;
		recursive_lambert_calls += profiles[i].recursive_lambert_calls;
	}
	printf("Initial grid: %.3f summed worker-seconds, %llu Lambert solves\n",
		initial_grid_seconds, initial_lambert_calls);
	printf("Recursive search: %.3f summed worker-seconds, %llu Lambert solves\n",
		recursive_search_seconds, recursive_lambert_calls);
	for(int body_index = 0; body_index < num_profile_bodies; body_index++)
		for(int departure_index = 0; departure_index < num_deps; departure_index++)
			flyby_search_seconds += profiles[departure_index].flyby_stats[body_index].search_seconds;
	double recursive_route_expansion_seconds = recursive_search_seconds -
		recursive_end_node_seconds - recursive_dv_pruning_seconds - flyby_search_seconds;
	printf("Recursive breakdown (summed worker-seconds): end-node collection %.3f, delta-v pruning %.3f, flyby search %.3f, route expansion/other %.3f\n",
		recursive_end_node_seconds, recursive_dv_pruning_seconds, flyby_search_seconds,
		recursive_route_expansion_seconds);
	for(int body_index = 0; body_index < num_profile_bodies; body_index++) {
		unsigned long long searches = 0;
		unsigned long long left_evaluations = 0;
		unsigned long long right_evaluations = 0;
		double search_seconds = 0;
		for(int departure_index = 0; departure_index < num_deps; departure_index++) {
			ItinFlybyProfileStats stats = profiles[departure_index].flyby_stats[body_index];
			searches += stats.searches;
			left_evaluations += stats.left_evaluations;
			right_evaluations += stats.right_evaluations;
			search_seconds += stats.search_seconds;
		}
		if(searches > 0)
			printf("Flyby %s: %.3f summed worker-seconds, %llu searches, %llu left / %llu right root evaluations\n",
				profile_bodies[body_index]->name, search_seconds, searches,
				left_evaluations, right_evaluations);
	}
	free(profiles);
	free(flyby_profiles);
	#endif

	// Remove empty departure trees and compact the results in one pass.
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	phase_start = progress_clock();
#endif
	set_calculation_progress_phase(TRANSFER_PROGRESS_PREPARING, TF_FLYBY, 0, num_deps);
	int valid_departures = 0;
	for(int i = 0; i < num_deps; i++) {
		if(departures[i] == NULL || departures[i]->num_next_nodes == 0) {
			free_itinerary(departures[i]);
		} else {
			departures[valid_departures++] = departures[i];
		}
		set_calculation_progress_phase(TRANSFER_PROGRESS_PREPARING, TF_FLYBY, i + 1, num_deps);
	}
	num_deps = valid_departures;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	filtering_seconds += progress_clock() - phase_start;
#endif
	int shared_num_deps = num_deps;
	struct ItinStep **capture_departures = NULL;
	if(calculate_both_modes && shared_num_deps > 0) {
		set_calculation_progress_phase(
			TRANSFER_PROGRESS_PREPARING_CAPTURE, TF_CAPTURE, 0, shared_num_deps);
		capture_departures = calloc((size_t)shared_num_deps, sizeof(*capture_departures));
		if(capture_departures == NULL) {
			results.capture_calculation_failed = 1;
		} else {
			for(int i = 0; i < shared_num_deps; i++) {
				capture_departures[i] = copy_itinerary_tree_checked(departures[i]);
				if(capture_departures[i] == NULL) {
					for(int j = 0; j < i; j++) free_itinerary(capture_departures[j]);
					free(capture_departures);
					capture_departures = NULL;
					results.capture_calculation_failed = 1;
					break;
				}
				set_calculation_progress_phase(
					TRANSFER_PROGRESS_PREPARING_CAPTURE, TF_CAPTURE, i + 1, shared_num_deps);
			}
		}
	}
	Itin_Calc_Data capture_calc_data = calc_data;
	capture_calc_data.dv_filter.last_transfer_type = TF_CAPTURE;
	PoweredFlybyOptimizationStats capture_stats = {0};
	int capture_optimization_completed = 0;
	// Unpowered routes keep the dates and velocities of the search; the optimizer (and the deep-space maneuver pass
	// inside it) only applies when flybys may be powered.
	int run_powered_optimization = !calc_data.disable_powered_flybys;
	if(!run_powered_optimization)
		printf("Powered flybys off: unpowered gravity assists only, route optimization skipped\n");
	if(get_thread_counter(3) == 0 && run_powered_optimization) {
		PoweredFlybyOptimizationStats powered_stats = {0};
#ifdef KMAT_ENABLE_SEARCH_PROFILE
		phase_start = progress_clock();
#endif
		if(calculate_both_modes && capture_departures != NULL) {
			size_t worker_capacity = get_thread_pool32_worker_capacity();
			fenv_t calculation_fp_environment;
			if(worker_capacity >= 2 && fegetenv(&calculation_fp_environment) == 0) {
				int nested_worker_capacity = (int)worker_capacity - 2;
				Combined_Mode_Optimization_Work combined_work = {
					.modes = {
					{
						.departures = departures,
						.num_departures = num_deps,
						.calc_data = &calc_data,
						.max_workers = (nested_worker_capacity + 1) / 2,
						.calculation_fp_environment = calculation_fp_environment
					},
					{
						.departures = capture_departures,
						.num_departures = shared_num_deps,
						.calc_data = &capture_calc_data,
						.max_workers = nested_worker_capacity / 2,
						.calculation_fp_environment = calculation_fp_environment
					}
					}
				};
				atomic_init(&combined_work.next_mode, 0);
				begin_combined_optimization_progress(2);
				struct Thread_Pool pool = use_thread_pool32_preserving_counters_with_limit(
					optimize_powered_mode, &combined_work, 2);
				join_thread_pool(pool);
				for(int mode_index = 0; mode_index < 2; mode_index++) {
					Powered_Mode_Optimization_Work *mode = &combined_work.modes[mode_index];
					if(!mode->floating_environment_failed) continue;
					fprintf(stderr, "An optimization worker could not inherit the calculation thread's floating-point environment; finishing that mode on the calculation thread.\n");
					if(mode_index == 0) {
						mode->improvements = optimize_powered_flyby_routes(
							departures, num_deps, &calc_data,
							POWERED_ROUTE_REFINEMENT_LIMIT, &mode->stats);
						powered_stats = mode->stats;
					} else {
						mode->improvements = optimize_powered_flyby_routes(
							capture_departures, shared_num_deps, &capture_calc_data,
							POWERED_ROUTE_REFINEMENT_LIMIT, &mode->stats);
						capture_stats = mode->stats;
						capture_optimization_completed = 1;
					}
					complete_combined_optimization_progress();
				}
				end_combined_optimization_progress();
				if(!combined_work.modes[0].floating_environment_failed)
					powered_stats = combined_work.modes[0].stats;
				if(!combined_work.modes[1].floating_environment_failed) {
					capture_stats = combined_work.modes[1].stats;
					capture_optimization_completed = 1;
				}
			} else {
				fprintf(stderr, "Unable to capture the calculation thread's floating-point environment or start two mode workers; optimizing modes sequentially.\n");
				optimize_powered_flyby_routes(departures, num_deps, &calc_data,
					POWERED_ROUTE_REFINEMENT_LIMIT, &powered_stats);
			}
		} else {
			optimize_powered_flyby_routes(departures, num_deps, &calc_data,
				POWERED_ROUTE_REFINEMENT_LIMIT, &powered_stats);
		}
#ifdef KMAT_ENABLE_SEARCH_PROFILE
		optimization_seconds = progress_clock() - phase_start;
#endif
		results.powered_routes_valid = powered_stats.routes_valid;
		results.powered_routes_corrected = powered_stats.routes_corrected;
		results.powered_initial_rejections = powered_stats.initial_rejections;
		results.powered_repairs_attempted = powered_stats.repairs_attempted;
		results.powered_repairs_recovered = powered_stats.repairs_recovered;
		results.powered_repairs_competing = powered_stats.repairs_competing;
		results.powered_challenges_refined = powered_stats.challenges_refined;
		results.powered_dsm_routes_optimized = powered_stats.dsm_routes_optimized;
		results.powered_dsm_routes_improved = powered_stats.dsm_routes_improved;
		results.powered_best_dsm_saving = powered_stats.best_dsm_saving;
		results.best_unpowered_seed_total_dv = powered_stats.best_unpowered_seed_total_dv;
		results.best_powered_initial_total_dv = powered_stats.best_initial_total_dv;
		results.best_powered_refined_total_dv = powered_stats.best_refined_total_dv;
		printf("Powered flyby optimization: %d/%d improved, %d valid, %d SOI-corrected, best saving %.2f m/s, best full dV %.3f -> %.3f m/s, route scoring %.2f s, global ranking/merge %.4f s, challenge selection %.6f s, repair %.2f s, timing refinement %.2f s\n",
			powered_stats.routes_improved, powered_stats.routes_considered,
			powered_stats.routes_valid, powered_stats.routes_corrected,
			powered_stats.best_delta_v_saving, powered_stats.best_initial_total_dv,
			powered_stats.best_refined_total_dv, powered_stats.route_scoring_seconds,
			powered_stats.global_ranking_seconds, powered_stats.challenge_selection_seconds,
			powered_stats.repair_seconds,
			powered_stats.timing_refinement_seconds);
		printf("Refinement budget: %llu objective evaluations, %d multi-start routes, %d route budgets exhausted\n",
			powered_stats.objective_evaluations, powered_stats.multi_start_routes,
			powered_stats.refinement_budgets_exhausted);
		printf("Rejected-route repair: %llu initial rejections, %d sampled, %d recovered, %d entered refinement; cutoff challenge: %llu excluded, %d refined, %d beat the ranked worst\n",
			powered_stats.initial_rejections, powered_stats.repairs_attempted,
			powered_stats.repairs_recovered, powered_stats.repairs_competing,
			powered_stats.challenge_population, powered_stats.challenges_refined,
			powered_stats.challenges_beat_cutoff);
		if(calc_data.allow_dsm)
			printf("Deep-space maneuvers: %d routes optimized, %d improved, best saving %.2f m/s\n",
				powered_stats.dsm_routes_optimized, powered_stats.dsm_routes_improved,
				powered_stats.best_dsm_saving);
	}
	int itineraries_before_prune = 0;
	for(int i = 0; i < num_deps; i++)
		itineraries_before_prune += get_number_of_itineraries(departures[i]);
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	phase_start = progress_clock();
#endif
	set_calculation_progress_phase(
		TRANSFER_PROGRESS_FILTERING, TF_FLYBY, 0, itineraries_before_prune + num_deps);
	valid_departures = 0;
	atomic_store(&routes_blocked_by_moons, 0);
	// The shown candidate count starts from the routes left after optimization and falls as the filter removes them.
	incr_thread_counter_by_amount(2, itineraries_before_prune - get_thread_counter(2));
	num_deps = filter_itinerary_departure_array(departures, num_deps, &calc_data, 1);
	int itineraries_after_prune = 0;
	for(int i = 0; i < num_deps; i++)
		itineraries_after_prune += get_number_of_itineraries(departures[i]);
	results.powered_itineraries_pruned = itineraries_before_prune - itineraries_after_prune;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	filtering_seconds += progress_clock() - phase_start;
#endif
	int moon_blocked = atomic_load(&routes_blocked_by_moons);
	if(moon_blocked > 0)
		printf("Moon check removed %d of %d routes (hit a moon or passed inside a moon's sphere of influence)\n",
			moon_blocked, itineraries_before_prune);

	if(calculate_both_modes && capture_departures != NULL) {
		Itin_Calc_Data capture_calc_data = calc_data;
		capture_calc_data.dv_filter.last_transfer_type = TF_CAPTURE;
		if(get_thread_counter(3) == 0 && run_powered_optimization && !capture_optimization_completed) {
			optimize_powered_flyby_routes(capture_departures, shared_num_deps,
				&capture_calc_data, POWERED_ROUTE_REFINEMENT_LIMIT, &capture_stats);
			printf("Capture powered-route optimization: %d/%d improved, %d valid, route scoring %.2f s, global ranking/merge %.4f s, challenge selection %.6f s, repair %.2f s, timing refinement %.2f s\n",
				capture_stats.routes_improved, capture_stats.routes_considered, capture_stats.routes_valid,
				capture_stats.route_scoring_seconds, capture_stats.global_ranking_seconds,
				capture_stats.challenge_selection_seconds, capture_stats.repair_seconds,
				capture_stats.timing_refinement_seconds);
		}
		if(capture_optimization_completed)
			printf("Capture powered-route optimization: %d/%d improved, %d valid, route scoring %.2f s, global ranking/merge %.4f s, challenge selection %.6f s, repair %.2f s, timing refinement %.2f s\n",
				capture_stats.routes_improved, capture_stats.routes_considered, capture_stats.routes_valid,
				capture_stats.route_scoring_seconds, capture_stats.global_ranking_seconds,
				capture_stats.challenge_selection_seconds, capture_stats.repair_seconds,
				capture_stats.timing_refinement_seconds);

		atomic_store(&routes_blocked_by_moons, 0);
		int capture_itineraries_before_prune = 0;
		for(int i = 0; i < shared_num_deps; i++)
			capture_itineraries_before_prune += get_number_of_itineraries(capture_departures[i]);
		set_calculation_progress_phase(
			TRANSFER_PROGRESS_FILTERING, TF_CAPTURE, 0,
			capture_itineraries_before_prune + shared_num_deps);
		shared_num_deps = filter_itinerary_departure_array(
			capture_departures, shared_num_deps, &capture_calc_data, 0);
		if(shared_num_deps == 0) {
			free(capture_departures);
			capture_departures = NULL;
		}
	}

	set_calculation_progress_phase(TRANSFER_PROGRESS_FINALIZING, TF_BOTH, 0, 0);
	for(int i = 0; i < num_deps; i++)
		cache_arrival_dvs_in_tree(departures[i], calc_data.dv_filter.dep_periapsis,
			calc_data.dv_filter.arr_periapsis);
	for(int i = 0; i < shared_num_deps && capture_departures != NULL; i++)
		cache_arrival_dvs_in_tree(capture_departures[i], calc_data.dv_filter.dep_periapsis,
			calc_data.dv_filter.arr_periapsis);

	gettimeofday(&end, NULL);  // Record the ending time
	elapsed_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
	printf("----- | Total elapsed time: %.3f s | ---------\n", elapsed_time);
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	printf("Phase wall times: initialization %.3f s, search %.3f s, optimization/refinement %.3f s, filtering %.3f s\n",
		initialization_seconds, search_seconds, optimization_seconds, filtering_seconds);
#endif

	int num_itins = 0, num_nodes = 0;
	for(int i = 0; i < num_deps; i++) num_itins += get_number_of_itineraries(departures[i]);
	for(int i = 0; i < num_deps; i++) num_nodes += get_total_number_of_stored_steps(departures[i]);

	printf("\n%d itineraries found!\nNumber of Nodes: %d\n", num_itins, num_nodes);
	printf("%d of %d candidate routes passed optimization and the final limit checks\n",
		num_itins, results.num_candidate_routes);

	results.departures = departures;
	results.num_deps = num_deps;
	results.num_nodes = num_nodes;
	results.num_itins = num_itins;
	results.capture_departures = capture_departures;
	results.capture_num_deps = capture_departures == NULL ? 0 : shared_num_deps;
	if(capture_departures != NULL) {
		for(int i = 0; i < shared_num_deps; i++) {
			results.capture_num_itins += get_number_of_itineraries(capture_departures[i]);
			results.capture_num_nodes += get_total_number_of_stored_steps(capture_departures[i]);
		}
	}
	results.rejections = get_search_rejections();
	char rejection_reason[200];
	describe_search_rejections(&results.rejections, rejection_reason, sizeof(rejection_reason));
	if(num_itins == 0 && rejection_reason[0] != '\0') printf("%s\n", rejection_reason);

	set_moon_assist_enabled(previous_moon_assist);
	set_moon_aware_flybys_enabled(previous_moon_aware);
	return results;
}

struct Itin_Calc_Results search_for_itineraries(Itin_Calc_Data calc_data) {
	return search_for_itineraries_internal(calc_data, 0);
}

struct Itin_Calc_Results search_for_itineraries_both_modes(Itin_Calc_Data calc_data) {
	return search_for_itineraries_internal(calc_data, 1);
}
