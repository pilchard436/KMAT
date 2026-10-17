#include "orbit_calculator/transfer_calc.h"
#include "orbit_calculator/powered_flyby_optimizer.h"
#include "tools/file_io.h"
#include "orbitlib_fileio.h"
#include "orbitlib_transfer.h"
#include "tools/parking_orbit.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POWERED_TEST_DV_TOLERANCE 0.1
#define POWERED_TEST_MIN_LEG_DAYS 10.0
#define POWERED_TEST_GRID_RELATIVE_TOLERANCE 0.10

static int benchmark_resonant;
static double stored_total_dv(struct ItinStep *leaf, const Itin_Calc_Data *calc_data, double *dsm_dv);

// The same body twice in a row, ignoring maneuver nodes between the visits.
static int route_is_resonant(struct ItinStep *leaf) {
	Body *later = NULL;
	for(struct ItinStep *step = leaf; step != NULL; step = step->prev) {
		if(step->body == NULL) continue;
		if(later == step->body) return 1;
		later = step->body;
	}
	return 0;
}

static int check_speed_changing_seed_generation(CelestSystem *system, Body *departure,
		Body *flyby, Body *target, double departure_jd) {
	OSV dep = osv_from_elements(departure->orbit, departure_jd);
	struct ItinStep root = {.body = departure, .date = departure_jd};
	PoweredSeedSearchStats stats = {0};
	begin_powered_seed_search(&stats);
	int found_diverse_seeds = 0;
	for(int days = 120; days <= 600 && !found_diverse_seeds; days += 120) {
		OSV encounter = osv_from_elements(flyby->orbit, departure_jd + days);
		Lambert3 inbound = calc_lambert3(dep.r, encounter.r, days * 86400.0, system->cb);
		if(inbound.success == LAMBERT_FAIL_NAN || inbound.success == LAMBERT_FAIL_ECC ||
			inbound.success == LAMBERT_MAX_ITERATIONS) continue;
		struct ItinStep step = {.body = flyby, .date = departure_jd + days,
			.r = encounter.r, .v_arr = inbound.v1, .v_body = encounter.v, .prev = &root};
		find_viable_flybys(&step, system, target, 100 * 86400.0, 1000 * 86400.0);
		int speed_changing = 0;
		for(int i = 0; i < step.num_next_nodes; i++) {
			struct ItinStep *next = step.next[i];
			double difference = fabs(mag_vec3(subtract_vec3(step.v_arr, step.v_body)) -
				mag_vec3(subtract_vec3(next->v_dep, step.v_body)));
			if(difference > 1.0) speed_changing++;
			free_itinerary(next);
		}
		free(step.next);
		found_diverse_seeds = speed_changing >= 2;
	}
	begin_powered_seed_search(NULL);
	if(!found_diverse_seeds || stats.alternatives_added == 0) {
		fprintf(stderr, "Powered seed generation failed to retain distinct speed-changing dates\n");
		return 0;
	}
	printf("Powered seed generation retained distinct speed-changing dates before route optimization\n");
	return 1;
}

static int run_bounded_search(CelestSystem *system, Body *departure_body, Body *arrival_body,
        double min_departure_jd, double max_arrival_jd, int *num_nodes, int *num_itins) {
	Body *flyby_bodies[] = {departure_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = min_departure_jd;
	calc_data.jd_max_dep = min_departure_jd;
	calc_data.jd_max_arr = max_arrival_jd;
	calc_data.max_duration = max_arrival_jd - min_departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9,
		.max_depdv = 1e9,
		.max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET,
		.system = system,
		.dep_body = departure_body,
		.arr_body = arrival_body,
		.flyby_bodies = flyby_bodies,
		.num_flyby_bodies = 2
	};

	Itin_Calc_Results results = search_for_itineraries(calc_data);
	*num_nodes = results.num_nodes;
	*num_itins = results.num_itins;
	int valid = results.num_deps > 0 && results.num_nodes > 0 && results.num_itins > 0;
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return valid;
}

static double score_powered_arrival_route(struct ItinStep *arrival,
		const Itin_Calc_Data *calc_data, Body *flyby_body) {
	size_t num_steps = 0;
	int contains_flyby = 0;
	for(struct ItinStep *step = arrival; step != NULL; step = step->prev) {
		if(step->body == flyby_body && step->prev != NULL && step->next != NULL)
			contains_flyby = 1;
		num_steps++;
	}
	if(!contains_flyby || num_steps < 3) return DBL_MAX;
	Body **bodies = malloc(num_steps * sizeof(*bodies));
	double *times = malloc(num_steps * sizeof(*times));
	if(bodies == NULL || times == NULL) {
		free(bodies);
		free(times);
		return DBL_MAX;
	}
	struct ItinStep *step = arrival;
	for(size_t i = num_steps; i > 0; i--) {
		bodies[i - 1] = step->body;
		times[i - 1] = step->date;
		step = step->prev;
	}
	double objective = evaluate_powered_flyby_route_objective(calc_data, bodies, times, num_steps);
	free(bodies);
	free(times);
	return objective;
}

static int score_unpowered_grid_route(CelestSystem *system, Body **bodies,
		const double *times, const Itin_Calc_Data *calc_data, double *objective) {
	OSV states[3];
	for(int i = 0; i < 3; i++) {
		states[i] = system->prop_method == ORB_ELEMENTS ?
			osv_from_elements(bodies[i]->orbit, times[i]) :
			osv_from_ephem(bodies[i]->ephem, bodies[i]->num_ephems, times[i], system->cb);
		if(!isfinite(states[i].r.x) || !isfinite(states[i].r.y) || !isfinite(states[i].r.z) ||
			!isfinite(states[i].v.x) || !isfinite(states[i].v.y) || !isfinite(states[i].v.z)) return 0;
	}
	Lambert3 inbound = calc_lambert3(states[0].r, states[1].r,
		(times[1] - times[0]) * 86400, system->cb);
	Lambert3 outbound = calc_lambert3(states[1].r, states[2].r,
		(times[2] - times[1]) * 86400, system->cb);
	if(inbound.success == LAMBERT_FAIL_NAN || inbound.success == LAMBERT_FAIL_ECC ||
		inbound.success == LAMBERT_MAX_ITERATIONS || outbound.success == LAMBERT_FAIL_NAN ||
		outbound.success == LAMBERT_FAIL_ECC || outbound.success == LAMBERT_MAX_ITERATIONS ||
		!isfinite(inbound.v1.x) || !isfinite(inbound.v1.y) || !isfinite(inbound.v1.z) ||
		!isfinite(outbound.v0.x) || !isfinite(outbound.v0.y) || !isfinite(outbound.v0.z)) return 0;
	PoweredFlybyResult unpowered_flyby;
	double minimum_periapsis = bodies[1]->radius + bodies[1]->atmo_alt + 1e4;
	if(!calculate_powered_flyby(inbound.v1, outbound.v0, states[1].v,
		bodies[1], minimum_periapsis, &unpowered_flyby) ||
		unpowered_flyby.burn_delta_v > 1.0) return 0;
	*objective = evaluate_powered_flyby_route_objective(calc_data, bodies, times, 3);
	return isfinite(*objective) && *objective < DBL_MAX;
}

static int is_valid_lambert_solution(Lambert3 solution) {
	return solution.success != LAMBERT_FAIL_NAN && solution.success != LAMBERT_FAIL_ECC &&
		solution.success != LAMBERT_MAX_ITERATIONS &&
		isfinite(solution.v0.x) && isfinite(solution.v0.y) && isfinite(solution.v0.z) &&
		isfinite(solution.v1.x) && isfinite(solution.v1.y) && isfinite(solution.v1.z);
}

static int outgoing_excess_speed_difference(CelestSystem *system, Body *flyby_body,
		Body *arrival_body, OSV flyby_state, double flyby_jd, double arrival_jd,
		double incoming_excess_speed, Lambert3 *outbound, double *speed_difference) {
	OSV arrival_state = system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(arrival_body->orbit, arrival_jd) :
		osv_from_ephem(arrival_body->ephem, arrival_body->num_ephems, arrival_jd, system->cb);
	if(!isfinite(arrival_state.r.x) || !isfinite(arrival_state.r.y) || !isfinite(arrival_state.r.z) ||
		!isfinite(arrival_state.v.x) || !isfinite(arrival_state.v.y) || !isfinite(arrival_state.v.z)) return 0;
	*outbound = calc_lambert3(flyby_state.r, arrival_state.r,
		(arrival_jd - flyby_jd) * 86400, system->cb);
	if(!is_valid_lambert_solution(*outbound)) return 0;
	Vector3 outgoing_excess = subtract_vec3(outbound->v0, flyby_state.v);
	*speed_difference = mag_vec3(outgoing_excess) - incoming_excess_speed;
	if(!isfinite(*speed_difference)) return 0;
	return 1;
}

static void consider_unpowered_baseline(Body **bodies,
		OSV departure_state, OSV flyby_state, Lambert3 inbound, Lambert3 outbound,
		double departure_jd, double flyby_jd, double arrival_jd,
		const Itin_Calc_Data *calc_data, double *best_baseline) {
	PoweredFlybyResult flyby;
	double minimum_periapsis = bodies[1]->radius + bodies[1]->atmo_alt + 1e4;
	if(!calculate_powered_flyby(inbound.v1, outbound.v0, flyby_state.v,
		bodies[1], minimum_periapsis, &flyby) ||
		flyby.burn_delta_v > POWERED_TEST_DV_TOLERANCE) return;
	double times[] = {departure_jd, flyby_jd, arrival_jd};
	double objective = evaluate_powered_flyby_route_objective(calc_data, bodies, times, 3);
	if(isfinite(objective) && objective < DBL_MAX && objective < *best_baseline)
		*best_baseline = objective;
}

