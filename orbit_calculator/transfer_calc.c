#include "transfer_calc.h"
#include "powered_flyby_optimizer.h"
#include "tools/tool_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <math.h>
#include "tools/thread_pool.h"



#ifdef KMAT_ENABLE_SEARCH_PROFILE
typedef struct Itin_Search_Profile {
	double initial_grid_seconds;
	double recursive_search_seconds;
	unsigned long long initial_lambert_calls;
	unsigned long long recursive_lambert_calls;
	ItinFlybyProfileStats *flyby_stats;
} Itin_Search_Profile;
#endif

typedef struct Itin_Calc_Thread_Args {
	struct ItinStep **departures;
	PoweredSeedSearchStats *seed_stats;
	Itin_Calc_Data *calc_data;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	Itin_Search_Profile *profiles;
	Body **profile_bodies;
	int num_profile_bodies;
#endif
} Itin_Calc_Thread_Args;

struct Thread_Progress_and_Prop_Info {
	double jd_min_dep;
	double jd_max_dep;
	int total_initial_samples;
	int has_post_processing;
	double search_start_time;
	double search_end_time;
	double max_progress;
} thread_progress_and_prop_info;

static double progress_clock(void) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return now.tv_sec + now.tv_usec / 1e6;
}

#define INITIAL_SAMPLE_PROGRESS_COUNTER 5

static int itinerary_search_cancelled(void *context) {
	(void)context;
	return get_thread_counter(3) > 0;
}

static void show_departure_progress(double total_departures) {
	int completed = get_thread_counter(1);
	int started = get_thread_counter(0);
	int active = get_thread_counter(4);
	if(total_departures > 0 && started > total_departures) started = (int)total_departures;
	char status[128];
	snprintf(status, sizeof(status), "Departure dates: %d/%d complete, %d started, %d active",
		completed, (int)total_departures, started, active);
	Transfer_Calc_Status calc_status = get_current_transfer_calc_status();
	show_progress(status, calc_status.progress, 1);
}

