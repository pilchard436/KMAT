#include "itin_tool.h"
#include "powered_seed_selection.h"
#include "search_cancel.h"
#include "search_rejections.h"
#include "transfer_search_internal.h"
#include "tools/starting_orbit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>






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

static int porkchop_exact_flybys;
int set_porkchop_exact_flybys(int exact) {
	int previous = porkchop_exact_flybys;
	porkchop_exact_flybys = !!exact;
	return previous;
}

struct PorkchopPoint create_porkchop_point(struct ItinStep *itin, double dep_periapsis, double arr_periapsis) {
	struct PorkchopPoint pp;
	pp.arrival = itin;
	pp.dur = get_itinerary_duration(itin);

	Vector3 arrival_excess = subtract_vec3(itin->v_arr, itin->v_body);
	double vinf = mag_vec3(arrival_excess);
	double arrival_radius = alt2radius(itin->body, arr_periapsis);
	pp.dv_arr_cap = dv_capture(itin->body, arrival_radius, vinf);
	pp.dv_arr_circ = dv_circ(itin->body, arrival_radius, vinf);
	// Search nodes call this for every candidate, so assists are only applied when asked for.
	if(moon_assist_is_active() && itin->body != NULL) {
		pp.dv_arr_cap = arrival_dv_with_assist(itin->body, arrival_excess, arrival_radius, 0, pp.dv_arr_cap, itin->date, NULL);
		pp.dv_arr_circ = arrival_dv_with_assist(itin->body, arrival_excess, arrival_radius, 1, pp.dv_arr_circ, itin->date, NULL);
	}