static double find_unpowered_grid_baseline(CelestSystem *system, Body *departure_body,
		Body *flyby_body, Body *arrival_body, double departure_jd,
		const Itin_Calc_Data *calc_data) {
	OSV departure_state = system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(departure_body->orbit, departure_jd) :
		osv_from_ephem(departure_body->ephem, departure_body->num_ephems, departure_jd, system->cb);
	if(!isfinite(departure_state.r.x) || !isfinite(departure_state.r.y) || !isfinite(departure_state.r.z) ||
		!isfinite(departure_state.v.x) || !isfinite(departure_state.v.y) || !isfinite(departure_state.v.z))
		return DBL_MAX;
	Body *bodies[] = {departure_body, flyby_body, arrival_body};
	double best_baseline = DBL_MAX;
	const double flyby_step_days = 30;
	const double arrival_step_days = 40;
	for(double flyby_offset = 30; flyby_offset < calc_data->max_duration - 2 * arrival_step_days;
		flyby_offset += flyby_step_days) {
		double flyby_jd = departure_jd + flyby_offset;
		OSV flyby_state = system->prop_method == ORB_ELEMENTS ?
			osv_from_elements(flyby_body->orbit, flyby_jd) :
			osv_from_ephem(flyby_body->ephem, flyby_body->num_ephems, flyby_jd, system->cb);
		if(!isfinite(flyby_state.r.x) || !isfinite(flyby_state.r.y) || !isfinite(flyby_state.r.z) ||
			!isfinite(flyby_state.v.x) || !isfinite(flyby_state.v.y) || !isfinite(flyby_state.v.z)) continue;
		Lambert3 inbound = calc_lambert3(departure_state.r, flyby_state.r,
			flyby_offset * 86400, system->cb);
		if(!is_valid_lambert_solution(inbound)) continue;
		Vector3 incoming_excess = subtract_vec3(inbound.v1, flyby_state.v);
		double incoming_speed = mag_vec3(incoming_excess);
		if(!(incoming_speed > 0)) continue;

		double previous_offset = flyby_offset + POWERED_TEST_MIN_LEG_DAYS;
		Lambert3 previous_outbound;
		double previous_difference;
		if(!outgoing_excess_speed_difference(system, flyby_body, arrival_body, flyby_state,
			flyby_jd, departure_jd + previous_offset, incoming_speed,
			&previous_outbound, &previous_difference)) continue;
		if(fabs(previous_difference) <= POWERED_TEST_DV_TOLERANCE)
			consider_unpowered_baseline(bodies, departure_state, flyby_state, inbound,
				previous_outbound, departure_jd, flyby_jd, departure_jd + previous_offset,
				calc_data, &best_baseline);

		for(double arrival_offset = previous_offset + arrival_step_days;
			arrival_offset <= calc_data->max_duration; arrival_offset += arrival_step_days) {
			Lambert3 current_outbound;
			double current_difference;
			if(!outgoing_excess_speed_difference(system, flyby_body, arrival_body, flyby_state,
				flyby_jd, departure_jd + arrival_offset, incoming_speed,
				&current_outbound, &current_difference)) continue;
			if(fabs(current_difference) <= POWERED_TEST_DV_TOLERANCE)
				consider_unpowered_baseline(bodies, departure_state, flyby_state, inbound,
					current_outbound, departure_jd, flyby_jd, departure_jd + arrival_offset,
					calc_data, &best_baseline);
			if((previous_difference < 0 && current_difference > 0) ||
				(previous_difference > 0 && current_difference < 0)) {
				double lower_offset = previous_offset;
				double upper_offset = arrival_offset;
				double lower_difference = previous_difference;
				Lambert3 root_outbound = current_outbound;
				for(int iteration = 0; iteration < 30; iteration++) {
					double middle_offset = (lower_offset + upper_offset) / 2;
					double middle_difference;
					if(!outgoing_excess_speed_difference(system, flyby_body, arrival_body,
						flyby_state, flyby_jd, departure_jd + middle_offset, incoming_speed,
						&root_outbound, &middle_difference)) break;
					if(fabs(middle_difference) <= POWERED_TEST_DV_TOLERANCE) {
						lower_offset = middle_offset;
						upper_offset = middle_offset;
						break;
					}
					if((lower_difference < 0 && middle_difference < 0) ||
						(lower_difference > 0 && middle_difference > 0)) {
						lower_offset = middle_offset;
						lower_difference = middle_difference;
					} else upper_offset = middle_offset;
				}
				double root_offset = (lower_offset + upper_offset) / 2;
				double root_difference;
				if(outgoing_excess_speed_difference(system, flyby_body, arrival_body,
					flyby_state, flyby_jd, departure_jd + root_offset, incoming_speed,
					&root_outbound, &root_difference) &&
					fabs(root_difference) <= POWERED_TEST_DV_TOLERANCE)
					consider_unpowered_baseline(bodies, departure_state, flyby_state,
						inbound, root_outbound, departure_jd, flyby_jd,
						departure_jd + root_offset, calc_data, &best_baseline);
			}
		previous_offset = arrival_offset;
		previous_difference = current_difference;
		previous_outbound = current_outbound;
		}
	}
	return best_baseline;
}

// Compares the objective implied by each stored node with one recomputed from encounter dates alone.
static void measure_stored_objective_mismatch(const Itin_Calc_Results *results,
		const Itin_Calc_Data *calc_data, Body *flyby_body, double *worst_mismatch,
		int *unscorable_routes, int *checked_routes) {
	*worst_mismatch = 0;
	*unscorable_routes = 0;
	*checked_routes = 0;
	for(int i = 0; i < results->num_deps; i++) {
		int count = get_number_of_itineraries(results->departures[i]);
		struct ItinStep **leaves = malloc((size_t)count * sizeof(*leaves));
		if(leaves == NULL) { *worst_mismatch = DBL_MAX; return; }
		int stored = 0;
		store_itineraries_in_array(results->departures[i], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			struct PorkchopPoint point = create_porkchop_point(leaves[j],
				calc_data->dv_filter.dep_periapsis, calc_data->dv_filter.arr_periapsis);
			double stored_objective = point.dv_dep + point.dv_dsm + point.dv_flyby;
			double recomputed = score_powered_arrival_route(leaves[j], calc_data, flyby_body);
			(*checked_routes)++;
			if(!isfinite(stored_objective) || !isfinite(recomputed) || recomputed >= DBL_MAX) {
				(*unscorable_routes)++;
				continue;
			}
			*worst_mismatch = fmax(*worst_mismatch, fabs(stored_objective - recomputed));
		}
		free(leaves);
	}
}

// A configured parking orbit must govern the departure delta-v that the search stores and filters on.
static int check_parking_orbit_search(CelestSystem *system, Body *departure_body, Body *flyby_body,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {flyby_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 2
	};

	// A tilted circular orbit can never beat the free-plane burn at the same radius, and must lose on
	// routes whose asymptote is off its line of nodes.
	double radius = alt2radius(departure_body, calc_data.dv_filter.dep_periapsis);
	ParkingOrbit tilted = {.semi_major_axis = radius, .inclination = 1.4, .raan = 0.5};
	int valid = parking_orbit_is_valid(&tilted, departure_body);
	parking_orbit_set_active(&tilted, departure_body);
	Itin_Calc_Results run = search_for_itineraries(calc_data);

	double worst_mismatch = 0, lowest_margin = DBL_MAX, largest_penalty = 0;
	int checked = 0, penalized = 0;
	for(int i = 0; i < run.num_deps && valid; i++) {
		int count = get_number_of_itineraries(run.departures[i]);
		struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
		if(leaves == NULL) { valid = 0; break; }
		int stored = 0;
		store_itineraries_in_array(run.departures[i], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			struct PorkchopPoint point = create_porkchop_point(leaves[j],
				calc_data.dv_filter.dep_periapsis, calc_data.dv_filter.arr_periapsis);
			if(!isfinite(point.dv_dep)) continue;
			// Independent recomputation from the first leg's departure velocity.
			struct ItinStep *first = leaves[j];
			while(first->prev != NULL && first->prev->prev != NULL) first = first->prev;
			Vector3 v_inf = subtract_vec3(first->v_dep, first->prev->v_body);
			double direct;
			if(!parking_orbit_best_departure_burn(&tilted, departure_body, v_inf, &direct, NULL)) continue;
			double free_plane = dv_circ(departure_body, radius, mag_vec3(v_inf));
			worst_mismatch = fmax(worst_mismatch, fabs(direct - point.dv_dep));
			lowest_margin = fmin(lowest_margin, point.dv_dep - free_plane);
			largest_penalty = fmax(largest_penalty, point.dv_dep - free_plane);
			penalized += point.dv_dep > free_plane + 10;
			checked++;
		}
		free(leaves);
	}
	parking_orbit_set_active(NULL, NULL);
	for(int i = 0; i < run.num_deps; i++) free_itinerary(run.departures[i]);
	free(run.departures);
	if(!valid || checked == 0 || worst_mismatch > 1e-6 || lowest_margin < -1e-3 || penalized == 0) {
		fprintf(stderr, "Parking orbit search check failed: %d routes checked, mismatch %.9f m/s, lowest margin over free plane %.6f m/s, %d routes penalized\n",
			checked, worst_mismatch, lowest_margin, penalized);
		return 0;
	}
	printf("Parking orbit search check passed: stored departure dV matches the orbit on %d routes (worst mismatch %.9f m/s); never below the free-plane burn, and above it by up to %.1f m/s on %d routes\n",
		checked, worst_mismatch, largest_penalty, penalized);
	return 1;
}

