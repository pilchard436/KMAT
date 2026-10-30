#include "itin_tool.h"
#include "flyby_policy.h"
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

static int arrival_dv_cache_matches(const struct ItinStep *arrival, Vector3 incoming_excess,
		double arr_periapsis, int moon_assist_enabled) {
	const struct ItinArrivalDvCache *cache = &arrival->arrival_dv_cache;
	return cache->valid &&
		cache->incoming_excess.x == incoming_excess.x &&
		cache->incoming_excess.y == incoming_excess.y &&
		cache->incoming_excess.z == incoming_excess.z &&
		cache->date == arrival->date &&
		fabs(cache->arr_periapsis - arr_periapsis) <= 1e-6 &&
		(cache->moon_assist_enabled == moon_assist_enabled || cache->moon_assist_dvs_valid);
}

int cache_itinerary_arrival_dvs(struct ItinStep *arrival, double arr_periapsis) {
	if(arrival == NULL || arrival->body == NULL) return 0;

	Vector3 incoming_excess = subtract_vec3(arrival->v_arr, arrival->v_body);
	int moon_assist_enabled = moon_assist_is_active();
	if(arrival_dv_cache_matches(arrival, incoming_excess, arr_periapsis, moon_assist_enabled)) {
		if(arrival->arrival_dv_cache.moon_assist_dvs_valid) {
			arrival->arrival_dv_cache.dv_arr_cap = moon_assist_enabled ?
					arrival->arrival_dv_cache.dv_arr_cap_assisted :
					arrival->arrival_dv_cache.dv_arr_cap_plain;
			arrival->arrival_dv_cache.dv_arr_circ = moon_assist_enabled ?
					arrival->arrival_dv_cache.dv_arr_circ_assisted :
					arrival->arrival_dv_cache.dv_arr_circ_plain;
			arrival->arrival_dv_cache.moon_assist_enabled = moon_assist_enabled;
		}
		return 1;
	}

	double arrival_radius = alt2radius(arrival->body, arr_periapsis);
	double vinf = mag_vec3(incoming_excess);
	double dv_arr_cap_plain = dv_capture(arrival->body, arrival_radius, vinf);
	double dv_arr_circ_plain = dv_circ(arrival->body, arrival_radius, vinf);
	double dv_arr_cap_assisted = dv_arr_cap_plain;
	double dv_arr_circ_assisted = dv_arr_circ_plain;
	if(moon_assist_enabled) {
		dv_arr_cap_assisted = arrival_dv_with_assist(arrival->body, incoming_excess,
				arrival_radius, 0, dv_arr_cap_plain, arrival->date, NULL);
		dv_arr_circ_assisted = arrival_dv_with_assist(arrival->body, incoming_excess,
				arrival_radius, 1, dv_arr_circ_plain, arrival->date, NULL);
	}

	arrival->arrival_dv_cache = (struct ItinArrivalDvCache) {
		.dv_arr_cap = moon_assist_enabled ? dv_arr_cap_assisted : dv_arr_cap_plain,
		.dv_arr_circ = moon_assist_enabled ? dv_arr_circ_assisted : dv_arr_circ_plain,
		.dv_arr_cap_plain = dv_arr_cap_plain,
		.dv_arr_cap_assisted = dv_arr_cap_assisted,
		.dv_arr_circ_plain = dv_arr_circ_plain,
		.dv_arr_circ_assisted = dv_arr_circ_assisted,
		.incoming_excess = incoming_excess,
		.date = arrival->date,
		.arr_periapsis = arr_periapsis,
		.moon_assist_enabled = moon_assist_enabled,
		.moon_assist_dvs_valid = moon_assist_enabled,
		.valid = 1
	};
	return 1;
}

