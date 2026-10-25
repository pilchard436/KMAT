#include "integration_common.h"

// The same body twice in a row, ignoring maneuver nodes between the visits.
int route_is_resonant(struct ItinStep *leaf) {
	Body *later = NULL;
	for(struct ItinStep *step = leaf; step != NULL; step = step->prev) {
		if(step->body == NULL) continue;
		if(later == step->body) return 1;
		later = step->body;
	}
	return 0;
}

int check_speed_changing_seed_generation(CelestSystem *system, Body *departure,
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

int run_bounded_search(CelestSystem *system, Body *departure_body, Body *arrival_body,
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

double score_powered_arrival_route(struct ItinStep *arrival,
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

int run_powered_flyby_objective_regression(CelestSystem *system,
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
	// Force exhaustion so the budget accounting is exercised even with a large default budget.
	set_powered_seed_budget(64);
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	set_powered_seed_budget(POWERED_SEEDS_PER_DEPARTURE);
	if(results.num_deps <= 0 || results.num_itins <= 0) goto cleanup;
	if(results.powered_seed_stats.seeds_added > 64 ||
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
		// Routes that settle in the SOI iteration are not rejected, so repairs only have to be consistent when needed.
		if((results.powered_initial_rejections > 0 && (results.powered_repairs_attempted == 0 ||
			results.powered_repairs_recovered == 0 || results.powered_repairs_competing == 0)) ||
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

// Total mission delta-v implied by the stored node velocities, including deep-space maneuvers.
double stored_total_dv(struct ItinStep *leaf, const Itin_Calc_Data *calc_data, double *dsm_dv) {
	struct PorkchopPoint point = create_porkchop_point(leaf, calc_data->dv_filter.dep_periapsis,
		calc_data->dv_filter.arr_periapsis);
	if(dsm_dv != NULL) *dsm_dv = point.dv_dsm;
	return point.dv_dep + point.dv_dsm + point.dv_flyby;
}

// Cost of the same route with its maneuvers removed, recomputed from encounter dates alone.
double plain_cost_without_maneuvers(struct ItinStep *leaf, const Itin_Calc_Data *calc_data) {
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
void summarize_itineraries(struct ItinStep **departures, int num_departures,
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

// A saved route whose flyby iteration used to settle on a mirrored, costlier solution on some departure dates.
int check_departure_sweep_fixture(void) {
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

int check_empty_result_reason(CelestSystem *system, Body *departure_body, Body *arrival_body,
        double departure_jd, double max_arrival_jd) {
	Body *flyby_bodies[] = {departure_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = max_arrival_jd;
	calc_data.max_duration = max_arrival_jd - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9,
		.max_depdv = 1,	// no departure burn is this small
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
	char reason[200];
	describe_search_rejections(&results.rejections, reason, sizeof(reason));
	int valid = results.num_itins == 0 && strstr(reason, "departure delta-v") != NULL;
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	if(!valid) {
		fprintf(stderr, "An impossible departure limit must give no routes and name the departure delta-v limit, got %d routes and '%s'\n",
			results.num_itins, reason);
		return 0;
	}
	printf("Empty search result names its limit: %s\n", reason);
	return 1;
}