static int run_powered_flyby_objective_regression(CelestSystem *system,
		Body *departure_body, Body *flyby_body, Body *arrival_body,
		double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {flyby_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9,
		.max_depdv = 1e9,
		.max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET,
		.system = system,
		.dep_body = departure_body,
		.arr_body = arrival_body,
		.flyby_bodies = flyby_bodies,
		.num_flyby_bodies = 2
	};
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	if(results.num_deps <= 0 || results.num_itins <= 0) goto cleanup;
	if(results.powered_seed_stats.seeds_added > POWERED_SEEDS_PER_DEPARTURE ||
		results.powered_seed_stats.alternatives_added > POWERED_EXTRA_SEEDS_PER_DEPARTURE ||
		results.powered_seed_stats.searches_budget_skipped == 0) {
		fprintf(stderr, "Powered search failed its per-departure budget/exhaustion checks\n");
		goto cleanup;
	}

	struct ItinStep **arrivals = malloc((size_t)results.num_itins * sizeof(*arrivals));
	if(arrivals == NULL) goto cleanup;
	int arrival_count = 0;
	for(int i = 0; i < results.num_deps; i++)
		store_itineraries_in_array(results.departures[i], arrivals, &arrival_count);
	int has_valid_powered_route = 0;
	int has_speed_changing_powered_route = 0;
	for(int i = 0; i < arrival_count && !has_speed_changing_powered_route; i++) {
		struct ItinStep *outbound = arrivals[i];
		for(struct ItinStep *step = arrivals[i]->prev; step != NULL && step->prev != NULL;
			outbound = step, step = step->prev) {
			if(step->body != flyby_body) continue;
			PoweredFlybyResult flyby_result;
			double minimum_periapsis = flyby_body->radius + flyby_body->atmo_alt + 1e4;
			if(calculate_powered_flyby(step->v_arr, outbound->v_dep, step->v_body,
				flyby_body, minimum_periapsis, &flyby_result) &&
				isfinite(flyby_result.burn_delta_v)) {
				has_valid_powered_route = 1;
				Vector3 incoming_excess = subtract_vec3(step->v_arr, step->v_body);
				Vector3 outgoing_excess = subtract_vec3(outbound->v_dep, step->v_body);
				double excess_speed_change = fabs(mag_vec3(outgoing_excess) - mag_vec3(incoming_excess));
				if(excess_speed_change > 1.0 && flyby_result.burn_delta_v > POWERED_TEST_DV_TOLERANCE)
					has_speed_changing_powered_route = 1;
			}
			if(has_speed_changing_powered_route) break;
		}
	}
	free(arrivals);
	if(!has_valid_powered_route || !has_speed_changing_powered_route) {
		fprintf(stderr, "Powered search did not return a valid flyby with |delta V-infinity| > 1 m/s\n");
		goto cleanup;
	}

	double best_search_objective = DBL_MAX;
	int optimized_itinerary_count = 0;
	for(int i = 0; i < results.num_deps; i++)
		optimized_itinerary_count += get_number_of_itineraries(results.departures[i]);
	struct ItinStep **optimized_arrivals = malloc((size_t)optimized_itinerary_count * sizeof(*optimized_arrivals));
	if(optimized_arrivals != NULL) {
		int optimized_arrival_count = 0;
		for(int i = 0; i < results.num_deps; i++)
			store_itineraries_in_array(results.departures[i], optimized_arrivals, &optimized_arrival_count);
		for(int i = 0; i < optimized_arrival_count; i++) {
			double objective = score_powered_arrival_route(optimized_arrivals[i], &calc_data, flyby_body);
			if(objective < best_search_objective) best_search_objective = objective;
		}
		free(optimized_arrivals);
	}
	{
		double worst_mismatch;
		int unscorable, checked;
		measure_stored_objective_mismatch(&results, &calc_data, flyby_body, &worst_mismatch, &unscorable, &checked);
		// Stale node velocities (the old suffix-only write-back) show up as a large mismatch here.
		if(checked == 0 || checked - unscorable == 0 || worst_mismatch > POWERED_TEST_DV_TOLERANCE) {
			fprintf(stderr, "Stored itinerary data disagrees with the date-based objective: %d checked, %d unscorable, worst mismatch %.6f m/s\n",
				checked, unscorable, worst_mismatch);
			goto cleanup;
		}
		if(results.powered_initial_rejections == 0 || results.powered_repairs_attempted == 0 ||
			results.powered_repairs_recovered == 0 || results.powered_repairs_competing == 0 ||
			results.powered_repairs_recovered > results.powered_repairs_attempted ||
			(unsigned long long)results.powered_repairs_attempted > results.powered_initial_rejections ||
			results.powered_repairs_competing > results.powered_repairs_recovered) {
			fprintf(stderr, "Rejected-route repair failed its recovery checks: %llu rejections, %d attempted, %d recovered, %d competing\n",
				results.powered_initial_rejections, results.powered_repairs_attempted,
				results.powered_repairs_recovered, results.powered_repairs_competing);
			goto cleanup;
		}
		printf("Stored itinerary data matches the date-based objective (%d routes, worst mismatch %.6f m/s); repaired %d of %d rejected routes\n",
			checked - unscorable, worst_mismatch, results.powered_repairs_recovered,
			results.powered_repairs_attempted);
	}
	double grid_flyby_offsets[] = {120, 240, 360, 480, 600};
	double grid_arrival_offsets[] = {700, 850, 1000, 1150, 1300, 1400};
	Body *reference_bodies[] = {departure_body, flyby_body, arrival_body};
	double best_grid_objective = DBL_MAX;
	double best_unpowered_baseline = results.best_unpowered_seed_total_dv;
	for(size_t flyby_index = 0; flyby_index <
		sizeof(grid_flyby_offsets) / sizeof(grid_flyby_offsets[0]); flyby_index++) {
		for(size_t arrival_index = 0; arrival_index <
			sizeof(grid_arrival_offsets) / sizeof(grid_arrival_offsets[0]); arrival_index++) {
			if(grid_arrival_offsets[arrival_index] <= grid_flyby_offsets[flyby_index] + 10 ||
				grid_arrival_offsets[arrival_index] > calc_data.max_duration) continue;
			double reference_times[] = {departure_jd,
				departure_jd + grid_flyby_offsets[flyby_index],
				departure_jd + grid_arrival_offsets[arrival_index]};
			double objective = evaluate_powered_flyby_route_objective(&calc_data,
				reference_bodies, reference_times, 3);
			if(objective < best_grid_objective) best_grid_objective = objective;
			double unpowered_objective;
			if(score_unpowered_grid_route(system, reference_bodies, reference_times,
				&calc_data, &unpowered_objective) && unpowered_objective < best_unpowered_baseline)
				best_unpowered_baseline = unpowered_objective;
		}
	}
	double root_found_unpowered_baseline = find_unpowered_grid_baseline(system,
		departure_body, flyby_body, arrival_body, departure_jd, &calc_data);
	if(root_found_unpowered_baseline < best_unpowered_baseline)
		best_unpowered_baseline = root_found_unpowered_baseline;
	int objective_did_not_increase = results.powered_routes_valid > 0 &&
		results.powered_routes_corrected > 0 &&
		isfinite(results.best_powered_initial_total_dv) && results.best_powered_initial_total_dv < DBL_MAX &&
		isfinite(results.best_powered_refined_total_dv) && results.best_powered_refined_total_dv < DBL_MAX &&
		results.best_powered_refined_total_dv <= results.best_powered_initial_total_dv + POWERED_TEST_DV_TOLERANCE &&
		isfinite(best_search_objective) && best_search_objective < DBL_MAX &&
		isfinite(best_grid_objective) && best_grid_objective < DBL_MAX &&
		best_search_objective <= best_grid_objective *
			(1 + POWERED_TEST_GRID_RELATIVE_TOLERANCE) + POWERED_TEST_DV_TOLERANCE &&
		isfinite(best_unpowered_baseline) && best_unpowered_baseline < DBL_MAX &&
		best_search_objective <= best_unpowered_baseline + POWERED_TEST_DV_TOLERANCE;
	if(!objective_did_not_increase)
		fprintf(stderr, "Powered objective regression failed: %d valid, %d corrected, refine %.3f -> %.3f m/s, search %.3f, grid %.3f, unpowered %.3f m/s\n",
			results.powered_routes_valid, results.powered_routes_corrected,
			results.best_powered_initial_total_dv, results.best_powered_refined_total_dv,
			best_search_objective, best_grid_objective,
			best_unpowered_baseline);
	else
		printf("Powered-flyby objective regression passed: %d valid, %d SOI-corrected, refine %.3f -> %.3f m/s, best search %.3f vs grid %.3f and unpowered %.3f m/s\n",
			results.powered_routes_valid, results.powered_routes_corrected,
			results.best_powered_initial_total_dv, results.best_powered_refined_total_dv,
			best_search_objective, best_grid_objective,
			best_unpowered_baseline);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return objective_did_not_increase;

cleanup:
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return 0;
}

