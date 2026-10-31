#include "integration_common.h"
#include "tools/thread_pool.h"

int benchmark_resonant;
int benchmark_unpowered;
int benchmark_coverage;

int main(int argc, char **argv) {
	if(argc == 4 && strcmp(argv[1], "--full-profile-worker-scale") == 0) {
		char *worker_end;
		char *mask_end;
		unsigned long worker_limit = strtoul(argv[2], &worker_end, 10);
		unsigned long option_mask = strtoul(argv[3], &mask_end, 10);
		if(worker_end == argv[2] || *worker_end != '\0' || worker_limit == 0 ||
			worker_limit > THREAD_POOL_MAX_WORKERS || mask_end == argv[3] ||
			*mask_end != '\0' || option_mask >= (1u << 5)) {
			fprintf(stderr, "Worker scale expects a worker limit in 1..%d and profile mask in 0..31\n",
				THREAD_POOL_MAX_WORKERS);
			return EXIT_FAILURE;
		}
		set_thread_pool_worker_limit((size_t)worker_limit);
		fprintf(stderr, "Worker scale: requested %lu, effective capacity %zu\n",
			worker_limit, get_thread_pool32_worker_capacity());
		return run_full_profile_config((unsigned int)option_mask);
	}
	// Trailing modifiers, in either order: --resonant (mission benchmarks) and --unpowered (powered flybys off).
	while(argc > 1 && (strcmp(argv[argc - 1], "--resonant") == 0 || strcmp(argv[argc - 1], "--unpowered") == 0)) {
		if(strcmp(argv[argc - 1], "--resonant") == 0) benchmark_resonant = 1;
		else benchmark_unpowered = 1;
		argc--;
	}
	if(argc >= 3 && argc <= 6 && strcmp(argv[1], "--replay-itins") == 0)
		return run_itins_replay(argv[2], argc > 3 ? atof(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0,
			argc > 5 ? atoi(argv[5]) : 0);
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
	if(argc == 3 && strcmp(argv[1], "--full-profile-both-config") == 0) {
		char *end;
		unsigned long option_mask = strtoul(argv[2], &end, 10);
		if(end == argv[2] || *end != '\0' || option_mask >= (1u << 5)) {
			fprintf(stderr, "Profile option mask must be in the range 0..31\n");
			return EXIT_FAILURE;
		}
		return run_full_profile_both_config((unsigned int)option_mask);
	}
	if(argc != 1) {
		fprintf(stderr, "Usage: %s [--full-profile|--full-profile-config <0..31>|--full-profile-capture-config <0..31>|--full-profile-both-config <0..31>|--full-profile-worker-scale <workers> <0..31>|--mission-benchmarks|--coverage|--coverage-reference|--coverage-multi] [--resonant] [--unpowered]\n", argv[0]);
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
	int limit_valid = check_no_seed_at_search_limit(system, departure_body, flyby_body,
		arrival_body, departure_jd);
	int unpowered_valid = check_unpowered_flyby_search(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int starting_valid = check_starting_orbit_search(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int dsm_model_valid = check_dsm_model(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int dsm_valid = run_dsm_comparison(system, departure_body, flyby_body,
		arrival_body, departure_jd, hohmann_duration);
	int sweep_valid = check_departure_sweep_fixture();
	int resonant_valid = check_resonant_flybys(system, departure_body, flyby_body, arrival_body,
		departure_jd, hohmann_duration);
	int route_limits_valid = check_route_limits_search(system, departure_body, flyby_body, arrival_body,
		departure_jd, hohmann_duration);
	int near_equal_valid = check_near_equal_speed_flybys(departure_body);
	int empty_reason_valid = check_empty_result_reason(system, departure_body, arrival_body, departure_jd,
		max_arrival_jd);

	free_celestial_system(system);
	if(!search_valid || !powered_valid || !seeds_valid || !limit_valid || !unpowered_valid || !starting_valid || !dsm_model_valid || !dsm_valid ||
		!sweep_valid || !resonant_valid || !route_limits_valid || !near_equal_valid || !empty_reason_valid) {
		fprintf(stderr, "Integration checks failed (bounded search: %d nodes/%d itineraries)\n",
			search_nodes, search_itins);
		return EXIT_FAILURE;
	}
	printf("UI-free itinerary integration passed: %d nodes, %d itineraries\n",
		search_nodes, search_itins);
	return EXIT_SUCCESS;
}
