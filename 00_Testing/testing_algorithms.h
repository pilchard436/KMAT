#ifndef KSP_TESTING_ALGORITHMS_H
#define KSP_TESTING_ALGORITHMS_H


#include "orbit_calculator/itin_tool.h"


enum TestType {TEST_ITIN_TO_TARGET, TEST_ITIN_SPEC_SEQ, TEST_ITINS_FILE, TEST_ITIN_FILE};

enum TestResult {
	TEST_PASSED,
	TEST_FAIL_CELESTIAL_SYSTEM_NOT_FOUND,
	TEST_FAIL_CELESTIAL_BODY_NOT_FOUND,
	TEST_FAIL_CONFIGURATION_ERROR,
	TEST_WARN_DIFF_EXPECTED_NUM_NODES_OR_ITINS,
	TEST_FAIL_ITINERARY_NOT_INSIDE_DV_FILTER,
	TEST_FAIL_ITINERARY_HAS_INVALID_CELESTIAL_BODY,
	TEST_FAIL_ITINERARY_OUTSIDE_TIME_CONSTRAINTS,
	TEST_FAIL_INVALID_ITINERARY,
	TEST_FAIL_TIME_BUDGET_EXCEEDED,
	TEST_FAIL_BEST_DV_REGRESSION,
	TEST_FAIL_TOO_FEW_DISTINCT_ROUTES,
};

union TestData { ;
	struct Itin_To_Target_Calc_Test {
		enum TestType test_type;
		struct Datetime min_dep_date, max_dep_date, max_arr_date;
		int max_duration;
		double max_depdv, max_satdv, max_totdv;
		enum LastTransferType last_transfer_type;
		char *system_name;
		int num_flyby_bodies;
		char **flyby_bodies_names;
		char *dep_body_name;
		char *arr_body_name;
		int expected_num_nodes, expected_num_itins;
		// Optional (0 = default or skipped): seconds the test may take, best total delta-v it must not exceed by
		// more than 1 percent, and the number of different body sequences the result must contain.
		double time_budget_seconds, reference_best_dv;
		int min_distinct_sequences;
	} itin_to_target_test;

	struct Itin_Spec_Seq_Calc_Test {
		enum TestType test_type;
		struct Datetime min_dep_date, max_dep_date, max_arr_date;
		int max_duration;
		double max_depdv, max_satdv, max_totdv;
		enum LastTransferType last_transfer_type;
		char *system_name;
		int num_steps;
		char **body_names;
		int expected_num_nodes, expected_num_itins;
		// Optional (0 = default or skipped): seconds the test may take, best total delta-v it must not exceed by
		// more than 1 percent, and the number of different body sequences the result must contain.
		double time_budget_seconds, reference_best_dv;
		int min_distinct_sequences;
	} itin_spec_seq_calc_test;

	struct Itins_File_Test {
		enum TestType test_type;
		char *filepath;
	} itins_file_test;

	struct Itin_File_Test {
		enum TestType test_type;
		char *filepath;
	} itin_file_test;
};

enum TestResult test_itinerary_calculator(struct Itin_To_Target_Calc_Test test_data);

enum TestResult test_sequence_calculator(struct Itin_Spec_Seq_Calc_Test test_data);

// Runs a small search and stores its result as one .itins and one .itin file in the directory, so the file tests
// always have something to load. Returns 1 on success.
int create_file_test_fixtures(const char *directory);

enum TestResult test_itins_file(char *filepath);

enum TestResult test_itin_file(char *filepath);

#endif //KSP_TESTING_ALGORITHMS_H