static int run_saved_mission_benchmark(CelestSystem *system, const char *label,
		const char *departure_name, const char *arrival_name,
		const char **flyby_names, int num_flybys, double jd_min_dep, double jd_max_dep,
		double jd_max_arr, double max_duration, double max_total_dv,
		double max_departure_dv, double max_satellite_dv,
		double departure_periapsis, double arrival_periapsis) {
	Body *departure_body = get_body_by_name((char *)departure_name, system);
	Body *arrival_body = get_body_by_name((char *)arrival_name, system);
	Body **flyby_bodies = malloc((size_t)num_flybys * sizeof(*flyby_bodies));
	if(departure_body == NULL || arrival_body == NULL || flyby_bodies == NULL) {
		fprintf(stderr, "%s benchmark: missing body or allocation failure\n", label);
		free(flyby_bodies);
		return 0;
	}
	for(int i = 0; i < num_flybys; i++) {
		flyby_bodies[i] = get_body_by_name((char *)flyby_names[i], system);
		if(flyby_bodies[i] == NULL) {
			fprintf(stderr, "%s benchmark: missing flyby body %s\n", label, flyby_names[i]);
			free(flyby_bodies);
			return 0;
		}
	}

	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = jd_min_dep;
	calc_data.jd_max_dep = jd_max_dep;
	calc_data.jd_max_arr = jd_max_arr;
	calc_data.max_duration = max_duration;
	calc_data.step_dep_date = 1;
	calc_data.num_deps_per_date = 500;
	calc_data.max_num_waiting_orbits = 0;
	calc_data.calc_acc = (Calc_Acc){.lambert = 1, .fb_finder = 1};
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = max_total_dv,
		.max_depdv = max_departure_dv,
		.max_satdv = max_satellite_dv,
		.dep_periapsis = departure_periapsis,
		.arr_periapsis = arrival_periapsis,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET,
		.system = system,
		.dep_body = departure_body,
		.arr_body = arrival_body,
		.flyby_bodies = flyby_bodies,
		.num_flyby_bodies = num_flybys,
		.allow_resonant = benchmark_resonant
	};
	printf("\n=== %s saved-settings benchmark ===\n", label);
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	printf("%s benchmark result: %d departure roots, %d nodes, %d itineraries\n",
		label, results.num_deps, results.num_nodes, results.num_itins);
	if(benchmark_resonant) {
		int resonant_routes = 0;
		double best_resonant = DBL_MAX, best_overall = DBL_MAX;
		for(int i = 0; i < results.num_deps; i++) {
			int count = get_number_of_itineraries(results.departures[i]);
			struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
			if(leaves == NULL) continue;
			int stored = 0;
			store_itineraries_in_array(results.departures[i], leaves, &stored);
			for(int j = 0; j < stored; j++) {
				double total = stored_total_dv(leaves[j], &calc_data, NULL);
				int resonant = route_is_resonant(leaves[j]);
				if(!isfinite(total)) continue;
				if(total < best_overall) best_overall = total;
				if(resonant) { resonant_routes++; if(total < best_resonant) best_resonant = total; }
			}
			free(leaves);
		}
		printf("%s resonant routes: %d, best resonant %.3f m/s, best overall %.3f m/s\n",
			label, resonant_routes, best_resonant, best_overall);
	}
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	free(flyby_bodies);
	return results.num_deps > 0 && results.num_itins > 0;
}

static int run_saved_mission_benchmarks(void) {
	char system_path[] = "../Celestial_Systems/solar_system.cfg";
	CelestSystem *system = load_celestial_system_from_cfg_file(system_path);
	if(system == NULL) {
		fprintf(stderr, "Failed to load benchmark system: %s\n", system_path);
		return EXIT_FAILURE;
	}
	const char *jupiter_flybys[] = {"EARTH", "MARS", "JUPITER"};
	const char *saturn_flybys[] = {"EARTH", "MARS", "JUPITER", "SATURN"};
	int jupiter_valid = run_saved_mission_benchmark(system, "Jupiter",
		"EARTH", "JUPITER", jupiter_flybys, 3,
		2461327.50, 2461692.50, 2463325.50, 1998.0,
		6939, 6939, 6939, 190e3, 1600e3);
	int saturn_valid = run_saved_mission_benchmark(system, "Saturn",
		"EARTH", "SATURN", saturn_flybys, 4,
		2461326.50, 2461691.50, 2466098.50, 4407.0,
		7282, 7282, 7282, 190e3, 2050e3);
	free_celestial_system(system);
	return jupiter_valid && saturn_valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_full_profile(void) {
	char system_path[] = "../Celestial_Systems/solar_system_ephem.cfg";
	CelestSystem *system = load_celestial_system_from_cfg_file(system_path);
	if(system == NULL) {
		fprintf(stderr, "Failed to load profile celestial system: %s\n", system_path);
		return EXIT_FAILURE;
	}
	Body *departure_body = get_body_by_name("Earth", system);
	Body *arrival_body = get_body_by_name("Saturn", system);
	const char *candidate_names[] = {"Earth", "Mars", "Vesta", "Ceres", "Jupiter", "Saturn"};
	Body *flyby_bodies[sizeof(candidate_names) / sizeof(candidate_names[0])];
	for(size_t i = 0; i < sizeof(candidate_names) / sizeof(candidate_names[0]); i++) {
		flyby_bodies[i] = get_body_by_name((char *)candidate_names[i], system);
		if(flyby_bodies[i] == NULL) {
			fprintf(stderr, "Profile system is missing %s\n", candidate_names[i]);
			free_celestial_system(system);
			return EXIT_FAILURE;
		}
	}
	if(departure_body == NULL || arrival_body == NULL) {
		fprintf(stderr, "Profile system is missing Earth or Saturn\n");
		free_celestial_system(system);
		return EXIT_FAILURE;
	}

	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = 2461325.50;
	calc_data.jd_max_dep = 2461690.50;
	calc_data.jd_max_arr = 2466097.50;
	calc_data.max_duration = 4407.00;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 7282,
		.max_depdv = 7282,
		.max_satdv = 7282,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET,
		.system = system,
		.dep_body = departure_body,
		.arr_body = arrival_body,
		.flyby_bodies = flyby_bodies,
		.num_flyby_bodies = (int)(sizeof(candidate_names) / sizeof(candidate_names[0]))
	};
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	int valid = results.num_itins > 0 && results.powered_routes_corrected > 0 &&
		results.powered_itineraries_pruned * 100 <= results.num_itins &&
		results.powered_repairs_attempted > 0 && results.powered_repairs_attempted <= POWERED_REPAIR_SLOTS &&
		results.powered_repairs_recovered <= results.powered_repairs_attempted &&
		results.powered_challenges_refined > 0 && results.powered_challenges_refined <= POWERED_CHALLENGE_SLOTS &&
		isfinite(results.best_powered_refined_total_dv) &&
		results.best_powered_refined_total_dv <= calc_data.dv_filter.max_totdv;
	if(!valid)
		fprintf(stderr, "Full-profile summary: itins %d corrected %d pruned %d repairs %d/%d challenges %d refined best %.1f\n",
			results.num_itins, results.powered_routes_corrected, results.powered_itineraries_pruned,
			results.powered_repairs_attempted, results.powered_repairs_recovered,
			results.powered_challenges_refined, results.best_powered_refined_total_dv);
	for(int i = 0; i < results.num_deps; i++) {
		int count = get_number_of_itineraries(results.departures[i]);
		struct ItinStep **leaves = malloc((size_t)count * sizeof(*leaves));
		if(leaves == NULL) { valid = 0; continue; }
		int stored = 0;
		store_itineraries_in_array(results.departures[i], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			struct PorkchopPoint point = create_porkchop_point(leaves[j],
				calc_data.dv_filter.dep_periapsis, calc_data.dv_filter.arr_periapsis);
			double total = point.dv_dep + point.dv_flyby + point.dv_dsm;
			if(leaves[j]->body != arrival_body || !isfinite(total) ||
				total > calc_data.dv_filter.max_totdv + POWERED_TEST_DV_TOLERANCE ||
				point.dv_dep > calc_data.dv_filter.max_depdv + POWERED_TEST_DV_TOLERANCE ||
				point.dv_dsm + point.dv_flyby > calc_data.dv_filter.max_satdv + POWERED_TEST_DV_TOLERANCE ||
				point.dep_date < calc_data.jd_min_dep || point.dep_date > calc_data.jd_max_dep ||
				leaves[j]->date > calc_data.jd_max_arr || point.dur > calc_data.max_duration) {
				if(valid) fprintf(stderr, "Full-profile route violates limits: total %.1f dep %.1f sat %.1f dur %.1f\n",
					total, point.dv_dep, point.dv_dsm + point.dv_flyby, point.dur);
				valid = 0;
			}
		}
		free(leaves);
	}
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	free_celestial_system(system);
	if(!valid) {
		fprintf(stderr, "Full-profile search failed route validity/quality checks\n");
		return EXIT_FAILURE;
	}
	printf("UI-free full-profile search passed: %d nodes, %d itineraries\n",
		results.num_nodes, results.num_itins);
	return EXIT_SUCCESS;
}

// Total mission delta-v implied by the stored node velocities, including deep-space maneuvers.
static double stored_total_dv(struct ItinStep *leaf, const Itin_Calc_Data *calc_data, double *dsm_dv) {
	struct PorkchopPoint point = create_porkchop_point(leaf, calc_data->dv_filter.dep_periapsis,
		calc_data->dv_filter.arr_periapsis);
	if(dsm_dv != NULL) *dsm_dv = point.dv_dsm;
	return point.dv_dep + point.dv_dsm + point.dv_flyby;
}

// Cost of the same route with its maneuvers removed, recomputed from encounter dates alone.
static double plain_cost_without_maneuvers(struct ItinStep *leaf, const Itin_Calc_Data *calc_data) {
	size_t count = 0;
	for(struct ItinStep *step = leaf; step != NULL; step = step->prev)
		if(step->body != NULL) count++;
	Body **bodies = malloc(count * sizeof(*bodies));
	double *times = malloc(count * sizeof(*times));
	if(bodies == NULL || times == NULL) { free(bodies); free(times); return DBL_MAX; }
	size_t index = count;
	for(struct ItinStep *step = leaf; step != NULL; step = step->prev) {
		if(step->body == NULL) continue;
		bodies[--index] = step->body;
		times[index] = step->date;
	}
	double cost = evaluate_powered_flyby_route_objective(calc_data, bodies, times, count);
	free(bodies);
	free(times);
	return cost;
}

// Counts routes, maneuver nodes, and the best total delta-v over a set of departure trees.
static void summarize_itineraries(struct ItinStep **departures, int num_departures,
		const Itin_Calc_Data *calc_data, int *routes, int *maneuvers, double *best_total) {
	for(int i = 0; i < num_departures; i++) {
		int count = get_number_of_itineraries(departures[i]);
		struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
		if(leaves == NULL) return;
		int stored = 0;
		store_itineraries_in_array(departures[i], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			(*routes)++;
			for(struct ItinStep *step = leaves[j]; step != NULL; step = step->prev)
				if(step->body == NULL) (*maneuvers)++;
			double total = stored_total_dv(leaves[j], calc_data, NULL);
			if(isfinite(total) && total < *best_total) *best_total = total;
		}
		free(leaves);
	}
}

