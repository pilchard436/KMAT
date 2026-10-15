#include "orbit_calculator/transfer_calc.h"
#include "orbit_calculator/powered_flyby_optimizer.h"
#include "orbitlib_fileio.h"
#include "orbitlib_transfer.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POWERED_TEST_DV_TOLERANCE 0.1
#define POWERED_TEST_MIN_LEG_DAYS 10.0
#define POWERED_TEST_GRID_RELATIVE_TOLERANCE 0.10

static int audit_requested;

static int validate_rejection_audit(int require_recovery) {
	if(!audit_requested) return 1;
	PoweredRejectionAudit audit = get_powered_rejection_audit();
	if(!audit.completed) return 0;
	PoweredRejectionAuditGroup groups[] = {audit.initial_failure, audit.refinement_cutoff};
	for(int i = 0; i < 2; i++) {
		PoweredRejectionAuditGroup group = groups[i];
		int expected = group.population < POWERED_AUDIT_SAMPLE_SIZE ? (int)group.population : POWERED_AUDIT_SAMPLE_SIZE;
		if(group.sampled != expected || group.feasible > group.sampled ||
			group.beats_retained_best > group.feasible || group.beats_retained_worst > group.feasible ||
			group.evaluations > (unsigned long long)group.sampled * POWERED_AUDIT_OBJECTIVES_PER_ROUTE) return 0;
		if(group.feasible > 0 && (!isfinite(group.best_delta_v) || group.best_delta_v >= DBL_MAX)) return 0;
	}
	if(require_recovery && audit.initial_failure.feasible == 0) return 0;
	return 1;
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
		Lambert3 inbound = calc_lambert3_selected(dep.r, encounter.r, days * 86400.0, system->cb);
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
        double min_departure_jd, double max_arrival_jd, enum LambertSolverMethod solver,
        int *num_nodes, int *num_itins) {
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

	set_lambert_solver_method(solver);
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
	Lambert3 inbound = calc_lambert3_selected(states[0].r, states[1].r,
		(times[1] - times[0]) * 86400, system->cb);
	Lambert3 outbound = calc_lambert3_selected(states[1].r, states[2].r,
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
	*outbound = calc_lambert3_selected(flyby_state.r, arrival_state.r,
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
		Lambert3 inbound = calc_lambert3_selected(departure_state.r, flyby_state.r,
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
	set_lambert_solver_method(LAMBERT_METHOD_IZZO);
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	if(results.num_deps <= 0 || results.num_itins <= 0) goto cleanup;
	if(!validate_rejection_audit(1)) {
		fprintf(stderr, "Rejected-route audit failed recovery or sampling/work-budget checks\n");
		goto cleanup;
	}
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
		.num_flyby_bodies = num_flybys
	};
	set_lambert_solver_method(LAMBERT_METHOD_IZZO);
	printf("\n=== %s saved-settings benchmark ===\n", label);
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	printf("%s benchmark result: %d departure roots, %d nodes, %d itineraries\n",
		label, results.num_deps, results.num_nodes, results.num_itins);
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
		6939, 6939, 0, 190e3, 1600e3);
	int saturn_valid = run_saved_mission_benchmark(system, "Saturn",
		"EARTH", "SATURN", saturn_flybys, 4,
		2461326.50, 2461691.50, 2466098.50, 4407.0,
		7282, 7282, 0, 190e3, 2050e3);
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
		.max_satdv = 0,
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
	set_lambert_solver_method(LAMBERT_METHOD_IZZO);
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	int valid = validate_rejection_audit(0) && results.num_itins > 0 && results.powered_routes_corrected > 0 &&
		results.powered_itineraries_pruned == 0 &&
		results.powered_repairs_attempted > 0 && results.powered_repairs_attempted <= POWERED_REPAIR_SLOTS &&
		results.powered_repairs_recovered <= results.powered_repairs_attempted &&
		results.powered_challenges_refined > 0 && results.powered_challenges_refined <= POWERED_CHALLENGE_SLOTS &&
		isfinite(results.best_powered_refined_total_dv) &&
		results.best_powered_refined_total_dv <= calc_data.dv_filter.max_totdv;
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
				point.dv_dsm > calc_data.dv_filter.max_satdv + POWERED_TEST_DV_TOLERANCE ||
				point.dep_date < calc_data.jd_min_dep || point.dep_date > calc_data.jd_max_dep ||
				leaves[j]->date > calc_data.jd_max_arr || point.dur > calc_data.max_duration)
				valid = 0;
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

#define COVERAGE_GRID_STEP_DAYS 4.0
#define COVERAGE_NEAR_OPTIMAL 0.50
#define COVERAGE_PROMINENCE 0.03
#define COVERAGE_BASIN_DAYS 8.0
#define COVERAGE_TIGHT 0.02
#define COVERAGE_REPORTED_FAMILIES 15

typedef struct CoverageRoute {
	double flyby_offset, arrival_offset, objective;
} CoverageRoute;

static int compare_coverage_routes(const void *left, const void *right) {
	double a = ((const CoverageRoute *)left)->objective;
	double b = ((const CoverageRoute *)right)->objective;
	return (a > b) - (a < b);
}

static const double *coverage_sort_grid;

static int compare_grid_cells(const void *left, const void *right) {
	double a = coverage_sort_grid[*(const int *)left];
	double b = coverage_sort_grid[*(const int *)right];
	return (a > b) - (a < b);
}

// Informational: compares the search with a dense grid of the exact objective on one small mission.
static int run_coverage_reference(CelestSystem *system, Body *departure_body, Body *flyby_body,
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
	set_lambert_solver_method(LAMBERT_METHOD_IZZO);
	Itin_Calc_Results results = search_for_itineraries(calc_data);

	int exit_code = EXIT_FAILURE;
	CoverageRoute *found = NULL, *minima = NULL;
	double *grid = NULL;
	int found_count = 0, minima_count = 0;
	int total_itineraries = 0;
	for(int i = 0; i < results.num_deps; i++)
		total_itineraries += get_number_of_itineraries(results.departures[i]);
	found = malloc((size_t)(total_itineraries > 0 ? total_itineraries : 1) * sizeof(*found));
	struct ItinStep **arrivals = malloc((size_t)(total_itineraries > 0 ? total_itineraries : 1) * sizeof(*arrivals));
	if(found == NULL || arrivals == NULL) { free(arrivals); goto cleanup; }
	for(int i = 0; i < results.num_deps; i++) {
		int stored = 0;
		store_itineraries_in_array(results.departures[i], arrivals, &stored);
		for(int j = 0; j < stored; j++) {
			struct ItinStep *leaf = arrivals[j];
			if(leaf->prev == NULL || leaf->prev->prev == NULL || leaf->prev->prev->prev != NULL ||
				leaf->prev->body != flyby_body) continue;
			double objective = score_powered_arrival_route(leaf, &calc_data, flyby_body);
			if(!isfinite(objective) || objective >= DBL_MAX) continue;
			found[found_count++] = (CoverageRoute){leaf->prev->date - departure_jd,
				leaf->date - departure_jd, objective};
		}
	}
	free(arrivals);

	const double step = COVERAGE_GRID_STEP_DAYS;
	int n = (int)floor((calc_data.max_duration - POWERED_TEST_MIN_LEG_DAYS) / step) + 1;
	grid = malloc((size_t)n * n * sizeof(*grid));
	if(n <= 0 || grid == NULL) goto cleanup;
	Body *bodies[] = {departure_body, flyby_body, arrival_body};
	unsigned long long evaluations = 0;
	int feasible_cells = 0;
	double reference_best = DBL_MAX;
	for(int i = 0; i < n; i++) {
		for(int j = 0; j < n; j++) {
			double flyby_offset = POWERED_TEST_MIN_LEG_DAYS + i * step;
			double arrival_offset = POWERED_TEST_MIN_LEG_DAYS + j * step;
			grid[(size_t)i * n + j] = DBL_MAX;
			if(arrival_offset < flyby_offset + POWERED_TEST_MIN_LEG_DAYS ||
				arrival_offset > calc_data.max_duration) continue;
			double times[] = {departure_jd, departure_jd + flyby_offset, departure_jd + arrival_offset};
			double objective = evaluate_powered_flyby_route_objective(&calc_data, bodies, times, 3);
			evaluations++;
			if(!isfinite(objective) || objective >= DBL_MAX) continue;
			grid[(size_t)i * n + j] = objective;
			feasible_cells++;
			if(objective < reference_best) reference_best = objective;
		}
	}
	if(feasible_cells == 0) goto cleanup;

	// A family is a valley of the objective surface: two minima stay separate only if the saddle
	// between them rises at least COVERAGE_PROMINENCE above the shallower valley floor.
	int cells = n * n;
	int *order = malloc((size_t)feasible_cells * sizeof(*order));
	int *parent = malloc((size_t)cells * sizeof(*parent));
	char *seen = calloc((size_t)cells, 1);
	minima = malloc((size_t)feasible_cells * sizeof(*minima));
	if(order == NULL || parent == NULL || seen == NULL || minima == NULL) {
		free(order); free(parent); free(seen);
		goto cleanup;
	}
	int ordered = 0;
	for(int c = 0; c < cells; c++) {
		parent[c] = c;
		if(grid[c] < DBL_MAX) order[ordered++] = c;
	}
	coverage_sort_grid = grid;
	qsort(order, (size_t)ordered, sizeof(*order), compare_grid_cells);
	double near_optimal_limit = reference_best * (1 + COVERAGE_NEAR_OPTIMAL);
	for(int k = 0; k < ordered; k++) {
		int cell = order[k];
		int roots[8], root_count = 0;
		for(int di = -1; di <= 1; di++)
			for(int dj = -1; dj <= 1; dj++) {
				int ni = cell / n + di, nj = cell % n + dj;
				if((di == 0 && dj == 0) || ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
				int neighbour = ni * n + nj;
				if(!seen[neighbour]) continue;
				int root = neighbour;
				while(parent[root] != root) {
					parent[root] = parent[parent[root]];
					root = parent[root];
				}
				int known = 0;
				for(int r = 0; r < root_count; r++) known |= roots[r] == root;
				if(!known) roots[root_count++] = root;
			}
		seen[cell] = 1;
		if(root_count == 0) continue;
		int oldest = roots[0];
		for(int r = 1; r < root_count; r++)
			if(grid[roots[r]] < grid[oldest]) oldest = roots[r];
		parent[cell] = oldest;
		for(int r = 0; r < root_count; r++) {
			if(roots[r] == oldest) continue;
			double floor_value = grid[roots[r]];
			if(grid[cell] - floor_value >= COVERAGE_PROMINENCE * floor_value && floor_value <= near_optimal_limit)
				minima[minima_count++] = (CoverageRoute){
					POWERED_TEST_MIN_LEG_DAYS + (roots[r] / n) * step,
					POWERED_TEST_MIN_LEG_DAYS + (roots[r] % n) * step, floor_value};
			parent[roots[r]] = oldest;
		}
	}
	for(int c = 0; c < cells; c++)
		if(seen[c] && parent[c] == c && grid[c] <= near_optimal_limit)
			minima[minima_count++] = (CoverageRoute){POWERED_TEST_MIN_LEG_DAYS + (c / n) * step,
				POWERED_TEST_MIN_LEG_DAYS + (c % n) * step, grid[c]};
	free(order); free(parent); free(seen);
	qsort(minima, (size_t)minima_count, sizeof(*minima), compare_coverage_routes);
	double search_best = DBL_MAX;
	for(int i = 0; i < found_count; i++)
		if(found[i].objective < search_best) search_best = found[i].objective;
	printf("Coverage reference (Earth-Venus-Jupiter, one departure): grid step %.1f d, %llu evaluations, %d feasible cells\n",
		step, evaluations, feasible_cells);
	printf("Reference best %.1f m/s; search returned %d routes of this sequence, best %.1f m/s\n",
		reference_best, found_count, search_best);
	printf("Powered seeds added %llu, encounter searches skipped after budget %llu\n",
		results.powered_seed_stats.seeds_added, results.powered_seed_stats.searches_budget_skipped);
	{
		// Mirrors the first-leg duration window used by the search (calc_itins_from_departure).
		OSV dep_state = osv_from_elements(departure_body->orbit, departure_jd);
		OSV flyby_state = osv_from_elements(flyby_body->orbit, departure_jd);
		double r0 = mag_vec3(dep_state.r), r1 = mag_vec3(flyby_state.r);
		double hohmann_days = calc_hohmann_transfer(r0, r1, system->cb).dur / 86400;
		double ratio = r1 / r0;
		double window_days = (4 * (ratio - 0.85) * (ratio - 0.85) + 1.5) * hohmann_days;
		if(window_days / hohmann_days > 3) window_days = 3 * hohmann_days;
		printf("First-leg grid to %s covers %.0f to %.0f days after departure\n",
			flyby_body->name, 0.4 * hohmann_days, window_days);
	}
	int in_basin = 0, tight = 0;
	for(int i = 0; i < minima_count; i++) {
		double best_in_basin = DBL_MAX;
		int routes_in_basin = 0;
		for(int k = 0; k < found_count; k++) {
			if(fabs(found[k].flyby_offset - minima[i].flyby_offset) > COVERAGE_BASIN_DAYS ||
				fabs(found[k].arrival_offset - minima[i].arrival_offset) > COVERAGE_BASIN_DAYS) continue;
			routes_in_basin++;
			if(found[k].objective < best_in_basin) best_in_basin = found[k].objective;
		}
		int covered = routes_in_basin > 0;
		int is_tight = covered && best_in_basin <= minima[i].objective * (1 + COVERAGE_TIGHT);
		in_basin += covered;
		tight += is_tight;
		if(i < COVERAGE_REPORTED_FAMILIES) {
			if(covered)
				printf("  family %2d: flyby +%4.0f d, arrival +%4.0f d, reference %.1f m/s, search %.1f m/s (%d routes) %s\n",
					i + 1, minima[i].flyby_offset, minima[i].arrival_offset, minima[i].objective,
					best_in_basin, routes_in_basin, is_tight ? "COVERED" : "found, not refined to within 2%");
			else
				printf("  family %2d: flyby +%4.0f d, arrival +%4.0f d, reference %.1f m/s, search returned none  MISSED\n",
					i + 1, minima[i].flyby_offset, minima[i].arrival_offset, minima[i].objective);
		}
	}
	printf("Families within %.0f%% of the reference best: %d; basin covered %d/%d; within %.0f%% of reference %d/%d\n",
		COVERAGE_NEAR_OPTIMAL * 100, minima_count, in_basin, minima_count,
		COVERAGE_TIGHT * 100, tight, minima_count);
	exit_code = EXIT_SUCCESS;

cleanup:
	free(found);
	free(grid);
	free(minima);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return exit_code;
}

int main(int argc, char **argv) {
	if(argc > 1 && strcmp(argv[argc - 1], "--audit-rejections") == 0) {
		audit_requested = 1;
		set_powered_rejection_audit_enabled(1);
		argc--;
	}
	int coverage_requested = argc == 2 && strcmp(argv[1], "--coverage-reference") == 0;
	if(coverage_requested) argc = 1;
	if(argc == 2 && strcmp(argv[1], "--mission-benchmarks") == 0)
		return run_saved_mission_benchmarks();
	if(argc == 2 && strcmp(argv[1], "--full-profile") == 0)
		return run_full_profile();
	if(argc != 1) {
		fprintf(stderr, "Usage: %s [--full-profile|--mission-benchmarks|--coverage-reference] [--audit-rejections]\n", argv[0]);
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
	if(coverage_requested) {
		int coverage_result = run_coverage_reference(system, departure_body, flyby_body,
			arrival_body, departure_jd, hohmann_duration);
		free_celestial_system(system);
		return coverage_result;
	}
	int legacy_nodes, legacy_itins, izzo_nodes, izzo_itins;

	int legacy_valid = run_bounded_search(system, departure_body, arrival_body, departure_jd,
		max_arrival_jd, LAMBERT_METHOD_LEGACY, &legacy_nodes, &legacy_itins);
	int izzo_valid = run_bounded_search(system, departure_body, arrival_body, departure_jd,
		max_arrival_jd, LAMBERT_METHOD_IZZO, &izzo_nodes, &izzo_itins);
	int powered_valid = run_powered_flyby_objective_regression(system, departure_body,
		flyby_body, arrival_body, departure_jd, hohmann_duration);
	int seeds_valid = check_speed_changing_seed_generation(system, departure_body,
		flyby_body, arrival_body, departure_jd);

	free_celestial_system(system);
	if(!legacy_valid || !izzo_valid || !powered_valid || !seeds_valid ||
		legacy_nodes != izzo_nodes || legacy_itins != izzo_itins) {
		fprintf(stderr, "Search mismatch: legacy %d nodes/%d itineraries, Izzo %d nodes/%d itineraries\n",
			legacy_nodes, legacy_itins, izzo_nodes, izzo_itins);
		return EXIT_FAILURE;
	}
	printf("UI-free itinerary integration passed: %d nodes, %d itineraries with both solvers\n",
		izzo_nodes, izzo_itins);
	return EXIT_SUCCESS;
}
