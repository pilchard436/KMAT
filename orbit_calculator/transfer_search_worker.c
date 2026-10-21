#include "transfer_search_internal.h"
#include "search_cancel.h"
#include "search_rejections.h"
#include "tools/tool_funcs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static double first_leg_window_scale = 1;

void set_first_leg_window_scale(double scale) {
	first_leg_window_scale = scale;
}

const int NUM_INITIAL_TRANSFERS_PER_BODY = 500;

void *calc_itins_from_departure(void *args) {
	Itin_Calc_Thread_Args *thread_args = (struct Itin_Calc_Thread_Args *)args;
	Itin_Calc_Data *calc_data = thread_args->calc_data;
	int previous_moon_aware = set_moon_aware_flybys_enabled(calc_data->moon_aware_flybys);
	int previous_moon_assist = set_moon_assist_enabled(calc_data->moon_assist_arrival);

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
				if(next_step_body == dep_body && !calc_data->seq_info.to_target.allow_resonant) continue;
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
			double max_duration = first_leg_window_scale * (4*(r_ratio-0.85)*(r_ratio-0.85)+1.5) * hohmann_dur; if(max_duration/hohmann_dur > 3*first_leg_window_scale) max_duration = hohmann_dur*3*first_leg_window_scale;
			double max_min_duration_diff = max_duration - min_duration;

			// A return to the departure body only works with the revolution variants.
			int resonant_leg = next_step_body == dep_body;
			double departure_period_days = resonant_leg ?
				calc_orbital_period(constr_orbit_from_osv(osv_body0.r, osv_body0.v, system->cb)) / 86400 : 0;
			if(resonant_leg && !(departure_period_days > 0)) continue;
			int num_samples = resonant_leg ? resonant_departure_sample_count() : NUM_INITIAL_TRANSFERS_PER_BODY;

			for(int j = 0; j < num_samples && !itinerary_search_cancelled(NULL); j++) {
				int leg_variant = 0;
				double jd_arr;
				if(resonant_leg) {
					double duration_days;
					resonant_departure_sample(j, departure_period_days, &duration_days, &leg_variant);
					jd_arr = jd_dep + duration_days;
					if(jd_arr > jd_max_arr) continue;
				} else {
					jd_arr = jd_dep + min_duration + max_min_duration_diff * j/(NUM_INITIAL_TRANSFERS_PER_BODY - 1);
					if(jd_arr > jd_max_arr) break;
				}

				osv_body1 = system->prop_method == ORB_ELEMENTS ?
							osv_from_elements(next_step_body->orbit, jd_arr) :
							osv_from_ephem(next_step_body->ephem, next_step_body->num_ephems, jd_arr, system->cb);

#ifdef KMAT_ENABLE_SEARCH_PROFILE
				profile->initial_lambert_calls++;
#endif
				Lambert3 tf = calc_leg_variant_lambert(system->cb, osv_body0.r, osv_body1.r,
					(jd_arr - jd_dep) * 86400, leg_variant, itinerary_search_cancelled, NULL);
				if(++pending_progress_samples >= 10) {
					incr_thread_counter_by_amount(INITIAL_SAMPLE_PROGRESS_COUNTER, pending_progress_samples);
					pending_progress_samples = 0;
				}
				if(itinerary_search_cancelled(NULL)) break;
				if(resonant_leg && (tf.success == LAMBERT_FAIL_NAN || tf.success == LAMBERT_FAIL_ECC ||
					tf.success == LAMBERT_MAX_ITERATIONS)) continue;
				
				
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
				
				if(dv_dep > dv_filter.max_depdv) { count_search_rejection(REJECT_DEPARTURE_DV); continue; }
				if(dv_dep > dv_filter.max_totdv) { count_search_rejection(REJECT_TOTAL_DV); continue; }
				if(num_steps == 2 && dv_arr > dv_filter.max_satdv) { count_search_rejection(REJECT_SATELLITE_DV); continue; }
				if(num_steps == 2 && dv_dep + dv_arr > dv_filter.max_totdv) { count_search_rejection(REJECT_TOTAL_DV); continue; }

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
	set_moon_assist_enabled(previous_moon_assist);
	set_moon_aware_flybys_enabled(previous_moon_aware);
	return NULL;
}