// Operations the transfer planner and porkchop analyzer perform on a stored route, run on real results.
#define TEXT_GUARD_SIZE 16

static int itinerary_structure_problem(struct ItinStep *first) {
	if(first == NULL || first->prev != NULL || first->body == NULL) return 1;
	for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL) {
		if(s->next == NULL ? s->num_next_nodes != 0 : s->num_next_nodes != 1) return 2;
		if(s->next != NULL && s->next[0]->prev != s) return 3;
		if(s->next != NULL && !(s->next[0]->date > s->date)) return 4;
		if(s->body == NULL && (s->prev == NULL || s->next == NULL)) return 5;
		if(!isfinite(s->r.x + s->r.y + s->r.z + s->v_dep.x + s->v_dep.y + s->v_dep.z +
			s->v_arr.x + s->v_arr.y + s->v_arr.z)) return 6;
	}
	return 0;
}

// Calls every text builder on every step with the planner's buffer sizes; returns nonzero on an overflow.
static int guard_broken(const char *buffer, size_t size) {
	for(size_t i = size; i < size + TEXT_GUARD_SIZE; i++)
		if(buffer[i] != 0x5A) return 1;
	return 0;
}

static int itinerary_text_problem(struct ItinStep *first, double dep_periapsis, double arr_periapsis) {
	static char labels[ITIN_STEP_TEXT_SIZE + TEXT_GUARD_SIZE], values[ITIN_STEP_TEXT_SIZE + TEXT_GUARD_SIZE], overview[10000 + TEXT_GUARD_SIZE];
	for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL) {
		memset(labels, 0x5A, sizeof(labels));
		memset(values, 0x5A, sizeof(values));
		labels[0] = values[0] = 0;
		itinerary_step_parameters_to_string(labels, values, DATE_ISO, dep_periapsis, arr_periapsis, s);
		if(guard_broken(labels, ITIN_STEP_TEXT_SIZE) || guard_broken(values, ITIN_STEP_TEXT_SIZE)) return 1;
		// A maneuver is described by its three burn components.
		if(s->body == NULL && (strstr(labels, "Prograde") == NULL || strstr(labels, "Radial") == NULL || strstr(labels, "Normal") == NULL)) return 4;
	}
	for(int detailed = 0; detailed < 2; detailed++) {
		memset(overview, 0x5A, sizeof(overview));
		overview[0] = 0;
		if(detailed) itinerary_detailed_overview_to_string(first, DATE_ISO, dep_periapsis, arr_periapsis, overview);
		else itinerary_short_overview_to_string(first, DATE_ISO, dep_periapsis, arr_periapsis, overview);
		if(guard_broken(overview, 10000) || strstr(overview, "nan") != NULL) return 2 + detailed;
		if(first->next != NULL && strstr(overview, "Total dv") == NULL) return 6;
		if(strstr(overview, "Deep-Space Maneuver") != NULL && (strstr(overview, "tangential") == NULL || strstr(overview, "normal") == NULL)) return 5;
	}
	return 0;
}

// Step delta-vs shown by the planner must add up to the totals of the itinerary overview.
static double itinerary_dv_mismatch(struct ItinStep *first, double dep_periapsis, double arr_periapsis) {
	struct PorkchopPoint pp = create_porkchop_point(get_last(first), dep_periapsis, arr_periapsis);
	double worst = 0;
	for(int type = TF_FLYBY; type <= TF_CIRC; type++) {
		double expected = pp.dv_dep + pp.dv_dsm + pp.dv_flyby +
			(type == TF_CAPTURE ? pp.dv_arr_cap : type == TF_CIRC ? pp.dv_arr_circ : 0);
		if(!isfinite(expected)) continue;
		double sum = 0;
		for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL)
			sum += itinerary_step_dv(s, dep_periapsis, arr_periapsis, type);
		worst = fmax(worst, fabs(sum - expected));
	}
	return worst;
}

static int itinerary_recompute_problem(struct ItinStep *first, CelestSystem *system) {
	update_itin_body_osvs(first, system);
	calc_itin_v_vectors_from_dates_and_r(first, system);
	return itinerary_structure_problem(first) != 0;
}

static int check_itinerary_operations(struct ItinStep **departures, int num_departures, CelestSystem *system) {
	int problems = 0, routes = 0, maneuver_routes = 0, reported = 0, resolve_attempts = 0, resolve_fallbacks = 0, maneuver_optimizations = 0, sweep_routes = 0, flagged_routes = 0;
#define OPERATION_PROBLEM(...) do { problems++; if(reported++ < 12) fprintf(stderr, __VA_ARGS__); } while(0)
	int maneuver_budget = 30, plain_budget = 30;
	for(int d = 0; d < num_departures; d++) {
		int count = get_number_of_itineraries(departures[d]);
		struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
		if(leaves == NULL) return 0;
		int stored = 0;
		store_itineraries_in_array(departures[d], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			int has_maneuver = 0, nodes = 0;
			for(struct ItinStep *s = leaves[j]; s != NULL; s = s->prev) { has_maneuver |= s->body == NULL; nodes++; }
			if(has_maneuver ? maneuver_budget <= 0 : (plain_budget <= 0 || j % 7 != 0)) continue;
			if(has_maneuver) maneuver_budget--; else plain_budget--;
			routes++; maneuver_routes += has_maneuver;
			flagged_routes += count_flybys_needing_burn(leaves[j], NULL) > 0;

			struct ItinStep *copy = get_first(create_itin_copy_from_arrival(leaves[j]));
			double dep = copy->body->atmo_alt + 50e3, arr = get_last(copy)->body->atmo_alt + 50e3;
			int problem = itinerary_structure_problem(copy);
			if(problem) OPERATION_PROBLEM("route %d.%d: structure problem %d\n", d, j, problem);
			if(!is_valid_itinerary(get_last(copy))) OPERATION_PROBLEM("route %d.%d: is_valid_itinerary rejects a stored route\n", d, j);
			if((problem = itinerary_text_problem(copy, dep, arr)) != 0) OPERATION_PROBLEM("route %d.%d: text builders problem %d (1 step text overflow, 2 short or 3 detailed overview overflow or nan)\n", d, j, problem);
			double mismatch = itinerary_dv_mismatch(copy, dep, arr);
			if(mismatch > 1e-3) OPERATION_PROBLEM("route %d.%d: step dv do not add up to the overview (%.4f m/s off)\n", d, j, mismatch);
			if(itinerary_recompute_problem(copy, system)) OPERATION_PROBLEM("route %d.%d: recompute leaves non-finite nodes\n", d, j);
			free_itinerary(copy);

			// Re-solving at unchanged dates, as the planner does after an edit, must reproduce the stored delta-v.
			copy = get_first(create_itin_copy_from_arrival(leaves[j]));
			int encounters = 0;
			for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) encounters += s->body != NULL;
			if(encounters >= 3) {
				double before = 0, after = 0;
				for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) before += itinerary_step_dv(s, dep, arr, TF_FLYBY);
				resolve_attempts++;
				if(!resolve_itinerary_with_powered_flybys(system, copy, dep, arr, TF_FLYBY)) {
					resolve_fallbacks++;
					double shortest = DBL_MAX;
					for(struct ItinStep *s = copy; s->next != NULL; s = s->next[0]) shortest = fmin(shortest, s->next[0]->date - s->date);
					printf("  re-solve fell back on route %d.%d (maneuver %d, %d encounters, shortest leg %.1f days)\n", d, j, has_maneuver, encounters, shortest);
				}
				else {
					for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) after += itinerary_step_dv(s, dep, arr, TF_FLYBY);
					// A maneuver node is pinned by its stored position and date, which amplifies the solver's convergence noise.
					double tolerance = has_maneuver ? 0.05 * before : 50.0;
					if(fabs(after - before) > tolerance) OPERATION_PROBLEM("route %d.%d: re-solve changed total dv from %.1f to %.1f m/s\n", d, j, before, after);
				}
			}
			free_itinerary(copy);

			// Re-optimizing maneuvers must leave a valid route that is not worse than before.
			if(encounters >= 3 && maneuver_optimizations < 6) {
				copy = get_first(create_itin_copy_from_arrival(leaves[j]));
				double before = 0, after = 0;
				for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) before += itinerary_step_dv(s, dep, arr, TF_FLYBY);
				if(optimize_itinerary_maneuvers(system, copy, dep, arr, TF_FLYBY)) {
					maneuver_optimizations++;
					problem = itinerary_structure_problem(copy);
					if(problem) OPERATION_PROBLEM("route %d.%d: structure problem %d after optimizing maneuvers\n", d, j, problem);
					for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) after += itinerary_step_dv(s, dep, arr, TF_FLYBY);
					if(after > before * 1.05 + 50.0) OPERATION_PROBLEM("route %d.%d: maneuver optimization raised dv from %.1f to %.1f m/s\n", d, j, before, after);
					if(itinerary_text_problem(copy, dep, arr)) OPERATION_PROBLEM("route %d.%d: text problem after optimizing maneuvers\n", d, j);
				}
				free_itinerary(copy);
			}

			// Moving the departure a day at a time must change delta-v smoothly, never jump to a mirrored flyby solution.
			if(encounters >= 3 && sweep_routes < 24) {
				copy = get_first(create_itin_copy_from_arrival(leaves[j]));
				update_itin_body_osvs(copy, system);
				if(resolve_itinerary_with_powered_flybys(system, copy, dep, arr, TF_FLYBY)) {
					sweep_routes++;
					double previous = 0;
					for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) previous += itinerary_step_dv(s, dep, arr, TF_FLYBY);
					for(int click = 1; click <= 20; click++) {
						set_itinerary_step_date(copy, system, copy->date + (sweep_routes % 2 ? 1 : -1));
						update_itin_body_osvs(copy, system);
						if(!resolve_itinerary_with_powered_flybys(system, copy, dep, arr, TF_FLYBY)) break;
						double current = 0;
						for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) current += itinerary_step_dv(s, dep, arr, TF_FLYBY);
						if(current > previous * 1.4 + 200.0) OPERATION_PROBLEM("route %d.%d: moving the departure one day raised dv from %.0f to %.0f m/s (click %d)\n", d, j, previous, current, click);
						previous = current;
					}
				}
				free_itinerary(copy);
			}

			for(int k = 0; k < nodes; k++) {
				// Moving the step's date.
				copy = get_first(create_itin_copy_from_arrival(leaves[j]));
				struct ItinStep *moved = copy;
				for(int n = 0; n < k; n++) moved = moved->next[0];
				if(moved->prev == NULL) { free_itinerary(copy); continue; }
				double original_date = moved->date;
				Vector3 original_r = moved->r;
				double target = moved->next != NULL ? original_date + 0.3 * (moved->next[0]->date - original_date)
					: original_date - 0.3 * (original_date - moved->prev->date);
				set_itinerary_step_date(moved, system, target);
				problem = itinerary_structure_problem(copy);
				if(problem) OPERATION_PROBLEM("route %d.%d: structure problem %d after moving step %d\n", d, j, problem, k);
				itinerary_recompute_problem(copy, system);
				if(itinerary_text_problem(copy, dep, arr)) OPERATION_PROBLEM("route %d.%d: text problem after moving step %d\n", d, j, k);
				if(moved->body == NULL) {
					double slide = mag_vec3(subtract_vec3(moved->r, original_r)) / mag_vec3(original_r);
					if(!(slide > 1e-6)) OPERATION_PROBLEM("route %d.%d: maneuver node did not move with its date\n", d, j);
					set_itinerary_step_date(moved, system, original_date);
					double shift = mag_vec3(subtract_vec3(moved->r, original_r)) / mag_vec3(original_r);
					if(shift > 0.05) OPERATION_PROBLEM("route %d.%d: maneuver node moved %.3f of its radius after a date round trip\n", d, j, shift);
				}
				free_itinerary(copy);

				// Moving a step past its successor must push the later steps back.
				copy = get_first(create_itin_copy_from_arrival(leaves[j]));
				moved = copy;
				for(int n = 0; n < k; n++) moved = moved->next[0];
				if(moved->prev != NULL && moved->next != NULL) {
					set_itinerary_step_date(moved, system, moved->next[0]->date + 2.0);
					problem = itinerary_structure_problem(copy);
					if(problem) OPERATION_PROBLEM("route %d.%d: structure problem %d after moving step %d past its successor\n", d, j, problem, k);
				}
				free_itinerary(copy);
			}
		}
		free(leaves);
	}
