#include "transfer_search_internal.h"
#include "search_cancel.h"
#include "search_rejections.h"
#include "tools/tool_funcs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static int routes_blocked_by_moons;

static int route_passes_filters(struct ItinStep *arrival, const Itin_Calc_Data *calc_data) {
	if(calc_data->seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET &&
		arrival->body != calc_data->seq_info.to_target.arr_body) return 0;
	struct PorkchopPoint point = create_porkchop_point(arrival,
		calc_data->dv_filter.dep_periapsis, calc_data->dv_filter.arr_periapsis);
	struct ItinStep *departure = get_first(arrival);
	if(!isfinite(point.dv_dep) || !isfinite(point.dv_dsm) || !isfinite(point.dv_flyby)) return 0;
	if(arrival->date > calc_data->jd_max_arr || departure->date < calc_data->jd_min_dep ||
		departure->date > calc_data->jd_max_dep) {
		count_search_rejection(REJECT_TIME_LIMIT);
		return 0;
	}
	double satellite_dv = point.dv_dsm;
	if(calc_data->dv_filter.last_transfer_type == TF_CAPTURE) satellite_dv += point.dv_arr_cap;
	if(calc_data->dv_filter.last_transfer_type == TF_CIRC) satellite_dv += point.dv_arr_circ;
	if(point.dv_dep > calc_data->dv_filter.max_depdv) {
		count_search_rejection(REJECT_DEPARTURE_DV);
		return 0;
	}
	if(satellite_dv + point.dv_flyby > calc_data->dv_filter.max_satdv) {
		count_search_rejection(REJECT_SATELLITE_DV);
		return 0;
	}
	if(point.dv_dep + satellite_dv + point.dv_flyby > calc_data->dv_filter.max_totdv) {
		count_search_rejection(REJECT_TOTAL_DV);
		return 0;
	}
	if(route_blocked_by_moons(arrival)) {
		routes_blocked_by_moons++;
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

static int prune_invalid_route_leaves(struct ItinStep *step, const Itin_Calc_Data *calc_data) {
	for(int child_index = 0; child_index < step->num_next_nodes;) {
		struct ItinStep *child = step->next[child_index];
		int valid = child->num_next_nodes == 0 ?
			route_passes_filters(child, calc_data) : prune_invalid_route_leaves(child, calc_data);
		if(valid) child_index++;
		else detach_itinerary_child(step, child_index);
	}
	return step->num_next_nodes > 0;
}


struct Itin_Calc_Results search_for_itineraries(Itin_Calc_Data calc_data) {
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

	int num_deps = (int) (calc_data.jd_max_dep-calc_data.jd_min_dep+1);

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

	int initial_body_count = 1;
	int resonant_samples = 0;
	if(calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
		initial_body_count = 0;
		for(int i = 0; i < calc_data.seq_info.to_target.num_flyby_bodies; i++) {
			if(calc_data.seq_info.to_target.flyby_bodies[i] != calc_data.seq_info.to_target.dep_body)
				initial_body_count++;
			else if(calc_data.seq_info.to_target.allow_resonant)
				resonant_samples = resonant_departure_sample_count();
		}
	}
	thread_progress_and_prop_info = (struct Thread_Progress_and_Prop_Info) {
		.jd_min_dep = calc_data.jd_min_dep,
		.jd_max_dep = calc_data.jd_max_dep,
		.total_initial_samples = num_deps * (initial_body_count * NUM_INITIAL_TRANSFERS_PER_BODY + resonant_samples),
		.has_post_processing = calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET,
		.search_start_time = progress_clock()
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
		for(int departure_index = 0; departure_index < (int)(calc_data.jd_max_dep - calc_data.jd_min_dep + 1); departure_index++)
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
		for(int departure_index = 0; departure_index < (int)(calc_data.jd_max_dep - calc_data.jd_min_dep + 1); departure_index++) {
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
	int valid_departures = 0;
	for(int i = 0; i < num_deps; i++) {
		if(departures[i] == NULL || departures[i]->num_next_nodes == 0) {
			free_itinerary(departures[i]);
			continue;
		}
		departures[valid_departures++] = departures[i];
	}
	num_deps = valid_departures;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	filtering_seconds += progress_clock() - phase_start;
#endif
	if(calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
		if(get_thread_counter(3) == 0) {
			PoweredFlybyOptimizationStats powered_stats;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
			phase_start = progress_clock();
#endif
			optimize_powered_flyby_routes(departures, num_deps, &calc_data,
				POWERED_ROUTE_REFINEMENT_LIMIT, &powered_stats);
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
			printf("Powered flyby optimization: %d/%d improved, %d valid, %d SOI-corrected, best saving %.2f m/s, best full dV %.3f -> %.3f m/s, exact scoring %.2f s, timing refinement %.2f s\n",
				powered_stats.routes_improved, powered_stats.routes_considered,
				powered_stats.routes_valid, powered_stats.routes_corrected,
				powered_stats.best_delta_v_saving, powered_stats.best_initial_total_dv,
				powered_stats.best_refined_total_dv, powered_stats.exact_scoring_seconds,
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
		valid_departures = 0;
		routes_blocked_by_moons = 0;
		for(int i = 0; i < num_deps; i++) {
			if(!prune_invalid_route_leaves(departures[i], &calc_data)) {
				free_itinerary(departures[i]);
				continue;
			}
			departures[valid_departures++] = departures[i];
		}
		num_deps = valid_departures;
		int itineraries_after_prune = 0;
		for(int i = 0; i < num_deps; i++)
			itineraries_after_prune += get_number_of_itineraries(departures[i]);
		results.powered_itineraries_pruned = itineraries_before_prune - itineraries_after_prune;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
		filtering_seconds += progress_clock() - phase_start;
#endif
		if(routes_blocked_by_moons > 0)
			printf("Moon check removed %d of %d routes (hit a moon or passed inside a moon's sphere of influence)\n",
				routes_blocked_by_moons, itineraries_before_prune);
	}

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

	results.departures = departures;
	results.num_deps = num_deps;
	results.num_nodes = num_nodes;
	results.num_itins = num_itins;
	results.rejections = get_search_rejections();
	char rejection_reason[200];
	describe_search_rejections(&results.rejections, rejection_reason, sizeof(rejection_reason));
	if(num_itins == 0 && rejection_reason[0] != '\0') printf("%s\n", rejection_reason);

	set_moon_assist_enabled(previous_moon_assist);
	set_moon_aware_flybys_enabled(previous_moon_aware);
	return results;
}