// Burn of the flyby at `flyby` that leads into `next`; HUGE_VAL when no flyby exists. With use_cache the plain
// powered-flyby burn is kept on `next`, because search pruning walks the same ancestors for every deeper end node.
// Only the search worker that owns the tree may use the cache; later phases read shared nodes from several threads.
static double route_flyby_burn(struct ItinStep *flyby, struct ItinStep *next, int use_cache) {
	PoweredFlybyResult flyby_result;
	double minimum_periapsis = flyby_minimum_periapsis(flyby->body);
	// Search nodes skip the moon analysis for speed; the analyzer turns it on so totals match the details.
	if(porkchop_exact_flybys || moon_aware_flybys_is_active())
		return calculate_flyby_with_moons(flyby->v_arr, next->v_dep, flyby->v_body, flyby->body,
			minimum_periapsis, flyby->date, &flyby_result) ? flyby_result.burn_delta_v : HUGE_VAL;
	struct ItinFlybyBurnCache *cache = &next->flyby_burn_cache;
	if(use_cache && cache->valid &&
		cache->v_arr_prev.x == flyby->v_arr.x && cache->v_arr_prev.y == flyby->v_arr.y && cache->v_arr_prev.z == flyby->v_arr.z &&
		cache->v_body_prev.x == flyby->v_body.x && cache->v_body_prev.y == flyby->v_body.y && cache->v_body_prev.z == flyby->v_body.z &&
		cache->v_dep.x == next->v_dep.x && cache->v_dep.y == next->v_dep.y && cache->v_dep.z == next->v_dep.z)
		return cache->burn;
	double burn = calculate_powered_flyby(flyby->v_arr, next->v_dep, flyby->v_body,
		flyby->body, minimum_periapsis, &flyby_result) ? flyby_result.burn_delta_v : HUGE_VAL;
	if(use_cache)
		*cache = (struct ItinFlybyBurnCache){.v_arr_prev = flyby->v_arr, .v_body_prev = flyby->v_body,
			.v_dep = next->v_dep, .burn = burn, .valid = 1};
	return burn;
}

// Departure, DSM and flyby delta-v of the route from its departure node up to `step`, which must not be the
// departure node itself. use_cache is for the search worker owning the tree; see route_flyby_burn.
static void accumulate_route_dvs(struct ItinStep *step, double dep_periapsis, int use_cache,
		double *dv_dep, double *dv_dsm, double *dv_flyby) {
	*dv_dsm = 0;
	*dv_flyby = 0;
	while(step->prev->prev != NULL) {
		if(step->body == NULL)
			*dv_dsm += mag_vec3(subtract_vec3(step->next[0]->v_dep, step->v_arr));
		// A DSM right after a flyby must not hide that flyby's burn.
		if(step->prev->body != NULL && step->prev->prev != NULL)
			*dv_flyby += route_flyby_burn(step->prev, step, use_cache);
		step = step->prev;
	}
	// The loop stops at the first-leg node, so a DSM on the first leg is counted here.
	if(step->body == NULL && step->next != NULL && step->num_next_nodes > 0)
		*dv_dsm += mag_vec3(subtract_vec3(step->next[0]->v_dep, step->v_arr));

	Vector3 departure_vinf = subtract_vec3(step->v_dep, step->prev->v_body);
	if(!starting_orbit_departure_dv(step->prev->body, departure_vinf,
		alt2radius(step->prev->body, dep_periapsis), dv_dep))
		*dv_dep = dv_circ(step->prev->body, alt2radius(step->prev->body, dep_periapsis),
			mag_vec3(departure_vinf));
}

struct PorkchopPoint create_porkchop_point(struct ItinStep *itin, double dep_periapsis, double arr_periapsis) {
	struct PorkchopPoint pp;
	pp.arrival = itin;
	pp.dur = get_itinerary_duration(itin);
	struct ItinArrivalDvCache saved_route_values = itin->arrival_dv_cache;
	int reuse_route_values = saved_route_values.route_values_valid &&
		fabs(saved_route_values.dep_periapsis - dep_periapsis) <= 1e-6;
	struct ItinStep *first_leg = itin;
	while(first_leg->prev->prev != NULL)
		first_leg = first_leg->prev;
	pp.dep_date = first_leg->prev->date;

	Vector3 arrival_excess = subtract_vec3(itin->v_arr, itin->v_body);
	if(cache_itinerary_arrival_dvs(itin, arr_periapsis)) {
		pp.dv_arr_cap = itin->arrival_dv_cache.dv_arr_cap;
		pp.dv_arr_circ = itin->arrival_dv_cache.dv_arr_circ;
	} else {
		double vinf = mag_vec3(arrival_excess);
		double arrival_radius = alt2radius(itin->body, arr_periapsis);
		pp.dv_arr_cap = dv_capture(itin->body, arrival_radius, vinf);
		pp.dv_arr_circ = dv_circ(itin->body, arrival_radius, vinf);
	}