#undef OPERATION_PROBLEM
	// A route that does not converge falls back to the plain recompute, which is fine for a few routes.
	if(resolve_fallbacks * 100 > resolve_attempts * 30) { problems++; fprintf(stderr, "re-solve fell back on %d of %d routes\n", resolve_fallbacks, resolve_attempts); }
	else printf("Planner re-solve: %d of %d stored routes re-solved to the stored delta-v (the rest fall back)\n", resolve_attempts - resolve_fallbacks, resolve_attempts);
	if(problems == 0) printf("Itinerary operations check passed: %d routes (%d with maneuvers, %d flagged as having a flyby that hits a planet) survive structure, text, dv-sum, recompute, removal and date-change checks\n", routes, maneuver_routes, flagged_routes);
	else fprintf(stderr, "Itinerary operations check failed: %d problems on %d routes\n", problems, routes);
	return problems == 0 && routes > 0;
}

static int run_dsm_comparison(CelestSystem *system, Body *departure_body, Body *flyby_body,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {flyby_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 2
	};
	calc_data.allow_dsm = 0;
	Itin_Calc_Results without_dsm = search_for_itineraries(calc_data);
	calc_data.allow_dsm = 1;
	Itin_Calc_Results with_dsm = search_for_itineraries(calc_data);

	int valid = 0;
	double best_without = DBL_MAX, best_with = DBL_MAX;
	int maneuver_routes = 0, bad_routes = 0;
	Itin_Calc_Results *runs[] = {&without_dsm, &with_dsm};
	for(int run = 0; run < 2; run++) {
		for(int i = 0; i < runs[run]->num_deps; i++) {
			int count = get_number_of_itineraries(runs[run]->departures[i]);
			struct ItinStep **leaves = malloc((size_t)count * sizeof(*leaves));
			if(leaves == NULL) goto done;
			int stored = 0;
			store_itineraries_in_array(runs[run]->departures[i], leaves, &stored);
			for(int j = 0; j < stored; j++) {
				double dsm_dv;
				double total = stored_total_dv(leaves[j], &calc_data, &dsm_dv);
				if(!isfinite(total)) { bad_routes++; continue; }
				int body_nodes = 0;
				for(struct ItinStep *step = leaves[j]; step != NULL; step = step->prev)
					body_nodes += step->body != NULL;
				// Direct routes have no flyby, so a DSM cannot change them.
				if(body_nodes < 3) continue;
				if(run == 0 && total < best_without) best_without = total;
				if(run == 1 && total < best_with) best_with = total;
				if(run == 1 && dsm_dv > 0) {
					maneuver_routes++;
					double plain = plain_cost_without_maneuvers(leaves[j], &calc_data);
					// A kept maneuver must beat the same route without it by more than the noise floor.
					if(!(plain < DBL_MAX) || total > plain - 1.0 || dsm_dv < 1.0) bad_routes++;
				}
			}
			free(leaves);
		}
	}
	printf("DSM comparison (Earth-Venus-Jupiter flyby routes, one departure): best total dV %.3f m/s without DSM, %.3f m/s with DSM (saving %.3f); %d routes carry a maneuver; optimizer improved %d of %d routes, best saving %.3f m/s\n",
		best_without, best_with, best_without - best_with, maneuver_routes,
		with_dsm.powered_dsm_routes_improved, with_dsm.powered_dsm_routes_optimized,
		with_dsm.powered_best_dsm_saving);
	valid = best_without < DBL_MAX && best_with < DBL_MAX && bad_routes == 0 &&
		best_with <= best_without + 1.0 && best_without - best_with >= 50.0 &&
		maneuver_routes > 0 && with_dsm.powered_dsm_routes_improved > 0;
	if(!valid)
		fprintf(stderr, "DSM comparison failed: %d inconsistent routes, %d with maneuvers, saving %.3f m/s\n",
			bad_routes, maneuver_routes, best_without - best_with);

	if(valid) {
		// Saving and reloading must keep maneuver nodes (stored with body id -1) and their delta-v.
		char file_path[] = "dsm_roundtrip.itins";
		struct ItinStep **saved = with_dsm.departures;
		int saved_routes = 0, saved_maneuvers = 0;
		double saved_best = DBL_MAX;
		summarize_itineraries(saved, with_dsm.num_deps, &calc_data, &saved_routes, &saved_maneuvers, &saved_best);
		store_itineraries_in_bfile(saved, with_dsm.num_nodes, with_dsm.num_deps, with_dsm.num_itins,
			calc_data, system, file_path, get_current_bin_file_type());
		struct ItinsLoadFileResults loaded = load_itineraries_from_bfile(file_path);
		int loaded_routes = 0, loaded_maneuvers = 0;
		double loaded_best = DBL_MAX;
		if(loaded.departures != NULL)
			summarize_itineraries(loaded.departures, loaded.header.num_deps, &loaded.header.calc_data,
				&loaded_routes, &loaded_maneuvers, &loaded_best);
		if(loaded.departures == NULL || saved_routes != loaded_routes ||
			saved_maneuvers != loaded_maneuvers || saved_maneuvers == 0 ||
			fabs(saved_best - loaded_best) > 0.01) {
			fprintf(stderr, "DSM file round trip failed: %d/%d routes, %d/%d maneuver nodes, best %.3f/%.3f m/s\n",
				loaded_routes, saved_routes, loaded_maneuvers, saved_maneuvers, loaded_best, saved_best);
			valid = 0;
		} else {
			if(!check_itinerary_operations(loaded.departures, loaded.header.num_deps, loaded.header.system) ||
				!check_itinerary_operations(with_dsm.departures, with_dsm.num_deps, system))
				valid = 0;
			printf("DSM file round trip passed: %d routes, %d maneuver nodes, best %.3f m/s\n",
				loaded_routes, loaded_maneuvers, loaded_best);
		}
		for(int i = 0; loaded.departures != NULL && i < loaded.header.num_deps; i++)
			free_itinerary(loaded.departures[i]);
		free(loaded.departures);
		if(loaded.header.system != NULL) free_celestial_system(loaded.header.system);
		remove(file_path);
	}

done:
	for(int run = 0; run < 2; run++) {
		for(int i = 0; i < runs[run]->num_deps; i++) free_itinerary(runs[run]->departures[i]);
		free(runs[run]->departures);
	}
	return valid;
}

