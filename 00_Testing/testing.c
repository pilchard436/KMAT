#include <gtk/gtk.h>
#include "testing.h"
#include "testing_algorithms.h"
#include "tools/tool_funcs.h"
#include "tools/file_io.h"
#include "tools/thread_pool.h"


#define DEFAULT_TEST_BUDGET_SECONDS 120.0
#define FILE_TEST_BUDGET_SECONDS 60.0

// KMAT_TEST_TIME_SCALE stretches every budget, for slower machines or for recording new baselines.
static double test_time_scale(void) {
	const char *text = getenv("KMAT_TEST_TIME_SCALE");
	double scale = text != NULL ? strtod(text, NULL) : 1;
	return scale > 0 ? scale : 1;
}

// Raises the search cancel counter when a test runs past its budget, so a slow test stops instead of hanging the suite.
struct TestWatchdog {
	gint64 deadline_us;
	volatile gint finished, expired;
};

static gpointer test_watchdog_main(gpointer data) {
	struct TestWatchdog *watchdog = data;
	while(!g_atomic_int_get(&watchdog->finished)) {
		if(g_get_monotonic_time() >= watchdog->deadline_us) {
			g_atomic_int_set(&watchdog->expired, 1);
			incr_thread_counter_by_amount(3, 1);
			break;
		}
		g_usleep(50000);
	}
	return NULL;
}

