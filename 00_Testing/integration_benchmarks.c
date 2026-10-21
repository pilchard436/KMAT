#include "integration_common.h"

#include <sys/time.h>
#include <time.h>

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
	if(benchmark_coverage) {
		double best_total = DBL_MAX, best_departure_jd = 0;
		for(int i = 0; i < results.num_deps; i++) {
			int count = get_number_of_itineraries(results.departures[i]);
			struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
			if(leaves == NULL) continue;
			int stored = 0;
			store_itineraries_in_array(results.departures[i], leaves, &stored);
			for(int j = 0; j < stored; j++) {
				double total = stored_total_dv(leaves[j], &calc_data, NULL);
				if(isfinite(total) && total < best_total) {
					best_total = total;
					best_departure_jd = get_first(leaves[j])->date;
				}
			}
			free(leaves);
		}
		printf("COVERAGE %s: best departs JD %.1f\n", label, best_departure_jd);
		printf("COVERAGE %s: %d itineraries, best %.3f m/s, %llu seeds added, %llu encounters skipped powered sampling, %llu bucket alternatives omitted\n",
			label, results.num_itins, best_total, results.powered_seed_stats.seeds_added,
			results.powered_seed_stats.searches_budget_skipped, results.powered_seed_stats.alternatives_skipped);
	}
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

