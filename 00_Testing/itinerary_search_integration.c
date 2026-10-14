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
	for(int i = 0; i < arrival_count && !has_valid_powered_route; i++) {
		for(struct ItinStep *step = arrivals[i]->prev; step != NULL && step->prev != NULL;
			step = step->prev) {
			if(step->body != flyby_body) continue;
			PoweredFlybyResult flyby_result;
			double minimum_periapsis = flyby_body->radius + flyby_body->atmo_alt + 1e4;
			if(calculate_powered_flyby(step->v_arr, step->next[0]->v_dep, step->v_body,
				flyby_body, minimum_periapsis, &flyby_result) &&
				isfinite(flyby_result.burn_delta_v)) has_valid_powered_route = 1;
			break;
		}
	}
	free(arrivals);
	if(!has_valid_powered_route) goto cleanup;

	PoweredFlybyOptimizationStats stats;
	optimize_powered_flyby_routes(results.departures, results.num_deps,
		&calc_data, 100, &stats);
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
		}
	}
	int objective_did_not_increase = stats.routes_valid > 0 && stats.routes_corrected > 0 &&
		isfinite(stats.best_initial_total_dv) && isfinite(stats.best_refined_total_dv) &&
		stats.best_refined_total_dv <= stats.best_initial_total_dv + POWERED_TEST_DV_TOLERANCE &&
		isfinite(best_search_objective) && isfinite(best_grid_objective) &&
		best_search_objective <= best_grid_objective *
			(1 + POWERED_TEST_GRID_RELATIVE_TOLERANCE) + POWERED_TEST_DV_TOLERANCE;
	if(!objective_did_not_increase)
		fprintf(stderr, "Powered objective regression failed: %d valid, %d corrected, refine %.3f -> %.3f m/s, search %.3f, grid %.3f m/s\n",
			stats.routes_valid, stats.routes_corrected, stats.best_initial_total_dv,
			stats.best_refined_total_dv, best_search_objective, best_grid_objective);
	else
		printf("Powered-flyby objective regression passed: %d valid, %d SOI-corrected, refine %.3f -> %.3f m/s, best search %.3f vs grid %.3f m/s\n",
			stats.routes_valid, stats.routes_corrected, stats.best_initial_total_dv,
			stats.best_refined_total_dv, best_search_objective, best_grid_objective);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return objective_did_not_increase;

cleanup:
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return 0;
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