// Model-level DSM checks on routes with fixed dates, independent of the search.
static int check_dsm_model(CelestSystem *system, Body *departure_body, Body *flyby_body,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {flyby_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 2
	};
	Body *bodies[] = {departure_body, flyby_body, arrival_body};
	// Flyby and arrival offsets in days; the first pair is a route where a DSM clearly helps.
	double pairs[][2] = {{66, 1414}, {218, 1414}, {238, 1414}, {120, 900}, {200, 1100}, {150, 1300}};
	int valid = 1;
	double first_saving = 0, first_maneuver = 0;
	PoweredLegDsm first_found[2] = {{0}};
	for(size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
		double times[] = {departure_jd, departure_jd + pairs[p][0], departure_jd + pairs[p][1]};
		double plain = evaluate_powered_flyby_route_objective(&calc_data, bodies, times, 3);
		PoweredLegDsm zero[2] = {{.enabled = 1, .eta = 0.5}, {.enabled = 1, .eta = 0.3}};
		double with_zero = evaluate_powered_flyby_route_objective_with_dsm(&calc_data, bodies, times, 3, zero);
		PoweredLegDsm found[2];
		double best = optimize_powered_flyby_route_dsm(&calc_data, bodies, times, 3, found);
		double recheck = evaluate_powered_flyby_route_objective_with_dsm(&calc_data, bodies, times, 3, found);
		if(!(plain < DBL_MAX) || !(with_zero < DBL_MAX) || !(best < DBL_MAX)) {
			fprintf(stderr, "DSM model: route +%.0f/+%.0f was not evaluable\n", pairs[p][0], pairs[p][1]);
			valid = 0;
			continue;
		}
		// A zero maneuver is the plain route; the gap is the evaluator's convergence noise.
		if(fabs(with_zero - plain) > 10.0) {
			fprintf(stderr, "DSM model: zero-delta-v DSM changed the cost by %.3f m/s on route +%.0f/+%.0f\n",
				with_zero - plain, pairs[p][0], pairs[p][1]);
			valid = 0;
		}
		if(best > plain + 1e-6 || fabs(recheck - best) > 0.01) {
			fprintf(stderr, "DSM model: optimizer returned %.3f (recheck %.3f) against plain %.3f on route +%.0f/+%.0f\n",
				best, recheck, plain, pairs[p][0], pairs[p][1]);
			valid = 0;
		}
		if(p == 0) {
			first_saving = plain - best;
			memcpy(first_found, found, sizeof(first_found));
			for(int leg = 0; leg < 2; leg++)
				if(found[leg].enabled) first_maneuver = fmax(first_maneuver, mag_vec3(found[leg].delta_v));
		}
	}
	if(first_saving < 100.0 || first_maneuver < POWERED_TEST_DV_TOLERANCE) {
		fprintf(stderr, "DSM model: expected a saving above 100 m/s on the first route, got %.3f m/s with a %.1f m/s maneuver\n",
			first_saving, first_maneuver);
		valid = 0;
	}

	// A DSM counts toward the satellite delta-v limit, so a tight limit must reject it.
	double times[] = {departure_jd, departure_jd + pairs[0][0], departure_jd + pairs[0][1]};
	calc_data.dv_filter.max_satdv = first_maneuver / 4;
	double limited = evaluate_powered_flyby_route_objective_with_dsm(&calc_data, bodies, times, 3, first_found);
	calc_data.dv_filter.max_satdv = 1e9;
	double unlimited = evaluate_powered_flyby_route_objective_with_dsm(&calc_data, bodies, times, 3, first_found);
	if(limited < DBL_MAX || !(unlimited < DBL_MAX)) {
		fprintf(stderr, "DSM model: satellite delta-v limit did not govern the maneuver (limited %g, unlimited %g)\n",
			limited, unlimited);
		valid = 0;
	}
	if(valid)
		printf("DSM model checks passed: a DSM saves %.1f m/s on the first route (maneuver %.1f m/s), zero maneuver matches the plain route, satellite limit enforced\n",
			first_saving, first_maneuver);
	return valid;
}

// Flybys with nearly equal in and out speeds must stay feasible whichever speed is larger.
// Routes that return to the same body after revolutions must be stored consistently and re-solve to the stored delta-v.
static int check_resonant_flybys(CelestSystem *system, Body *departure_body, Body *flyby_body,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {departure_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 2, .allow_resonant = 1
	};
	calc_data.allow_dsm = 1;
	Itin_Calc_Results results = search_for_itineraries(calc_data);

	int resonant_routes = 0, bad_routes = 0, resolve_failures = 0, reported = 0, scored_routes = 0, unscorable_routes = 0, maneuver_routes = 0;
	double best = DBL_MAX, worst_recompute = 0, worst_resolve = 0;
	struct ItinStep *best_leaf = NULL;
	// Only the cheapest routes are corrected by the optimizer, so the second pass checks those.
	for(int pass = 0; pass < 2; pass++)
	for(int i = 0; i < results.num_deps; i++) {
		int count = get_number_of_itineraries(results.departures[i]);
		struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
		if(leaves == NULL) { bad_routes++; continue; }
		int stored = 0;
		store_itineraries_in_array(results.departures[i], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			int resonant = route_is_resonant(leaves[j]);
			if(!resonant) continue;
			double total = stored_total_dv(leaves[j], &calc_data, NULL);
			if(pass == 0) {
				resonant_routes++;
				if(isfinite(total) && total < best) { best = total; best_leaf = leaves[j]; }
				continue;
			}
			if(!isfinite(total)) { bad_routes++; continue; }
			if(total > best * 1.1 + 100) continue;
			double recomputed = plain_cost_without_maneuvers(leaves[j], &calc_data);
			int has_maneuver = 0;
			for(struct ItinStep *step = leaves[j]; step != NULL; step = step->prev) has_maneuver |= step->body == NULL;
			maneuver_routes += has_maneuver;
			// Routes the optimizer could not correct stay as raw search nodes and cannot be scored from dates.
			if(!(recomputed < DBL_MAX)) { unscorable_routes++; continue; }
			scored_routes++;
			double recompute_gap = has_maneuver ? 0 : fabs(recomputed - total);
			worst_recompute = fmax(worst_recompute, recompute_gap);
			if(recompute_gap > POWERED_TEST_DV_TOLERANCE) {
				bad_routes++;
				if(reported++ < 5) fprintf(stderr, "Resonant route stored %.3f but recomputes to %.3f m/s\n", total, recomputed);
			}
			// The planner re-solves from dates alone, so an unchanged route must reproduce its stored cost.
			struct ItinStep *copy = get_first(create_itin_copy_from_arrival(leaves[j]));
			double dep = copy->body->atmo_alt + 50e3, arr = get_last(copy)->body->atmo_alt + 50e3;
			update_itin_body_osvs(copy, system);
			int converged = 0;
			int resolved = resolve_itinerary_with_powered_flybys_approximate(system, copy, dep, arr, TF_FLYBY, &converged);
			double after = 0;
			for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) after += itinerary_step_dv(s, dep, arr, TF_FLYBY);
			double resolve_gap = resolved && converged ? fabs(after - total) : DBL_MAX;
			if(resolve_gap < DBL_MAX) worst_resolve = fmax(worst_resolve, resolve_gap);
			if(!(after <= total + 0.5 + 1e-4 * total) || !resolved || !converged) {
				resolve_failures++;
				if(reported++ < 5) fprintf(stderr, "Resonant route stored %.3f re-solves to %.3f m/s (resolved %d, converged %d)\n", total, after, resolved, converged);
			}
			free_itinerary(copy);
		}
		free(leaves);
	}
	// A maneuver inside the revolution-spanning leg must be optimizable and never make the route worse.
	int dsm_probe_valid = 1, probe_maneuvers = 0;
	double probe_plain = DBL_MAX, probe_best = DBL_MAX;
	if(best_leaf != NULL) {
		Body *probe_bodies[16];
		double probe_times[16];
		size_t probe_steps = 0;
		for(struct ItinStep *step = best_leaf; step != NULL && probe_steps < 16; step = step->prev)
			if(step->body != NULL) { probe_bodies[probe_steps] = step->body; probe_times[probe_steps++] = step->date; }
		for(size_t a = 0, b = probe_steps - 1; a < b; a++, b--) {
			Body *tb = probe_bodies[a]; probe_bodies[a] = probe_bodies[b]; probe_bodies[b] = tb;
			double tt = probe_times[a]; probe_times[a] = probe_times[b]; probe_times[b] = tt;
		}
		PoweredLegDsm probe_dsm[16];
		probe_plain = evaluate_powered_flyby_route_objective(&calc_data, probe_bodies, probe_times, probe_steps);
		probe_best = optimize_powered_flyby_route_dsm(&calc_data, probe_bodies, probe_times, probe_steps, probe_dsm);
		for(size_t leg = 0; leg + 1 < probe_steps; leg++) probe_maneuvers += probe_dsm[leg].enabled;
		dsm_probe_valid = probe_plain < DBL_MAX && probe_best <= probe_plain + POWERED_TEST_DV_TOLERANCE;
		printf("Resonant DSM probe: %.3f m/s without, %.3f m/s optimized, %d maneuver(s)\n", probe_plain, probe_best, probe_maneuvers);
	}
	int valid = dsm_probe_valid && scored_routes > 0 && bad_routes == 0 && resolve_failures == 0;
	if(valid)
		printf("Resonant flyby check passed: %d resonant routes (%d scored, %d unscorable, %d with a maneuver), best %.3f m/s, worst recompute gap %.4f, worst re-solve gap %.4f m/s\n",
			resonant_routes, scored_routes, unscorable_routes, maneuver_routes, best, worst_recompute, worst_resolve);
	else
		fprintf(stderr, "Resonant flyby check failed: %d resonant routes, %d scored, %d inconsistent, %d did not re-solve\n",
			resonant_routes, scored_routes, bad_routes, resolve_failures);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return valid;
}

static int check_near_equal_speed_flybys(Body *body) {
	static const double speed_offsets[] = {-0.9, -0.5, 0.5, 0.9};
	int valid = 1;
	double minimum_radius = body->radius + body->atmo_alt + 1e4;
	Vector3 incoming = vec3(6000, 0, 0);
	for(int i = 0; i < 4; i++) {
		double speed = 6000 + speed_offsets[i];
		Vector3 outgoing = vec3(speed * cos(M_PI / 3), speed * sin(M_PI / 3), 0);
		PoweredFlybyResult result;
		int feasible = calculate_powered_flyby(incoming, outgoing, vec3(0, 0, 0), body, minimum_radius, &result);
		if(!feasible || !isfinite(result.burn_delta_v) || result.burn_delta_v > 5.0) {
			fprintf(stderr, "Near-equal-speed flyby with a %+.1f m/s speed difference: feasible %d, burn %.3f m/s\n", speed_offsets[i], feasible, feasible ? result.burn_delta_v : -1.0);
			valid = 0;
		}
	}
	if(valid) printf("Near-equal-speed flyby check passed on %s\n", body->name);
	return valid;
}