	if(reuse_route_values) {
		pp.dv_dep = saved_route_values.dv_dep;
		pp.dv_dsm = saved_route_values.dv_dsm;
		pp.dv_flyby = saved_route_values.dv_flyby;
	} else {
		accumulate_route_dvs(itin, dep_periapsis, 0, &pp.dv_dep, &pp.dv_dsm, &pp.dv_flyby);
	}
	pp.arrival->arrival_dv_cache.dv_dep = pp.dv_dep;
	pp.arrival->arrival_dv_cache.dv_dsm = pp.dv_dsm;
	pp.arrival->arrival_dv_cache.dv_flyby = pp.dv_flyby;
	pp.arrival->arrival_dv_cache.dv_arr_cap = pp.dv_arr_cap;
	pp.arrival->arrival_dv_cache.dv_arr_circ = pp.dv_arr_circ;
	pp.arrival->arrival_dv_cache.dep_periapsis = dep_periapsis;
	pp.arrival->arrival_dv_cache.route_values_valid = 1;
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
	// Once the route holds the most flybys allowed, only the final leg to the target is searched from it.
	int only_target_legs = route_flyby_count(curr_step) >= get_max_flybys_per_route();
	for(int i = 0; i < seq_info->num_flyby_bodies && !itinerary_search_cancelled(NULL); i++) {
		if(get_thread_counter(3) > 0) break;
		Body *flyby_body = seq_info->flyby_bodies[i];
		if(only_target_legs && flyby_body != seq_info->arr_body) continue;
		if(flyby_body == curr_step->body) {
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
		// Legs that do not end the route only look ahead as far as the pair's encounter geometry needs.
		if(flyby_body != seq_info->arr_body)
			max_duration = fmin(max_duration, maximum_intermediate_leg_days(mag_vec3(curr_step->r), curr_step->body, flyby_body, seq_info->system));
		double min_duration = minimum_leg_days(seq_info->system);
		if(max_duration > min_duration)
			find_viable_flybys(curr_step, seq_info->system, flyby_body, min_duration*86400, max_duration*86400);
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

	int group_active = 0;
	int group_floor = 0;
	struct Body *group_body = NULL;

	for(int i = 0; i < init_num_nodes-num_of_end_nodes && !itinerary_search_cancelled(NULL); i++) {
		int idx = i - step_del;
		int remaining = init_num_nodes - num_of_end_nodes - i;
		struct Body *body = curr_step->next[idx]->body;
		// Children are grouped by body; each body group leaves part of the budget for the bodies after it.
		if(!group_active || body != group_body) {
			if(group_active) end_powered_seed_share(group_floor);
			int groups = 1;
			for(int k = 1; k < remaining; k++)
				if(curr_step->next[idx + k]->body != curr_step->next[idx + k - 1]->body) groups++;
			group_floor = begin_powered_seed_share(groups);
			group_active = 1;
			group_body = body;
		}
		has_valid_init = calc_next_itin_to_target_step(curr_step->next[idx], seq_info, jd_max_arr, max_total_duration, dv_filter);
		num_valid += has_valid_init;
		if(!has_valid_init) step_del++;
	}
	if(group_active) end_powered_seed_share(group_floor);

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
				struct ItinStep *end_node = calloc(1, sizeof(struct ItinStep));
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
	// Every end node shares the route up to curr_step, so its departure, DSM and flyby costs are accumulated once;
	// only the flyby at curr_step into the end node differs. Ancestor burns come from the per-node cache, since
	// this worker owns the tree and the same ancestors are walked again for every deeper end node.
	double prefix_dep = 0, prefix_dsm = 0, prefix_flyby = 0;
	int has_prefix = curr_step->prev != NULL;
	if(has_prefix && num_of_end_nodes > 0)
		accumulate_route_dvs(curr_step, dv_filter->dep_periapsis, 1, &prefix_dep, &prefix_dsm, &prefix_flyby);
	for(int i = curr_step->num_next_nodes-num_of_end_nodes; i < curr_step->num_next_nodes; i++) {
		struct ItinStep *next = curr_step->next[i];
		double dv_dep, dv_dsm, dv_flyby, dv_arr_cap, dv_arr_circ;
		if(has_prefix) {
			dv_dep = prefix_dep;
			dv_dsm = prefix_dsm;
			dv_flyby = prefix_flyby;
			if(curr_step->body != NULL) dv_flyby += route_flyby_burn(curr_step, next, 1);
		} else {
			accumulate_route_dvs(next, dv_filter->dep_periapsis, 1, &dv_dep, &dv_dsm, &dv_flyby);
		}
		if(cache_itinerary_arrival_dvs(next, dv_filter->arr_periapsis)) {
			dv_arr_cap = next->arrival_dv_cache.dv_arr_cap;
			dv_arr_circ = next->arrival_dv_cache.dv_arr_circ;
		} else {
			double vinf = mag_vec3(subtract_vec3(next->v_arr, next->v_body));
			double arrival_radius = alt2radius(next->body, dv_filter->arr_periapsis);
			dv_arr_cap = dv_capture(next->body, arrival_radius, vinf);
			dv_arr_circ = dv_circ(next->body, arrival_radius, vinf);
		}
		// Same route values create_porkchop_point would store, so later callers reuse them.
		next->arrival_dv_cache.dv_dep = dv_dep;
		next->arrival_dv_cache.dv_dsm = dv_dsm;
		next->arrival_dv_cache.dv_flyby = dv_flyby;
		next->arrival_dv_cache.dv_arr_cap = dv_arr_cap;
		next->arrival_dv_cache.dv_arr_circ = dv_arr_circ;
		next->arrival_dv_cache.dep_periapsis = dv_filter->dep_periapsis;
		next->arrival_dv_cache.route_values_valid = 1;
		double dv_sat = dv_dsm;
		if(dv_filter->last_transfer_type == 1) dv_sat += dv_arr_cap;
		if(dv_filter->last_transfer_type == 2) dv_sat += dv_arr_circ;
			if(dv_sat + dv_flyby > dv_filter->max_satdv ||
				dv_dep + dv_sat + dv_flyby > dv_filter->max_totdv) {
			count_search_rejection(dv_sat + dv_flyby > dv_filter->max_satdv ? REJECT_SATELLITE_DV : REJECT_TOTAL_DV);
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


double itinerary_transition_dv(struct ItinStep *step, struct ItinStep *next, double dep_periapsis) {
	if(step == NULL || next == NULL) return 0;
	if(step->body == NULL) {
		if(step->prev == NULL) return 0;
		return mag_vec3(subtract_vec3(next->v_dep, step->v_arr));
	}
	if(step->prev == NULL) {
		Vector3 vinf = subtract_vec3(next->v_dep, step->v_body);
		double departure_dv;
		if(starting_orbit_departure_dv(step->body, vinf, alt2radius(step->body, dep_periapsis), &departure_dv))
			return departure_dv;
		return dv_circ(step->body, alt2radius(step->body, dep_periapsis), mag_vec3(vinf));
	}
	PoweredFlybyResult flyby;
	double minimum_periapsis = flyby_minimum_periapsis(step->body);
	if(calculate_flyby_with_moons(step->v_arr, next->v_dep, step->v_body, step->body, minimum_periapsis, step->date, &flyby))
		return flyby.burn_delta_v;
	return HUGE_VAL;
}

double itinerary_step_dv(struct ItinStep *step, double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type) {
	if(step == NULL || (step->prev == NULL && step->next == NULL)) return 0;
	if(step->next != NULL && step->num_next_nodes > 0)
		return itinerary_transition_dv(step, step->next[0], dep_periapsis);
	if(step->body == NULL) return 0;
	if(last_transfer_type != TF_CAPTURE && last_transfer_type != TF_CIRC) return 0;
	if(cache_itinerary_arrival_dvs(step, arr_periapsis))
		return last_transfer_type == TF_CIRC ?
				step->arrival_dv_cache.dv_arr_circ : step->arrival_dv_cache.dv_arr_cap;
	Vector3 excess = subtract_vec3(step->v_arr, step->v_body);
	double vinf = mag_vec3(excess), radius = alt2radius(step->body, arr_periapsis);
	return last_transfer_type == TF_CIRC ? dv_circ(step->body, radius, vinf) :
			dv_capture(step->body, radius, vinf);
}
