#ifndef KSP_TRANSFER_CALC_H
#define KSP_TRANSFER_CALC_H

#include "itin_tool.h"
#include "tools/celestial_systems.h"


typedef struct Itin_Calc_Data {
	double jd_min_dep;
	double jd_max_dep;
	double jd_max_arr;
	double max_duration;
	double step_dep_date;
	int num_deps_per_date;
	int max_num_waiting_orbits;
	struct Dv_Filter dv_filter;
	struct Calc_Acc calc_acc;
	ItinSequenceInfo seq_info;
} Itin_Calc_Data;

typedef struct Itin_Calc_Results {
	struct ItinStep **departures;
	int num_deps, num_nodes, num_itins;
	int powered_routes_valid, powered_routes_corrected;
	int powered_itineraries_pruned;
	unsigned long long powered_initial_rejections;
	int powered_repairs_attempted, powered_repairs_recovered, powered_repairs_competing;
	int powered_challenges_refined;
	double best_unpowered_seed_total_dv;
	double best_powered_initial_total_dv;
	double best_powered_refined_total_dv;
	PoweredSeedSearchStats powered_seed_stats;
} Itin_Calc_Results;

typedef struct Transfer_Calc_Status {
	int num_itins;
	int num_deps;
	int num_deps_started;
	int num_deps_active;
	double jd_diff;
	double progress;
} Transfer_Calc_Status;

Itin_Calc_Results search_for_itineraries(Itin_Calc_Data calc_data);

Transfer_Calc_Status get_current_transfer_calc_status();

#endif //KSP_TRANSFER_CALC_H
