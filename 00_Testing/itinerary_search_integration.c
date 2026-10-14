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

	struct ItinStep **arrivals = malloc((size_t)results.num_itins * sizeof(*arrivals));
	if(arrivals == NULL) goto cleanup;
	int arrival_count = 0;
	for(int i = 0; i < results.num_deps; i++)
		store_itineraries_in_array(results.departures[i], arrivals, &arrival_count);
	int has_valid_powered_route = 0;
	int has_speed_changing_powered_route = 0;
	for(int i = 0; i < arrival_count && !has_valid_powered_route; i++) {
		for(struct ItinStep *step = arrivals[i]->prev; step != NULL && step->prev != NULL;
			step = step->prev) {
			if(step->body != flyby_body) continue;
			PoweredFlybyResult flyby_result;
			double minimum_periapsis = flyby_body->radius + flyby_body->atmo_alt + 1e4;
			if(calculate_powered_flyby(step->v_arr, step->next[0]->v_dep, step->v_body,
				flyby_body, minimum_periapsis, &flyby_result) &&
				isfinite(flyby_result.burn_delta_v)) {
				has_valid_powered_route = 1;
				Vector3 incoming_excess = subtract_vec3(step->v_arr, step->v_body);
				Vector3 outgoing_excess = subtract_vec3(step->next[0]->v_dep, step->v_body);
				double excess_speed_change = fabs(mag_vec3(outgoing_excess) - mag_vec3(incoming_excess));
				if(excess_speed_change > 1.0 && flyby_result.burn_delta_v > POWERED_TEST_DV_TOLERANCE)
					has_speed_changing_powered_route = 1;
			}
			break;
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
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	free_celestial_system(system);
	const int expected_nodes = 7575;
	const int expected_itins = 3751;
	if(results.num_nodes != expected_nodes || results.num_itins != expected_itins) {
		fprintf(stderr, "SOI-pruned profile mismatch: got %d nodes/%d itineraries; expected %d/%d\n",
			results.num_nodes, results.num_itins, expected_nodes, expected_itins);
		return EXIT_FAILURE;
	}
	printf("UI-free full-profile search passed: %d nodes, %d itineraries\n",
		results.num_nodes, results.num_itins);
	return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
	if(argc == 2 && strcmp(argv[1], "--mission-benchmarks") == 0)
		return run_saved_mission_benchmarks();
	if(argc == 2 && strcmp(argv[1], "--full-profile") == 0)
		return run_full_profile();
	if(argc != 1) {
		fprintf(stderr, "Usage: %s [--full-profile]\n", argv[0]);
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
	int legacy_nodes, legacy_itins, izzo_nodes, izzo_itins;

	int legacy_valid = run_bounded_search(system, departure_body, arrival_body, departure_jd,
		max_arrival_jd, LAMBERT_METHOD_LEGACY, &legacy_nodes, &legacy_itins);
	int izzo_valid = run_bounded_search(system, departure_body, arrival_body, departure_jd,
		max_arrival_jd, LAMBERT_METHOD_IZZO, &izzo_nodes, &izzo_itins);
	int powered_valid = run_powered_flyby_objective_regression(system, departure_body,
		flyby_body, arrival_body, departure_jd, hohmann_duration);

	free_celestial_system(system);
	if(!legacy_valid || !izzo_valid || !powered_valid ||
		legacy_nodes != izzo_nodes || legacy_itins != izzo_itins) {
		fprintf(stderr, "Search mismatch: legacy %d nodes/%d itineraries, Izzo %d nodes/%d itineraries\n",
			legacy_nodes, legacy_itins, izzo_nodes, izzo_itins);
		return EXIT_FAILURE;
	}
	printf("UI-free itinerary integration passed: %d nodes, %d itineraries with both solvers\n",
		izzo_nodes, izzo_itins);
	return EXIT_SUCCESS;
}