// A saved route whose flyby iteration used to settle on a mirrored, costlier solution on some departure dates.
static int check_departure_sweep_fixture(void) {
	struct ItinLoadFileResults loaded = load_single_itinerary_from_bfile("../00_Testing/fixtures/departure_sweep.itin");
	if(loaded.itin == NULL) {
		fprintf(stderr, "Departure sweep fixture could not be loaded\n");
		return 0;
	}
	struct ItinStep *first = get_first(loaded.itin);
	double dep = first->body->atmo_alt + 50e3, arr = get_last(first)->body->atmo_alt + 50e3;
	int valid = 1, resolved = 0;
	if(count_flybys_needing_burn(first, NULL) != 0) {
		fprintf(stderr, "Departure sweep: the stored route is wrongly flagged as having a flyby that hits a planet\n");
		valid = 0;
	}
	double previous = 0, worst = 0;
	for(int direction = -1; direction <= 1; direction += 2) {
		free_itinerary(first);
		loaded = load_single_itinerary_from_bfile("../00_Testing/fixtures/departure_sweep.itin");
		first = get_first(loaded.itin);
		update_itin_body_osvs(first, loaded.system);
		previous = 0;
		for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL) previous += itinerary_step_dv(s, dep, arr, TF_FLYBY);
		for(int click = 1; click <= 20; click++) {
			set_itinerary_step_date(first, loaded.system, first->date + direction);
			update_itin_body_osvs(first, loaded.system);
			if(!resolve_itinerary_with_powered_flybys(loaded.system, first, dep, arr, TF_FLYBY)) continue;
			resolved++;
			double current = 0;
			for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL) current += itinerary_step_dv(s, dep, arr, TF_FLYBY);
			worst = fmax(worst, current / previous);
			if(current > previous * 1.4 + 200.0) {
				fprintf(stderr, "Departure sweep: moving the departure %+d day raised dv from %.0f to %.0f m/s (click %d)\n", direction, previous, current, click);
				valid = 0;
			}
			previous = current;
		}
	}
	// Moving the maneuver node a day at a time in either direction must always re-solve.
	for(int direction = -1; direction <= 1; direction += 2) {
		free_itinerary(first);
		loaded = load_single_itinerary_from_bfile("../00_Testing/fixtures/departure_sweep.itin");
		first = get_first(loaded.itin);
		struct ItinStep *maneuver = first->next[0];
		if(maneuver->body != NULL) { fprintf(stderr, "Departure sweep fixture has no maneuver node\n"); valid = 0; continue; }
		update_itin_body_osvs(first, loaded.system);
		for(int click = 1; click <= 20; click++) {
			set_itinerary_step_date(maneuver, loaded.system, maneuver->date + direction);
			update_itin_body_osvs(first, loaded.system);
			if(!resolve_itinerary_with_powered_flybys(loaded.system, first, dep, arr, TF_FLYBY)) {
				fprintf(stderr, "Departure sweep: the maneuver could not be re-solved after moving it %+d day(s) (click %d)\n", direction, click);
				valid = 0;
				break;
			}
		}
	}
	// A date the flyby iteration cannot settle on (second Earth flyby 13 days later) must still give an approximate result.
	free_itinerary(first);
	loaded = load_single_itinerary_from_bfile("../00_Testing/fixtures/departure_sweep.itin");
	first = get_first(loaded.itin);
	struct ItinStep *second_earth = first->next[0]->next[0]->next[0];
	update_itin_body_osvs(first, loaded.system);
	set_itinerary_step_date(second_earth, loaded.system, second_earth->date + 13);
	update_itin_body_osvs(first, loaded.system);
	int approximate_converged = 1;
	int approximate_ok = resolve_itinerary_with_powered_flybys_approximate(loaded.system, first, dep, arr, TF_FLYBY, &approximate_converged);
	double approximate_total = 0;
	for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL) approximate_total += itinerary_step_dv(s, dep, arr, TF_FLYBY);
	if(!approximate_ok || !isfinite(approximate_total) || approximate_total <= 0) {
		fprintf(stderr, "Departure sweep: no approximate result for the second Earth flyby 13 days later\n");
		valid = 0;
	} else {
		printf("Approximate re-solve of the second Earth flyby +13 days: converged %d, total dv %.0f m/s\n", approximate_converged, approximate_total);
	}
	free_itinerary(first);
	if(loaded.system != NULL) free_celestial_system(loaded.system);

	// The step panel must show the periapsis of the powered flyby that is flown, never the unpowered one below the surface.
	loaded = load_single_itinerary_from_bfile("../00_Testing/fixtures/departure_sweep.itin");
	first = get_first(loaded.itin);
	second_earth = first->next[0]->next[0]->next[0];
	update_itin_body_osvs(first, loaded.system);
	set_itinerary_step_date(second_earth, loaded.system, second_earth->date + 35);
	update_itin_body_osvs(first, loaded.system);
	if(resolve_itinerary_with_powered_flybys_approximate(loaded.system, first, dep, arr, TF_FLYBY, &approximate_converged)) {
		char panel_labels[ITIN_STEP_TEXT_SIZE], panel_values[ITIN_STEP_TEXT_SIZE];
		itinerary_step_parameters_to_string(panel_labels, panel_values, DATE_ISO, dep, arr, second_earth);
		char *altitude_line = panel_values;
		for(int line = 0; line < 3 && altitude_line != NULL; line++) {
			altitude_line = strchr(altitude_line, '\n');
			if(altitude_line != NULL) altitude_line++;
		}
		double minimum_altitude_km = (second_earth->body->atmo_alt + 1e4) / 1e3;
		if(altitude_line == NULL || atof(altitude_line) < minimum_altitude_km - 0.6) {
			fprintf(stderr, "Departure sweep: the step panel shows a periapsis altitude of %s km, below the %.0f km minimum\n",
				altitude_line != NULL ? altitude_line : "?", minimum_altitude_km);
			valid = 0;
		}
	}
	free_itinerary(first);
	if(loaded.system != NULL) free_celestial_system(loaded.system);
	if(resolved < 30) {
		fprintf(stderr, "Departure sweep: only %d of 40 edits could be re-solved\n", resolved);
		valid = 0;
	}
	if(valid) printf("Departure sweep fixture passed: %d edits re-solved, largest single-click dv ratio %.2f\n", resolved, worst);
	return valid;
}

int main(int argc, char **argv) {
	if(argc > 1 && strcmp(argv[argc - 1], "--resonant") == 0) {
		benchmark_resonant = 1;
		argc--;
	}
	if(argc == 2 && strcmp(argv[1], "--mission-benchmarks") == 0)
		return run_saved_mission_benchmarks();
	if(argc == 2 && strcmp(argv[1], "--full-profile") == 0)
		return run_full_profile();
	if(argc != 1) {
		fprintf(stderr, "Usage: %s [--full-profile|--mission-benchmarks]\n", argv[0]);
		return EXIT_FAILURE;
	}

	char system_path[] = "../Celestial_Systems/solar_system.cfg";
	CelestSystem *system = load_celestial_system_from_cfg_file(system_path);
	if(system == NULL) {
		fprintf(stderr, "Failed to load test celestial system: %s\n", system_path);
		return EXIT_FAILURE;
	}
	Body *departure_body = get_body_by_name("EARTH", system);
	Body *flyby_body = get_body_by_name("VENUS", system);
	Body *arrival_body = get_body_by_name("JUPITER", system);
	if(departure_body == NULL || flyby_body == NULL || arrival_body == NULL) {
		fprintf(stderr, "Test system is missing Earth, Venus, or Jupiter\n");
		free_celestial_system(system);
		return EXIT_FAILURE;
	}

	double departure_jd = system->ut0 + 100;
	OSV departure_state = osv_from_elements(departure_body->orbit, departure_jd);
	OSV arrival_state = osv_from_elements(arrival_body->orbit, departure_jd);
	double hohmann_duration = calc_hohmann_transfer(
		mag_vec3(departure_state.r), mag_vec3(arrival_state.r), system->cb).dur / 86400;
	double min_transfer_duration = 0.4 * hohmann_duration;
	double max_arrival_jd = departure_jd + min_transfer_duration + 0.25;
	int search_nodes, search_itins;

	int search_valid = run_bounded_search(system, departure_body, arrival_body, departure_jd,
		max_arrival_jd, &search_nodes, &search_itins);
	int powered_valid = run_powered_flyby_objective_regression(system, departure_body,
		flyby_body, arrival_body, departure_jd, hohmann_duration);
	int seeds_valid = check_speed_changing_seed_generation(system, departure_body,
		flyby_body, arrival_body, departure_jd);
	int parking_valid = check_parking_orbit_search(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int dsm_model_valid = check_dsm_model(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int dsm_valid = run_dsm_comparison(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int sweep_valid = check_departure_sweep_fixture();
	int resonant_valid = check_resonant_flybys(system, departure_body, flyby_body, arrival_body,
		departure_jd, hohmann_duration);
	int near_equal_valid = check_near_equal_speed_flybys(departure_body);

	free_celestial_system(system);
	if(!search_valid || !powered_valid || !seeds_valid || !parking_valid || !dsm_model_valid || !dsm_valid ||
		!sweep_valid || !resonant_valid || !near_equal_valid) {
		fprintf(stderr, "Integration checks failed (bounded search: %d nodes/%d itineraries)\n",
			search_nodes, search_itins);
		return EXIT_FAILURE;
	}
	printf("UI-free itinerary integration passed: %d nodes, %d itineraries\n",
		search_nodes, search_itins);
	return EXIT_SUCCESS;
}
