#ifndef KMAT_INTEGRATION_COMMON_H
#define KMAT_INTEGRATION_COMMON_H

/* Shared declarations of the UI-free itinerary search integration checks. */

#include "orbit_calculator/transfer_calc.h"
#include "orbit_calculator/powered_flyby_optimizer.h"
#include "tools/file_io.h"
#include "orbitlib_fileio.h"
#include "orbitlib_transfer.h"
#include "tools/starting_orbit.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POWERED_TEST_DV_TOLERANCE 0.1
#define POWERED_TEST_MIN_LEG_DAYS 10.0
#define POWERED_TEST_GRID_RELATIVE_TOLERANCE 0.10

extern int benchmark_resonant;
extern int benchmark_unpowered;

int route_is_resonant(struct ItinStep *leaf);
int check_speed_changing_seed_generation(CelestSystem *system, Body *departure, Body *flyby, Body *target, double departure_jd);
int check_unpowered_flyby_search(CelestSystem *system, Body *departure, Body *flyby, Body *target, double departure_jd, double hohmann_duration);
int run_bounded_search(CelestSystem *system, Body *departure_body, Body *arrival_body, double min_departure_jd, double max_arrival_jd, int *num_nodes, int *num_itins);
int run_powered_flyby_objective_regression(CelestSystem *system, Body *departure_body, Body *flyby_body, Body *arrival_body, double departure_jd, double hohmann_duration);
double stored_total_dv(struct ItinStep *leaf, const Itin_Calc_Data *calc_data, double *dsm_dv);
double plain_cost_without_maneuvers(struct ItinStep *leaf, const Itin_Calc_Data *calc_data);
void summarize_itineraries(struct ItinStep **departures, int num_departures, const Itin_Calc_Data *calc_data, int *routes, int *maneuvers, double *best_total);
int check_departure_sweep_fixture(void);
int check_empty_result_reason(CelestSystem *system, Body *departure_body, Body *arrival_body, double departure_jd, double max_arrival_jd);
int check_starting_orbit_search(CelestSystem *system, Body *departure_body, Body *flyby_body, Body *arrival_body, double departure_jd, double hohmann_duration);
int run_saved_mission_benchmarks(void);
int run_itins_replay(const char *path, double max_duration, int num_departures, int max_flybys);
int run_full_profile(void);
int run_full_profile_config(unsigned int option_mask);
int run_full_profile_capture_config(unsigned int option_mask);
int run_full_profile_both_config(unsigned int option_mask);
double score_powered_arrival_route(struct ItinStep *arrival, const Itin_Calc_Data *calc_data, Body *flyby_body);
int run_coverage_report(void);
int run_coverage_reference(CelestSystem *system, Body *departure_body, Body *flyby_body,
	Body *arrival_body, double departure_jd, double hohmann_duration);
int run_multi_flyby_coverage(CelestSystem *system, Body *departure_body, Body *venus,
	Body *arrival_body, double departure_jd, double hohmann_duration);
extern int benchmark_coverage;
int check_itinerary_operations(struct ItinStep **departures, int num_departures, CelestSystem *system);
int run_dsm_comparison(CelestSystem *system, Body *departure_body, Body *flyby_body, Body *arrival_body, double departure_jd, double hohmann_duration);
int check_dsm_model(CelestSystem *system, Body *departure_body, Body *flyby_body, Body *arrival_body, double departure_jd, double hohmann_duration);
int check_resonant_flybys(CelestSystem *system, Body *departure_body, Body *flyby_body, Body *arrival_body, double departure_jd, double hohmann_duration);
int check_route_limits_search(CelestSystem *system, Body *departure_body, Body *flyby_body, Body *arrival_body, double departure_jd, double hohmann_duration);
int check_near_equal_speed_flybys(Body *body);

#endif
