#ifndef KSP_TRANSFER_CALC_H
#define KSP_TRANSFER_CALC_H

#include "itin_tool.h"
#include "tools/celestial_systems.h"
#include "tools/starting_orbit.h"
#include "search_rejections.h"

typedef struct ItinSearchOptions {
	int known;
	int allow_dsm;
	int moon_aware_flybys;
	int moon_assist_arrival;
	int allow_resonant;
	int last_transfer_type;
	int use_custom_starting_orbit;
	int starting_orbit_body_id;
	StartingOrbit starting_orbit;
	// Zero (the default) keeps powered flybys on, so older files and zero-initialized settings behave as before.
	int disable_powered_flybys;
} ItinSearchOptions;


typedef struct Itin_Calc_Data {
	double jd_min_dep;
	double jd_max_dep;
	double jd_max_arr;
	double max_duration;
	int allow_dsm;
	int moon_aware_flybys;
	int moon_assist_arrival;
	/* Search only unpowered gravity assists: no powered-flyby seeds, no route optimization (which also runs the
	 * deep-space maneuver pass, so allow_dsm has no effect). */
	int disable_powered_flybys;
	double step_dep_date;
	int num_deps_per_date;
	int max_num_waiting_orbits;
	struct Dv_Filter dv_filter;
	struct Calc_Acc calc_acc;
	ItinSequenceInfo seq_info;
	ItinSearchOptions search_options;
} Itin_Calc_Data;

int get_num_departure_dates(double jd_min_dep, double jd_max_dep, double step_dep_date);

typedef struct Itin_Calc_Results {
	struct ItinStep **departures;
	int num_deps, num_nodes, num_itins;
	struct ItinStep **capture_departures;
	int capture_num_deps, capture_num_nodes, capture_num_itins;
	int capture_calculation_failed;
	int powered_routes_valid, powered_routes_corrected;
	int powered_itineraries_pruned;
	unsigned long long powered_initial_rejections;
	int powered_repairs_attempted, powered_repairs_recovered, powered_repairs_competing;
	int powered_challenges_refined;
	int powered_dsm_routes_optimized, powered_dsm_routes_improved;
	double powered_best_dsm_saving;
	double best_unpowered_seed_total_dv;
	double best_powered_initial_total_dv;
	double best_powered_refined_total_dv;
	PoweredSeedSearchStats powered_seed_stats;
	Itin_Rejection_Counts rejections;
} Itin_Calc_Results;

typedef enum Transfer_Progress_Phase {
	TRANSFER_PROGRESS_SEARCH,
	TRANSFER_PROGRESS_PREPARING,
	TRANSFER_PROGRESS_PREPARING_CAPTURE,
	TRANSFER_PROGRESS_SCORING,
	TRANSFER_PROGRESS_REPAIRING,
	TRANSFER_PROGRESS_REFINING,
	TRANSFER_PROGRESS_OPTIMIZING_BOTH,
	TRANSFER_PROGRESS_FILTERING,
	TRANSFER_PROGRESS_FINALIZING
} Transfer_Progress_Phase;

typedef struct Transfer_Calc_Status {
	int num_itins;
	int num_deps;
	int num_deps_started;
	int num_deps_active;
	int total_departures;
	Transfer_Progress_Phase phase;
	int phase_done;
	int phase_total;
	int phase_transfer_type;
	double progress;
} Transfer_Calc_Status;

Itin_Calc_Results search_for_itineraries(Itin_Calc_Data calc_data);
Itin_Calc_Results search_for_itineraries_both_modes(Itin_Calc_Data calc_data);

Transfer_Calc_Status get_current_transfer_calc_status();

#endif //KSP_TRANSFER_CALC_H

void set_first_leg_window_scale(double scale);
