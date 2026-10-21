#include "integration_common.h"

int benchmark_resonant;
int benchmark_coverage;

int main(int argc, char **argv) {
	if(argc > 1 && strcmp(argv[argc - 1], "--resonant") == 0) {
		benchmark_resonant = 1;
		argc--;
	}
	if(argc == 2 && strcmp(argv[1], "--mission-benchmarks") == 0)
		return run_saved_mission_benchmarks();
	if(argc == 2 && strcmp(argv[1], "--coverage") == 0)
		return run_coverage_report();
	int coverage_reference = argc == 2 && strcmp(argv[1], "--coverage-reference") == 0;
	int coverage_multi = argc == 2 && strcmp(argv[1], "--coverage-multi") == 0;
	if(coverage_reference || coverage_multi) argc = 1;
	if(argc == 2 && strcmp(argv[1], "--full-profile") == 0)
		return run_full_profile();
	if(argc == 3 && strcmp(argv[1], "--full-profile-config") == 0) {
		char *end;
		unsigned long option_mask = strtoul(argv[2], &end, 10);
		if(end == argv[2] || *end != '\0' || option_mask >= (1u << 5)) {
			fprintf(stderr, "Profile option mask must be in the range 0..31\n");
			return EXIT_FAILURE;
		}
		return run_full_profile_config((unsigned int)option_mask);
	}
	if(argc == 3 && strcmp(argv[1], "--full-profile-capture-config") == 0) {
		char *end;
		unsigned long option_mask = strtoul(argv[2], &end, 10);
		if(end == argv[2] || *end != '\0' || option_mask >= (1u << 5)) {
			fprintf(stderr, "Profile option mask must be in the range 0..31\n");
			return EXIT_FAILURE;
		}
		return run_full_profile_capture_config((unsigned int)option_mask);
	}
	if(argc != 1) {
		fprintf(stderr, "Usage: %s [--full-profile|--full-profile-config <0..31>|--full-profile-capture-config <0..31>|--mission-benchmarks|--coverage|--coverage-reference|--coverage-multi]\n", argv[0]);
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
	if(coverage_reference || coverage_multi) {
		// Best Earth-Jupiter departure of the saved mission window, so the reference is a realistic launch.
		departure_jd = 2461369.5;
		departure_state = osv_from_elements(departure_body->orbit, departure_jd);
		arrival_state = osv_from_elements(arrival_body->orbit, departure_jd);
		hohmann_duration = calc_hohmann_transfer(
			mag_vec3(departure_state.r), mag_vec3(arrival_state.r), system->cb).dur / 86400;
		int coverage_result = coverage_multi ? EXIT_SUCCESS : run_coverage_reference(system,
			departure_body, flyby_body, arrival_body, departure_jd, hohmann_duration);
		if(coverage_result == EXIT_SUCCESS)
			coverage_result = run_multi_flyby_coverage(system, departure_body, flyby_body,
				arrival_body, departure_jd, hohmann_duration);
		free_celestial_system(system);
		return coverage_result;
	}
	int search_nodes, search_itins;

	int search_valid = run_bounded_search(system, departure_body, arrival_body, departure_jd,
		max_arrival_jd, &search_nodes, &search_itins);
	int powered_valid = run_powered_flyby_objective_regression(system, departure_body,
		flyby_body, arrival_body, departure_jd, hohmann_duration);
	int seeds_valid = check_speed_changing_seed_generation(system, departure_body,
		flyby_body, arrival_body, departure_jd);
	int starting_valid = check_starting_orbit_search(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int dsm_model_valid = check_dsm_model(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int dsm_valid = run_dsm_comparison(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int sweep_valid = check_departure_sweep_fixture();
	int resonant_valid = check_resonant_flybys(system, departure_body, flyby_body, arrival_body,
		departure_jd, hohmann_duration);
	int near_equal_valid = check_near_equal_speed_flybys(departure_body);
	int empty_reason_valid = check_empty_result_reason(system, departure_body, arrival_body, departure_jd,
		max_arrival_jd);

	free_celestial_system(system);
	if(!search_valid || !powered_valid || !seeds_valid || !starting_valid || !dsm_model_valid || !dsm_valid ||
		!sweep_valid || !resonant_valid || !near_equal_valid || !empty_reason_valid) {
		fprintf(stderr, "Integration checks failed (bounded search: %d nodes/%d itineraries)\n",
			search_nodes, search_itins);
		return EXIT_FAILURE;
	}
	printf("UI-free itinerary integration passed: %d nodes, %d itineraries\n",
		search_nodes, search_itins);
	return EXIT_SUCCESS;
}