	pp.dv_dsm = 0;
	pp.dv_flyby = 0;
	while(itin->prev->prev != NULL) {
		if(itin->body == NULL)
			pp.dv_dsm += mag_vec3(subtract_vec3(itin->next[0]->v_dep, itin->v_arr));
		// A DSM right after a flyby must not hide that flyby's burn.
		if(itin->prev->body != NULL && itin->prev->prev != NULL) {
			struct ItinStep *flyby = itin->prev;
			PoweredFlybyResult flyby_result;
			double minimum_periapsis = flyby_minimum_periapsis(flyby->body);
			// Search nodes skip the moon analysis for speed; the analyzer turns it on so totals match the details.
			int flyby_ok = (porkchop_exact_flybys || moon_aware_flybys_is_active()) ?
				calculate_flyby_with_moons(flyby->v_arr, itin->v_dep, flyby->v_body, flyby->body,
					minimum_periapsis, flyby->date, &flyby_result) :
				calculate_powered_flyby(flyby->v_arr, itin->v_dep, flyby->v_body,
					flyby->body, minimum_periapsis, &flyby_result);
			if(flyby_ok)
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
	if(!starting_orbit_departure_dv(itin->prev->body, departure_vinf,
		alt2radius(itin->prev->body, dep_periapsis), &pp.dv_dep))
		pp.dv_dep = dv_circ(itin->prev->body, alt2radius(itin->prev->body, dep_periapsis),
			mag_vec3(departure_vinf));
	return pp;
}

int calc_next_spec_itin_step(struct ItinStep *curr_step, CelestSystem *system, Body **bodies, double jd_max_arr, struct Dv_Filter *dv_filter, int num_steps, int step) {
	if(itinerary_search_cancelled(NULL)) return curr_step->num_next_nodes > 0;
	double max_duration = jd_max_arr-curr_step->date;
	double min_duration = minimum_leg_days(system);
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
			count_search_rejection(dv_sat + porkchop_point.dv_flyby > dv_filter->max_satdv ? REJECT_SATELLITE_DV : REJECT_TOTAL_DV);
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
		double min_duration = minimum_leg_days(seq_info->system);
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
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	double profile_start = itin_search_profile_tls != NULL ? progress_clock() : 0;
#endif
	int num_of_end_nodes = 0;
	for(int i = 0; i < curr_step->num_next_nodes; i++) {
		if(curr_step->next[i]->body == arr_body) num_of_end_nodes++;
	}
	// if there were end nodes found, copy them and store them at the end
	if(num_of_end_nodes > 0) {
		struct ItinStep ** new_next = realloc(curr_step->next, (num_of_end_nodes+curr_step->num_next_nodes) * sizeof(struct ItinStep *));
		if(new_next == NULL) {
			printf("\nERROR WHILE REALLOCATING ITINERARIES DURING END NODE COPYING!!!\n");
#ifdef KMAT_ENABLE_SEARCH_PROFILE
			if(itin_search_profile_tls != NULL)
				itin_search_profile_tls->recursive_end_node_seconds += progress_clock() - profile_start;
#endif
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
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	if(itin_search_profile_tls != NULL)
		itin_search_profile_tls->recursive_end_node_seconds += progress_clock() - profile_start;
#endif
	return num_of_end_nodes;
}

int remove_end_nodes_that_do_not_satisfy_dv_requirements(struct ItinStep *curr_step, int num_of_end_nodes, struct Dv_Filter *dv_filter) {
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	double profile_start = itin_search_profile_tls != NULL ? progress_clock() : 0;
#endif
	for(int i = curr_step->num_next_nodes-num_of_end_nodes; i < curr_step->num_next_nodes; i++) {
		struct ItinStep *next = curr_step->next[i];
		struct PorkchopPoint porkchop_point = create_porkchop_point(next, dv_filter->dep_periapsis, dv_filter->arr_periapsis);
			double dv_sat = porkchop_point.dv_dsm;
		if(dv_filter->last_transfer_type == 1) dv_sat += porkchop_point.dv_arr_cap;
		if(dv_filter->last_transfer_type == 2) dv_sat += porkchop_point.dv_arr_circ;
			if(dv_sat + porkchop_point.dv_flyby > dv_filter->max_satdv ||
				porkchop_point.dv_dep + dv_sat + porkchop_point.dv_flyby > dv_filter->max_totdv) {
			count_search_rejection(dv_sat + porkchop_point.dv_flyby > dv_filter->max_satdv ? REJECT_SATELLITE_DV : REJECT_TOTAL_DV);
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
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	if(itin_search_profile_tls != NULL)
		itin_search_profile_tls->recursive_dv_pruning_seconds += progress_clock() - profile_start;
#endif
	return num_of_end_nodes;
}


double itinerary_step_dv(struct ItinStep *step, double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type) {
	if(step == NULL || (step->prev == NULL && step->next == NULL)) return 0;
	if(step->body == NULL) {
		if(step->next == NULL || step->prev == NULL) return 0;
		return mag_vec3(subtract_vec3(step->next[0]->v_dep, step->v_arr));
	}
	if(step->prev == NULL) {
		Vector3 vinf = subtract_vec3(step->next[0]->v_dep, step->v_body);
		double departure_dv;
		if(starting_orbit_departure_dv(step->body, vinf, alt2radius(step->body, dep_periapsis), &departure_dv))
			return departure_dv;
		return dv_circ(step->body, alt2radius(step->body, dep_periapsis), mag_vec3(vinf));
	}
	if(step->next == NULL) {
		Vector3 excess = subtract_vec3(step->v_arr, step->v_body);
		double vinf = mag_vec3(excess), radius = alt2radius(step->body, arr_periapsis);
		if(last_transfer_type != TF_CAPTURE && last_transfer_type != TF_CIRC) return 0;
		int circularize = last_transfer_type == TF_CIRC;
		double plain = circularize ? dv_circ(step->body, radius, vinf) : dv_capture(step->body, radius, vinf);
		return moon_assist_is_active() ? arrival_dv_with_assist(step->body, excess, radius, circularize, plain, step->date, NULL) : plain;
	}
	PoweredFlybyResult flyby;
	double minimum_periapsis = flyby_minimum_periapsis(step->body);
	if(calculate_flyby_with_moons(step->v_arr, step->next[0]->v_dep, step->v_body, step->body, minimum_periapsis, step->date, &flyby))
		return flyby.burn_delta_v;
	return HUGE_VAL;
}