int run_tests() {
	int num_tests = 0;
	int failed_tests = 0;
	const int max_num_tests = 1000;
	union TestData test_data[max_num_tests];


	// ITINERARY CALCULATOR TEST DATA #################################################################################

	// Kerbin -> Eeloo; 1-001 | 2-001 | 3000; dv_dep = 1800, dv_arr = 1800 (circ) | Bodies: Eve, Kerbin, Duna, Eeloo
	test_data[num_tests] = (union TestData) (struct Itin_To_Target_Calc_Test) {
			TEST_ITIN_TO_TARGET,
			(struct Datetime) {.y = 1, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 2, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 100, .d = 1, .date_type = DATE_KERBAL},
			3000,
			1800, 1800, 1e9, TF_CIRC,
			"Stock System",
			4,
			(char *[]){"Eve", "Kerbin", "Duna", "Eeloo"},
			"Kerbin",
			"Eeloo",
			805, 249, // expected num_nodes and num_itins
			60, 2992, 2 // time budget (s), reference best total dv (m/s), minimum distinct body sequences
	}; num_tests++;

	// Kerbin -> Kerbin; 1-001 | 2-001 | 1000; dv_dep = 1800 | Bodies: Eve, Kerbin, Duna, Jool
	test_data[num_tests] = (union TestData) (struct Itin_To_Target_Calc_Test) {
			TEST_ITIN_TO_TARGET,
			(struct Datetime) {.y = 1, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 2, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 100, .d = 1, .date_type = DATE_KERBAL},
			1000,
			1800, 1e9, 1e9, TF_FLYBY,
			"Stock System",
			4,
			(char *[]){"Eve", "Kerbin", "Duna", "Jool"},
			"Kerbin",
			"Kerbin",
			35538, 17728, // expected num_nodes and num_itins
			180, 1296, 45 // time budget (s), reference best total dv (m/s), minimum distinct body sequences
	}; num_tests++;

	// Earth -> Earth; 1959-01-01 | 1959-03-01 | 1000; dv_dep = 5000 | Bodies: Mercury, Venus, Earth
	test_data[num_tests] = (union TestData) (struct Itin_To_Target_Calc_Test) {
			TEST_ITIN_TO_TARGET,
			(struct Datetime) {.y = 1959, .m = 1, .d = 1, .date_type = DATE_ISO},
			(struct Datetime) {.y = 1959, .m = 3, .d = 1, .date_type = DATE_ISO},
			(struct Datetime) {.y = 2000, .m = 1, .d = 1, .date_type = DATE_ISO},
			1000,
			5000, 1e9, 1e9, TF_FLYBY,
			"Solar System (Ephemeris)",
			3,
			(char *[]){"Mercury", "Venus", "Earth"},
			"Earth",
			"Earth",
			17335, 8412, // expected num_nodes and num_itins
			270, 4379, 18 // time budget (s), reference best total dv (m/s), minimum distinct body sequences (routes hold at most MAX_FLYBYS_PER_ROUTE flybys)
	}; num_tests++;

	// Earth -> Jupiter; 1967-01-01 | 1967-03-01 | 3000; dv_dep = 4500 | Bodies: Venus, Earth, Mars, Jupiter
	test_data[num_tests] = (union TestData) (struct Itin_To_Target_Calc_Test) {
			TEST_ITIN_TO_TARGET,
			(struct Datetime) {.y = 1967, .m = 1, .d = 1, .date_type = DATE_ISO},
			(struct Datetime) {.y = 1967, .m = 3, .d = 1, .date_type = DATE_ISO},
			(struct Datetime) {.y = 2000, .m = 1, .d = 1, .date_type = DATE_ISO},
			3000,
			4500, 1e9, 1e9, TF_FLYBY,
			"Solar System (Ephemeris)",
			4,
			(char *[]){"Venus", "Earth", "Mars", "Jupiter"},
			"Earth",
			"Jupiter",
			19237, 8539, // expected num_nodes and num_itins
			840, 7697, 140 // time budget (s), reference best total dv (m/s), minimum distinct body sequences (routes hold at most MAX_FLYBYS_PER_ROUTE flybys)
	}; num_tests++;


	// SEQUENCE CALCULATOR TEST DATA #################################################################################

	// Kerbin -> Eve; 2-001 | 3-001 | 300; dv_dep = 1200; dv_arr = 1500 (circ)
	test_data[num_tests] = (union TestData) (struct Itin_Spec_Seq_Calc_Test) {
			TEST_ITIN_SPEC_SEQ,
			(struct Datetime) {.y = 2, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 3, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 100, .d = 1, .date_type = DATE_KERBAL},
			300,
			1200, 1500, 1e9, TF_CIRC,
			"Stock System",
			2,
			(char *[]){"Kerbin", "Eve"},
			0, 0 // expected num_nodes and num_itins
	}; num_tests++;

	// Kerbin -> Eve -> Duna -> Kerbin; 2-001 | 3-001 | 1000; dv_dep = 1500
	test_data[num_tests] = (union TestData) (struct Itin_Spec_Seq_Calc_Test) {
			TEST_ITIN_SPEC_SEQ,
			(struct Datetime) {.y = 2, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 3, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 100, .d = 1, .date_type = DATE_KERBAL},
			1000,
			1500, 1e9, 1e9, TF_FLYBY,
			"Stock System",
			4,
			(char *[]){"Kerbin", "Eve", "Duna", "Kerbin"},
			62982, 23791, // expected num_nodes and num_itins
			40, 1246, 1 // time budget (s), reference best total dv (m/s), minimum distinct body sequences
	}; num_tests++;

	// Earth -> Mars; 1950-01-01 | 1951-06-01 | 1950-11-01 | 600; dv_dep = 5000; dv_arr = 4000 (circ); dv_tot = 8200
	test_data[num_tests] = (union TestData) (struct Itin_Spec_Seq_Calc_Test) {
			TEST_ITIN_SPEC_SEQ,
			(struct Datetime) {.y = 1950, .m = 1, .d = 1, .date_type = DATE_ISO},
			(struct Datetime) {.y = 1950, .m = 6, .d = 1, .date_type = DATE_ISO},
			(struct Datetime) {.y = 1950, .m =11, .d = 1, .date_type = DATE_ISO},
			600,
			5000, 4000, 8200, TF_CIRC,
			"Solar System (Ephemeris)",
			2,
			(char *[]){"Earth", "Mars"},
			0, 0 // expected num_nodes and num_itins
	}; num_tests++;

	// Earth -> Jupiter -> Saturn -> Uranus -> Neptune; 1977-01-01 | 1978-01-01 | 8000; dv_dep = 7200
	test_data[num_tests] = (union TestData) (struct Itin_Spec_Seq_Calc_Test) {
			TEST_ITIN_SPEC_SEQ,
			(struct Datetime) {.y = 1977, .m = 1, .d = 1, .date_type = DATE_ISO},
			(struct Datetime) {.y = 1978, .m = 1, .d = 1, .date_type = DATE_ISO},
			(struct Datetime) {.y = 2000, .m = 1, .d = 1, .date_type = DATE_ISO},
			8000,
			7200, 1e9, 1e9, TF_FLYBY,
			"Solar System (Ephemeris)",
			5,
			(char *[]){"Earth", "Jupiter", "Saturn", "Uranus", "Neptune"},
			97625, 31408, // expected num_nodes and num_itins
			12, 6704, 1 // time budget (s), reference best total dv (m/s), minimum distinct body sequences
	}; num_tests++;

	// Kerbin -> Kerbin -> Jool (a resonant return to the departure body); 2-001 | 3-001 | 4000; dv_dep = 2500
	test_data[num_tests] = (union TestData) (struct Itin_Spec_Seq_Calc_Test) {
			TEST_ITIN_SPEC_SEQ,
			(struct Datetime) {.y = 2, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 3, .d = 1, .date_type = DATE_KERBAL},
			(struct Datetime) {.y = 100, .d = 1, .date_type = DATE_KERBAL},
			4000,
			2500, 1e9, 1e9, TF_FLYBY,
			"Stock System",
			3,
			(char *[]){"Kerbin", "Kerbin", "Jool"},
			6273, 3435, // expected num_nodes and num_itins
			60, 1994, 1 // time budget (s), reference best total dv (m/s), minimum distinct body sequences
	}; num_tests++;



	// .ITINS-FILE TEST DATA #################################################################################

	char *itins_directory = "../00_Testing/Itineraries/";
	create_directory_if_not_exists(itins_directory);
	if(!create_file_test_fixtures(itins_directory)) printf("WARNING: no .itins/.itin fixtures were created\n");
	GDir *dir = g_dir_open(itins_directory, 0, NULL);
	if (!dir) {
		g_printerr("Unable to open directory: %s\n", itins_directory);
		return 1;
	}
	const gchar *filename;
	while ((filename = g_dir_read_name(dir)) != NULL) {
		if (g_str_has_suffix(filename, ".itins")) {
			char *filepath = malloc(100 * sizeof(char));
			sprintf(filepath, "%s%s", itins_directory, filename);
			test_data[num_tests] = (union TestData) (struct Itins_File_Test) {
					TEST_ITINS_FILE,
					filepath
			}; num_tests++;
		}
	}
	g_dir_close(dir);


	// .ITIN-FILE TEST DATA #################################################################################

	char *itin_directory = "../00_Testing/Itineraries/";
	create_directory_if_not_exists(itin_directory);
	dir = g_dir_open(itin_directory, 0, NULL);
	if (!dir) {
		g_printerr("Unable to open directory: %s\n", itin_directory);
		return 1;
	}
	while ((filename = g_dir_read_name(dir)) != NULL) {
		if (g_str_has_suffix(filename, ".itin")) {
			char *filepath = malloc(100 * sizeof(char));
			sprintf(filepath, "%s%s", itin_directory, filename);
			test_data[num_tests] = (union TestData) (struct Itin_File_Test) {
					TEST_ITIN_FILE,
					filepath
			}; num_tests++;
		}
	}




	// TEST RESULTS #################################################################################

	enum TestResult test_results[max_num_tests];

	double test_seconds[max_num_tests], test_budgets[max_num_tests];
	for(int i = 0; i < num_tests; i++) {
		printf("###################### TEST %02d/%02d ######################\n", i+1, num_tests);
		double budget = FILE_TEST_BUDGET_SECONDS;
		if(test_data[i].itin_to_target_test.test_type == TEST_ITIN_TO_TARGET)
			budget = test_data[i].itin_to_target_test.time_budget_seconds > 0 ?
				test_data[i].itin_to_target_test.time_budget_seconds : DEFAULT_TEST_BUDGET_SECONDS;
		if(test_data[i].itin_to_target_test.test_type == TEST_ITIN_SPEC_SEQ)
			budget = test_data[i].itin_spec_seq_calc_test.time_budget_seconds > 0 ?
				test_data[i].itin_spec_seq_calc_test.time_budget_seconds : DEFAULT_TEST_BUDGET_SECONDS;
		budget *= test_time_scale();
		struct TestWatchdog watchdog = {.deadline_us = g_get_monotonic_time() + (gint64)(budget * 1e6)};
		gint64 test_start_us = g_get_monotonic_time();
		GThread *watchdog_thread = g_thread_new("test-watchdog", test_watchdog_main, &watchdog);
		switch(test_data[i].itin_to_target_test.test_type) {
			case TEST_ITIN_TO_TARGET:
				test_results[i] = test_itinerary_calculator(test_data[i].itin_to_target_test);
				break;
			case TEST_ITIN_SPEC_SEQ:
				test_results[i] = test_sequence_calculator(test_data[i].itin_spec_seq_calc_test);
				break;
			case TEST_ITINS_FILE:
				test_results[i] = test_itins_file(test_data[i].itins_file_test.filepath);
				free(test_data[i].itins_file_test.filepath);
				break;
			case TEST_ITIN_FILE:
				test_results[i] = test_itin_file(test_data[i].itin_file_test.filepath);
				free(test_data[i].itin_file_test.filepath);
				break;
			default:
				break;
		}
		g_atomic_int_set(&watchdog.finished, 1);
		g_thread_join(watchdog_thread);
		test_seconds[i] = (g_get_monotonic_time() - test_start_us) / 1e6;
		test_budgets[i] = budget;
		if(g_atomic_int_get(&watchdog.expired)) {
			incr_thread_counter_by_amount(3, -get_thread_counter(3));
			test_results[i] = TEST_FAIL_TIME_BUDGET_EXCEEDED;
		}
		printf("Test time: %.1f s (budget %.0f s)\n", test_seconds[i], budget);
		printf("------------------------------------------------------------\n\n");
	}


	int executed_tests = num_tests;
	int warning_tests = 0;
	printf("\n\n##################### TEST RESULTS #####################\n");
	for(int i = 0; i < num_tests; i++) {
		printf("TEST %02d: [%7.1f s] ", i+1, test_seconds[i]);
		if(test_results[i] == TEST_WARN_DIFF_EXPECTED_NUM_NODES_OR_ITINS) {
			warning_tests++;
		} else if(test_results[i] != TEST_PASSED) {
			failed_tests++;
		}
		switch(test_results[i]) {
			case TEST_PASSED:
				printf("PASS\n");
				break;
			case TEST_FAIL_CELESTIAL_SYSTEM_NOT_FOUND:
				printf("FAIL  |  Celestial system not found\n");
				break;
			case TEST_FAIL_CELESTIAL_BODY_NOT_FOUND:
				printf("FAIL  |  Celestial body not found\n");
				break;
			case TEST_FAIL_CONFIGURATION_ERROR:
				printf("FAIL  |  Configuration error\n");
				break;
			case TEST_WARN_DIFF_EXPECTED_NUM_NODES_OR_ITINS:
				printf("WARN  |  Number of nodes and/or number of itineraries differ from expectation\n");
				break;
			case TEST_FAIL_ITINERARY_NOT_INSIDE_DV_FILTER:
				printf("FAIL  |  Itinerary not inside dv-filter\n");
				break;
			case TEST_FAIL_ITINERARY_HAS_INVALID_CELESTIAL_BODY:
				printf("FAIL  |  Itinerary has invalid celestial body\n");
				break;
			case TEST_FAIL_ITINERARY_OUTSIDE_TIME_CONSTRAINTS:
				printf("FAIL  |  Itinerary is outside time constraint\n");
				break;
			case TEST_FAIL_INVALID_ITINERARY:
				printf("FAIL  |  Has invalid itinerary\n");
				break;
			case TEST_FAIL_TIME_BUDGET_EXCEEDED:
				printf("FAIL  |  Time budget of %.0f s exceeded\n", test_budgets[i]);
				break;
			case TEST_FAIL_BEST_DV_REGRESSION:
				printf("FAIL  |  Best total dv is above the reference\n");
				break;
			case TEST_FAIL_TOO_FEW_DISTINCT_ROUTES:
				printf("FAIL  |  Too few distinct body sequences\n");
				break;
			default:
				printf("\n");
		}
	}
	printf("--------------------------------------------------------\n\n");
	printf("KMAT integration tests: %d total, %d passed, %d warnings, %d failed.\n",
		   executed_tests, executed_tests - failed_tests - warning_tests, warning_tests, failed_tests);

	return failed_tests;
}