int run_saved_mission_benchmarks(void) {
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

// Reruns the Jupiter mission with different powered-seed budgets to show what the default budget leaves out.
int run_coverage_report(void) {
	char system_path[] = "../Celestial_Systems/solar_system.cfg";
	CelestSystem *system = load_celestial_system_from_cfg_file(system_path);
	if(system == NULL) {
		fprintf(stderr, "Failed to load benchmark system: %s\n", system_path);
		return EXIT_FAILURE;
	}
	const char *jupiter_flybys[] = {"EARTH", "MARS", "JUPITER"};
	const int budgets[] = {64, 128, POWERED_SEEDS_PER_DEPARTURE, 2048};
	int valid = 1;
	benchmark_coverage = 1;
	for(size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
		set_powered_seed_budget(budgets[i]);
		printf("\n##### seed budget per departure: %d #####\n", budgets[i]);
		valid &= run_saved_mission_benchmark(system, "Jupiter",
			"EARTH", "JUPITER", jupiter_flybys, 3,
			2461327.50, 2461692.50, 2463325.50, 1998.0,
			6939, 6939, 6939, 190e3, 1600e3);
	}
	set_powered_seed_budget(POWERED_SEEDS_PER_DEPARTURE);
	const char *saturn_flybys[] = {"EARTH", "MARS", "JUPITER", "SATURN"};
	const double scales[] = {1.0, 1.5, 2.0};
	for(size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
		set_first_leg_window_scale(scales[i]);
		printf("\n##### first-leg window scale: %.1f #####\n", scales[i]);
		time_t started = time(NULL);
		valid &= run_saved_mission_benchmark(system, "Jupiter",
			"EARTH", "JUPITER", jupiter_flybys, 3,
			2461327.50, 2461692.50, 2463325.50, 1998.0,
			6939, 6939, 6939, 190e3, 1600e3);
		printf("TIME Jupiter scale %.1f: %.0f s\n", scales[i], difftime(time(NULL), started));
		if(scales[i] == 1.5) continue;
		started = time(NULL);
		valid &= run_saved_mission_benchmark(system, "Saturn",
			"EARTH", "SATURN", saturn_flybys, 4,
			2461326.50, 2461691.50, 2466098.50, 4407.0,
			7282, 7282, 7282, 190e3, 2050e3);
		printf("TIME Saturn scale %.1f: %.0f s\n", scales[i], difftime(time(NULL), started));
	}
	set_first_leg_window_scale(1.0);
	free_celestial_system(system);
	return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

enum FullProfileOption {
	FULL_PROFILE_RESONANT = 1u << 0,
	FULL_PROFILE_DSM = 1u << 1,
	FULL_PROFILE_MOON_AWARE = 1u << 2,
	FULL_PROFILE_MOON_ASSIST = 1u << 3,
	FULL_PROFILE_CUSTOM_ORBIT = 1u << 4
};

static double benchmark_wall_seconds(void) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return now.tv_sec + now.tv_usec / 1000000.0;
}

static int run_full_profile_configuration(CelestSystem *system, unsigned int option_mask,
		enum LastTransferType last_transfer_type) {
	double outer_start = benchmark_wall_seconds();
	Body *departure_body = get_body_by_name("Earth", system);
	Body *arrival_body = get_body_by_name("Saturn", system);
	const char *candidate_names[] = {"Earth", "Mars", "Jupiter", "Saturn"};
	Body *flyby_bodies[sizeof(candidate_names) / sizeof(candidate_names[0])];
	for(size_t i = 0; i < sizeof(candidate_names) / sizeof(candidate_names[0]); i++) {
		flyby_bodies[i] = get_body_by_name((char *)candidate_names[i], system);
		if(flyby_bodies[i] == NULL) {
			fprintf(stderr, "Profile system is missing %s\n", candidate_names[i]);
			return 0;
		}
	}
	if(departure_body == NULL || arrival_body == NULL) {
		fprintf(stderr, "Profile system is missing Earth or Saturn\n");
		return 0;
	}

	Itin_Calc_Data calc_data = {0};
	calc_data.allow_dsm = (option_mask & FULL_PROFILE_DSM) != 0;
	calc_data.moon_aware_flybys = (option_mask & FULL_PROFILE_MOON_AWARE) != 0;
	calc_data.moon_assist_arrival = (option_mask & FULL_PROFILE_MOON_ASSIST) != 0;
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
		.last_transfer_type = last_transfer_type
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET,
		.system = system,
		.dep_body = departure_body,
		.arr_body = arrival_body,
		.flyby_bodies = flyby_bodies,
		.num_flyby_bodies = (int)(sizeof(candidate_names) / sizeof(candidate_names[0])),
		.allow_resonant = (option_mask & FULL_PROFILE_RESONANT) != 0
	};
	StartingOrbit custom_orbit = starting_orbit_circular(
		departure_body->radius + departure_body->atmo_alt + 50000);
	if((option_mask & FULL_PROFILE_CUSTOM_ORBIT) != 0) {
		if(!starting_orbit_is_valid(&custom_orbit, departure_body)) {
			fprintf(stderr, "Profile custom starting orbit is invalid\n");
			return 0;
		}
		starting_orbit_set_active(&custom_orbit, departure_body);
	} else {
		starting_orbit_set_active(NULL, NULL);
	}

	printf("\nPROFILE_CONFIG|%02u|transfer=%s|resonant=%d|dsm=%d|moon_aware=%d|moon_assist=%d|custom_orbit=%d\n",
		option_mask,
		last_transfer_type == TF_CAPTURE ? "capture" : "flyby",
		(option_mask & FULL_PROFILE_RESONANT) != 0,
		calc_data.allow_dsm, calc_data.moon_aware_flybys,
		calc_data.moon_assist_arrival, (option_mask & FULL_PROFILE_CUSTOM_ORBIT) != 0);
	double calculation_start = benchmark_wall_seconds();
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	double calculation_seconds = benchmark_wall_seconds() - calculation_start;
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
	int previous_moon_assist = set_moon_assist_enabled(calc_data.moon_assist_arrival);
	for(int i = 0; i < results.num_deps; i++) {
		int count = get_number_of_itineraries(results.departures[i]);
		struct ItinStep **leaves = malloc((size_t)count * sizeof(*leaves));
		if(leaves == NULL) { valid = 0; continue; }
		int stored = 0;
		store_itineraries_in_array(results.departures[i], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			struct PorkchopPoint point = create_porkchop_point(leaves[j],
				calc_data.dv_filter.dep_periapsis, calc_data.dv_filter.arr_periapsis);
			double arrival_dv = last_transfer_type == TF_CAPTURE ? point.dv_arr_cap :
				last_transfer_type == TF_CIRC ? point.dv_arr_circ : 0;
			double satellite_dv = point.dv_dsm + point.dv_flyby + arrival_dv;
			double total = point.dv_dep + satellite_dv;
			if(leaves[j]->body != arrival_body || !isfinite(total) ||
				total > calc_data.dv_filter.max_totdv + POWERED_TEST_DV_TOLERANCE ||
				point.dv_dep > calc_data.dv_filter.max_depdv + POWERED_TEST_DV_TOLERANCE ||
				satellite_dv > calc_data.dv_filter.max_satdv + POWERED_TEST_DV_TOLERANCE ||
				point.dep_date < calc_data.jd_min_dep || point.dep_date > calc_data.jd_max_dep ||
				leaves[j]->date > calc_data.jd_max_arr || point.dur > calc_data.max_duration) {
				if(valid) fprintf(stderr, "Full-profile route violates limits: total %.1f dep %.1f sat %.1f dur %.1f\n",
					total, point.dv_dep, satellite_dv, point.dur);
				valid = 0;
			}
		}
		free(leaves);
	}
	// The porkchop analyzer builds its points from the finished search, with moon assists enabled.
	struct PorkchopPoint *array = create_porkchop_array_from_departures(results.departures, results.num_deps,
		calc_data.dv_filter.dep_periapsis, calc_data.dv_filter.arr_periapsis);
	for(int i = 0; array != NULL && i < results.num_itins; i++)
		if(isnan(array[i].dv_arr_cap) || isnan(array[i].dv_arr_circ)) valid = 0;
	free(array);
	set_moon_assist_enabled(previous_moon_assist);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	starting_orbit_set_active(NULL, NULL);
	if(!valid) {
		fprintf(stderr, "Full-profile search failed route validity/quality checks\n");
	}
	else {
		printf("UI-free full-profile search passed: %d nodes, %d itineraries\n",
			results.num_nodes, results.num_itins);
	}
	printf("MATRIX_RESULT|%02u|%d|%d|%d|%d|%d|%d|%.3f|%.3f|%d|%d|%d\n",
		option_mask,
		(option_mask & FULL_PROFILE_RESONANT) != 0,
		calc_data.allow_dsm, calc_data.moon_aware_flybys,
		calc_data.moon_assist_arrival, (option_mask & FULL_PROFILE_CUSTOM_ORBIT) != 0,
		valid, calculation_seconds, benchmark_wall_seconds() - outer_start,
		results.num_deps, results.num_nodes, results.num_itins);
	return valid;
}

static int run_full_profile_config_with_transfer(unsigned int option_mask,
		enum LastTransferType last_transfer_type) {
	if(option_mask >= (1u << 5)) {
		fprintf(stderr, "Profile option mask must be in the range 0..31\n");
		return EXIT_FAILURE;
	}
	char system_path[] = "../Celestial_Systems/solar_system_ephem.cfg";
	CelestSystem *system = load_celestial_system_from_cfg_file(system_path);
	if(system == NULL) {
		fprintf(stderr, "Failed to load profile celestial system: %s\n", system_path);
		return EXIT_FAILURE;
	}
	int valid = run_full_profile_configuration(system, option_mask, last_transfer_type);
	free_celestial_system(system);
	return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_full_profile_config(unsigned int option_mask) {
	return run_full_profile_config_with_transfer(option_mask, TF_FLYBY);
}

int run_full_profile_capture_config(unsigned int option_mask) {
	return run_full_profile_config_with_transfer(option_mask, TF_CAPTURE);
}

int run_full_profile(void) {
	return run_full_profile_config(0);
}