static int route_passes_filters(struct ItinStep *arrival, const Itin_Calc_Data *calc_data) {
	if(calc_data->seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET &&
		arrival->body != calc_data->seq_info.to_target.arr_body) return 0;
	struct PorkchopPoint point = create_porkchop_point(arrival,
		calc_data->dv_filter.dep_periapsis, calc_data->dv_filter.arr_periapsis);
	struct ItinStep *departure = get_first(arrival);
	if(!isfinite(point.dv_dep) || !isfinite(point.dv_dsm) || !isfinite(point.dv_flyby) ||
		arrival->date > calc_data->jd_max_arr || departure->date < calc_data->jd_min_dep ||
		departure->date > calc_data->jd_max_dep) return 0;
	double satellite_dv = point.dv_dsm;
	if(calc_data->dv_filter.last_transfer_type == TF_CAPTURE) satellite_dv += point.dv_arr_cap;
	if(calc_data->dv_filter.last_transfer_type == TF_CIRC) satellite_dv += point.dv_arr_circ;
	return point.dv_dep <= calc_data->dv_filter.max_depdv &&
		satellite_dv <= calc_data->dv_filter.max_satdv &&
		point.dv_dep + satellite_dv + point.dv_flyby <= calc_data->dv_filter.max_totdv;
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

const int NUM_INITIAL_TRANSFERS_PER_BODY = 500;


void *calc_itins_from_departure(void *args) {
	Itin_Calc_Thread_Args *thread_args = (struct Itin_Calc_Thread_Args *)args;
	Itin_Calc_Data *calc_data = thread_args->calc_data;

	struct ItinStep **departures = thread_args->departures;
	struct Dv_Filter dv_filter = calc_data->dv_filter;
	double jd_min_dep = calc_data->jd_min_dep;
	double jd_max_dep = calc_data->jd_max_dep;
	double max_total_duration = calc_data->max_duration;

	enum ItinSequenceInfoType itin_seq_type = calc_data->seq_info.to_target.type;

	CelestSystem *system;
	Body *dep_body,
		*arr_body,			// only to_target: target body
		**fly_by_bodies;	// to_target: allowed fly_by_bodies; spec_seq: bodies in sequence
	int num_initial_transfers,
			num_steps = 0,		// only spec_seq: number of steps in sequence
			num_flyby_bodies;	// only to_target: number of allowed fly_by_bodies

	enum Transfer_Type tt = dv_filter.last_transfer_type == TF_CIRC ? circcirc : dv_filter.last_transfer_type == TF_CAPTURE ? circcap : circfb;

	if(itin_seq_type == ITIN_SEQ_INFO_TO_TARGET) {
		// to target
		system = calc_data->seq_info.to_target.system;
		dep_body = calc_data->seq_info.to_target.dep_body;
		arr_body = calc_data->seq_info.to_target.arr_body;
		fly_by_bodies = calc_data->seq_info.to_target.flyby_bodies;
		num_flyby_bodies = calc_data->seq_info.to_target.num_flyby_bodies;
		num_initial_transfers = NUM_INITIAL_TRANSFERS_PER_BODY * num_flyby_bodies;
	} else {
		// specific sequence
		system = calc_data->seq_info.spec_seq.system;
		dep_body = calc_data->seq_info.spec_seq.bodies[0];
		fly_by_bodies = calc_data->seq_info.spec_seq.bodies;
		num_steps = calc_data->seq_info.spec_seq.num_steps;
		num_initial_transfers = NUM_INITIAL_TRANSFERS_PER_BODY;
	}

	// index in departure array (iterated over)
	int index = get_incr_thread_counter(0);
	double jd_diff = jd_max_dep - jd_min_dep + 1;

	double jd_dep = jd_min_dep + index;
	struct ItinStep *curr_step;
	OSV osv_body0, osv_body1;

	while(jd_dep <= jd_max_dep && get_thread_counter(3) == 0) {
		begin_powered_seed_search(&thread_args->seed_stats[index]);
		incr_thread_counter_by_amount(4, 1);
		show_departure_progress(jd_diff);
#ifdef KMAT_ENABLE_SEARCH_PROFILE
		Itin_Search_Profile *profile = &thread_args->profiles[index];
		struct timeval phase_start, phase_end;
		gettimeofday(&phase_start, NULL);
#endif
		osv_body0 = system->prop_method == ORB_ELEMENTS ?
					osv_from_elements(dep_body->orbit, jd_dep) :
					osv_from_ephem(dep_body->ephem, dep_body->num_ephems, jd_dep, system->cb);

		curr_step = departures[index];
		curr_step->body = dep_body;
		curr_step->date = jd_dep;
		curr_step->r = osv_body0.r;
		curr_step->v_body = osv_body0.v;
		curr_step->v_dep = vec3(0, 0, 0);
		curr_step->v_arr = vec3(0, 0, 0);
		curr_step->num_next_nodes = num_initial_transfers;
		curr_step->prev = NULL;
		curr_step->next = (struct ItinStep **) malloc(curr_step->num_next_nodes * sizeof(struct ItinStep *));

		double jd_max_arr = jd_dep + max_total_duration < calc_data->jd_max_arr ? jd_dep + max_total_duration : calc_data->jd_max_arr;

		Body *next_step_body;
		int num_next_bodies = itin_seq_type == ITIN_SEQ_INFO_TO_TARGET ? num_flyby_bodies : 1;
		int next_step_id = 0;
		int pending_progress_samples = 0;

		for(int i = 0; i < num_next_bodies && !itinerary_search_cancelled(NULL); i++) {
			if(itin_seq_type == ITIN_SEQ_INFO_TO_TARGET) {
				next_step_body = fly_by_bodies[i];
				if(next_step_body == dep_body) continue;
			} else {
				next_step_body = fly_by_bodies[1];
			}

			OSV arr_body_temp_osv = system->prop_method == ORB_ELEMENTS ?
										   osv_from_elements(next_step_body->orbit, jd_dep) :
										   osv_from_ephem(next_step_body->ephem, next_step_body->num_ephems, jd_dep, system->cb);

			double r0 = mag_vec3(osv_body0.r), r1 = mag_vec3(arr_body_temp_osv.r);
			double r_ratio =  r1/r0;
			Hohmann hohmann = calc_hohmann_transfer(r0, r1, system->cb);
			double hohmann_dur = hohmann.dur/86400;
			double min_duration = 0.4 * hohmann_dur;
			double max_duration = (4*(r_ratio-0.85)*(r_ratio-0.85)+1.5) * hohmann_dur; if(max_duration/hohmann_dur > 3) max_duration = hohmann_dur*3;
			double max_min_duration_diff = max_duration - min_duration;

			for(int j = 0; j < NUM_INITIAL_TRANSFERS_PER_BODY && !itinerary_search_cancelled(NULL); j++) {
				double jd_arr = jd_dep + min_duration + max_min_duration_diff * j/(NUM_INITIAL_TRANSFERS_PER_BODY - 1);

				if(jd_arr > jd_max_arr) break;

				osv_body1 = system->prop_method == ORB_ELEMENTS ?
							osv_from_elements(next_step_body->orbit, jd_arr) :
							osv_from_ephem(next_step_body->ephem, next_step_body->num_ephems, jd_arr, system->cb);

#ifdef KMAT_ENABLE_SEARCH_PROFILE
				profile->initial_lambert_calls++;
#endif
				Lambert3 tf = calc_lambert3_selected_cancellable(osv_body0.r, osv_body1.r,
					(jd_arr - jd_dep) * 86400, system->cb, itinerary_search_cancelled, NULL);
				if(++pending_progress_samples >= 10) {
					incr_thread_counter_by_amount(INITIAL_SAMPLE_PROGRESS_COUNTER, pending_progress_samples);
					pending_progress_samples = 0;
				}
				if(itinerary_search_cancelled(NULL)) break;
				
				
				double dv_dep, dv_arr;
				if(dep_body != NULL) {
					double vinf = fabs(mag_vec3(subtract_vec3(tf.v0, osv_body0.v)));
					dv_dep = tt % 2 == 0 ? dv_capture(dep_body, alt2radius(dep_body, dv_filter.dep_periapsis), vinf) : dv_circ(dep_body,alt2radius(dep_body, dv_filter.dep_periapsis),vinf);
				} else dv_dep = mag_vec3(osv_body0.v);
				
				if(arr_body != NULL) {
					double vinf = fabs(mag_vec3(subtract_vec3(tf.v1, osv_body1.v)));
					if(tt < 2) dv_arr = dv_capture(arr_body, alt2radius(arr_body, dv_filter.arr_periapsis), vinf);
					else if(tt < 4) dv_arr = dv_circ(arr_body, alt2radius(arr_body, dv_filter.arr_periapsis), vinf);
					else dv_arr = 0;
				} else {
					dv_arr = mag_vec3(osv_body1.v);
				}
				
				if(dv_dep > dv_filter.max_totdv || dv_dep > dv_filter.max_depdv) continue;
				if(num_steps == 2 && (dv_dep + dv_arr > dv_filter.max_totdv || dv_arr > dv_filter.max_satdv)) continue;

				curr_step = get_first(curr_step);
				curr_step->next[next_step_id] = (struct ItinStep *) malloc(sizeof(struct ItinStep));
				curr_step->next[next_step_id]->prev = curr_step;
				curr_step->next[next_step_id]->next = NULL;

				curr_step = curr_step->next[next_step_id];

				curr_step->body = next_step_body;
				curr_step->date = jd_arr;
				curr_step->r = osv_body1.r;
				curr_step->v_dep = tf.v0;
				curr_step->v_arr = tf.v1;
				curr_step->v_body = osv_body1.v;
				curr_step->num_next_nodes = 0;

				next_step_id++;
			}
			if(pending_progress_samples > 0) {
				incr_thread_counter_by_amount(INITIAL_SAMPLE_PROGRESS_COUNTER, pending_progress_samples);
				pending_progress_samples = 0;
			}
		}

		curr_step = get_first(curr_step);
		curr_step->num_next_nodes = next_step_id;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
		gettimeofday(&phase_end, NULL);
		profile->initial_grid_seconds += (phase_end.tv_sec - phase_start.tv_sec) +
			(phase_end.tv_usec - phase_start.tv_usec) / 1000000.0;

		set_itin_search_flyby_profile(thread_args->profile_bodies,
			thread_args->num_profile_bodies, profile->flyby_stats);
		set_itin_search_lambert_counter(&profile->recursive_lambert_calls);
		gettimeofday(&phase_start, NULL);
#endif
		if(get_thread_counter(3) == 0 && itin_seq_type == ITIN_SEQ_INFO_TO_TARGET) {
			int num_of_end_nodes = find_copy_and_store_end_nodes(curr_step, arr_body);

			// remove end nodes that do not satisfy dv requirements
			num_of_end_nodes = remove_end_nodes_that_do_not_satisfy_dv_requirements(curr_step, num_of_end_nodes, &dv_filter);
			if(curr_step->num_next_nodes > 0) {
				continue_to_next_steps_and_check_for_valid_itins(curr_step, num_of_end_nodes, &calc_data->seq_info.to_target, jd_max_arr, max_total_duration, &dv_filter);
			}
		} else if(get_thread_counter(3) == 0) {
			if(num_steps > 2 && !itinerary_search_cancelled(NULL)) {
				int itin_del = 0;
				int init_num_next_nodes = curr_step->num_next_nodes;
				for(int i = 0; i < init_num_next_nodes && !itinerary_search_cancelled(NULL); i++) {
					if(!calc_next_spec_itin_step(curr_step->next[i - itin_del], system, fly_by_bodies, jd_max_arr, &dv_filter, num_steps, 2)) itin_del++;
				}
			}
		}

#ifdef KMAT_ENABLE_SEARCH_PROFILE
		set_itin_search_lambert_counter(NULL);
		gettimeofday(&phase_end, NULL);
		profile->recursive_search_seconds += (phase_end.tv_sec - phase_start.tv_sec) +
			(phase_end.tv_usec - phase_start.tv_usec) / 1000000.0;
		set_itin_search_flyby_profile(NULL, 0, NULL);
#endif

		incr_thread_counter_by_amount(2, get_number_of_itineraries(get_first(curr_step)));
		if(get_thread_counter(3) == 0) get_incr_thread_counter(1);
		incr_thread_counter_by_amount(4, -1);
		show_departure_progress(jd_diff);
		index = get_incr_thread_counter(0);
		jd_dep = jd_min_dep + index;
	}
	begin_powered_seed_search(NULL);
	return NULL;
}

struct Itin_Calc_Results search_for_itineraries(Itin_Calc_Data calc_data) {
	struct Itin_Calc_Results results = {
		.best_unpowered_seed_total_dv = HUGE_VAL,
		.best_powered_initial_total_dv = HUGE_VAL,
		.best_powered_refined_total_dv = HUGE_VAL
	};

	struct timeval start, end;
	double elapsed_time;
	gettimeofday(&start, NULL);  // Record the ending time

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
		return results;
	}
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	thread_args.profiles = profiles;
	thread_args.profile_bodies = profile_bodies;
	thread_args.num_profile_bodies = num_profile_bodies;
#endif

	int initial_body_count = 1;
	if(calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
		initial_body_count = 0;
		for(int i = 0; i < calc_data.seq_info.to_target.num_flyby_bodies; i++)
			if(calc_data.seq_info.to_target.flyby_bodies[i] != calc_data.seq_info.to_target.dep_body)
				initial_body_count++;
	}
	thread_progress_and_prop_info = (struct Thread_Progress_and_Prop_Info) {
		.jd_min_dep = calc_data.jd_min_dep,
		.jd_max_dep = calc_data.jd_max_dep,
		.total_initial_samples = num_deps * initial_body_count * NUM_INITIAL_TRANSFERS_PER_BODY,
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
	thread_pool = use_thread_pool32(calc_itins_from_departure, &thread_args);
	join_thread_pool(thread_pool);
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
	unsigned long long initial_lambert_calls = 0;
	unsigned long long recursive_lambert_calls = 0;
	for(int i = 0; i < num_deps; i++) {
		initial_grid_seconds += profiles[i].initial_grid_seconds;
		recursive_search_seconds += profiles[i].recursive_search_seconds;
		initial_lambert_calls += profiles[i].initial_lambert_calls;
		recursive_lambert_calls += profiles[i].recursive_lambert_calls;
	}
	printf("Initial grid: %.3f summed worker-seconds, %llu Lambert solves\n",
		initial_grid_seconds, initial_lambert_calls);
	printf("Recursive search: %.3f summed worker-seconds, %llu Lambert solves\n",
		recursive_search_seconds, recursive_lambert_calls);
	for(int body_index = 0; body_index < num_profile_bodies; body_index++) {
		unsigned long long searches = 0;
		unsigned long long left_evaluations = 0;
		unsigned long long right_evaluations = 0;
		for(int departure_index = 0; departure_index < (int)(calc_data.jd_max_dep - calc_data.jd_min_dep + 1); departure_index++) {
			ItinFlybyProfileStats stats = profiles[departure_index].flyby_stats[body_index];
			searches += stats.searches;
			left_evaluations += stats.left_evaluations;
			right_evaluations += stats.right_evaluations;
		}
		if(searches > 0)
			printf("Flyby %s: %llu searches, %llu left / %llu right root evaluations\n",
				profile_bodies[body_index]->name, searches, left_evaluations, right_evaluations);
	}
	free(profiles);
	free(flyby_profiles);
	#endif

	// Remove empty departure trees and compact the results in one pass.
	int valid_departures = 0;
	for(int i = 0; i < num_deps; i++) {
		if(departures[i] == NULL || departures[i]->num_next_nodes == 0) {
			free_itinerary(departures[i]);
			continue;
		}
		departures[valid_departures++] = departures[i];
	}
	num_deps = valid_departures;
	if(calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
		if(get_thread_counter(3) == 0) {
			PoweredFlybyOptimizationStats powered_stats;
			optimize_powered_flyby_routes(departures, num_deps, &calc_data,
				POWERED_ROUTE_REFINEMENT_LIMIT, &powered_stats);
			results.powered_routes_valid = powered_stats.routes_valid;
			results.powered_routes_corrected = powered_stats.routes_corrected;
			results.powered_initial_rejections = powered_stats.initial_rejections;
			results.powered_repairs_attempted = powered_stats.repairs_attempted;
			results.powered_repairs_recovered = powered_stats.repairs_recovered;
			results.powered_repairs_competing = powered_stats.repairs_competing;
			results.powered_challenges_refined = powered_stats.challenges_refined;
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
		}
		int itineraries_before_prune = 0;
		for(int i = 0; i < num_deps; i++)
			itineraries_before_prune += get_number_of_itineraries(departures[i]);
		valid_departures = 0;
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
	}

	gettimeofday(&end, NULL);  // Record the ending time
	elapsed_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
	printf("----- | Total elapsed time: %.3f s | ---------\n", elapsed_time);

	int num_itins = 0, num_nodes = 0;
	for(int i = 0; i < num_deps; i++) num_itins += get_number_of_itineraries(departures[i]);
	for(int i = 0; i < num_deps; i++) num_nodes += get_total_number_of_stored_steps(departures[i]);

	printf("\n%d itineraries found!\nNumber of Nodes: %d\n", num_itins, num_nodes);

	results.departures = departures;
	results.num_deps = num_deps;
	results.num_nodes = num_nodes;
	results.num_itins = num_itins;

	return results;
}

struct Transfer_Calc_Status get_current_transfer_calc_status() {
	double jd_diff = thread_progress_and_prop_info.jd_max_dep-thread_progress_and_prop_info.jd_min_dep+1;
	int num_deps = get_thread_counter(1);
	int num_deps_started = get_thread_counter(0);
	int num_deps_active = get_thread_counter(4);
	int initial_samples = get_thread_counter(INITIAL_SAMPLE_PROGRESS_COUNTER);
	if(jd_diff > 0 && num_deps_started > jd_diff) num_deps_started = (int)jd_diff;
	double completed_date_progress = jd_diff > 0 ? fmin(1, (double)num_deps / jd_diff) : 0;
	double initial_sample_progress = thread_progress_and_prop_info.total_initial_samples > 0 ?
		fmin(1, (double)initial_samples / thread_progress_and_prop_info.total_initial_samples) :
		completed_date_progress;
	double progress = 0.75 * initial_sample_progress + 0.25 * completed_date_progress;
	if(completed_date_progress >= 1) progress = 1;
	int post_processing = thread_progress_and_prop_info.has_post_processing && completed_date_progress >= 1;
	if(thread_progress_and_prop_info.has_post_processing) {
		// Share of the bar for scoring/refinement, predicted from the route count and elapsed search time.
		double now = progress_clock();
		if(post_processing && thread_progress_and_prop_info.search_end_time == 0)
			thread_progress_and_prop_info.search_end_time = now;
		double search_fraction = post_processing ? 1 : fmax(progress, 0.05);
		double search_seconds = (post_processing ? thread_progress_and_prop_info.search_end_time : now) -
			thread_progress_and_prop_info.search_start_time;
		double predicted_post_seconds = ((double)get_thread_counter(2) / search_fraction +
			POWERED_PROGRESS_REFINE_UNITS) * POWERED_PROGRESS_SECONDS_PER_UNIT;
		double predicted_search_seconds = search_seconds / search_fraction;
		double post_share = predicted_post_seconds / (predicted_search_seconds + predicted_post_seconds);
		if(post_processing) {
			int post_total = get_thread_counter(POWERED_PROGRESS_TOTAL_COUNTER);
			double post_progress = post_total > 0 ?
				fmin(1, (double)get_thread_counter(POWERED_PROGRESS_DONE_COUNTER) / post_total) : 0;
			progress = (1 - post_share) + post_share * post_progress;
		} else {
			progress *= 1 - post_share;
		}
		// Estimates shift as the search proceeds; the bar must never move backwards.
		progress = fmax(progress, thread_progress_and_prop_info.max_progress);
		thread_progress_and_prop_info.max_progress = progress;
	}

	return (struct Transfer_Calc_Status) {
		.num_itins = get_thread_counter(2),
		.num_deps = num_deps,
		.num_deps_started = num_deps_started,
		.num_deps_active = num_deps_active,
		.jd_diff = jd_diff,
		.progress = progress,
		.post_processing = post_processing
	};
}
