#ifndef KMAT_POWERED_FLYBY_OPTIMIZER_H
#define KMAT_POWERED_FLYBY_OPTIMIZER_H

#include "orbit_calculator/transfer_calc.h"

typedef struct PoweredFlybyOptimizationStats {
	int routes_considered;
	int routes_valid;
	int routes_corrected;
	int routes_improved;
	double best_delta_v_saving;
} PoweredFlybyOptimizationStats;

int optimize_powered_flyby_routes(struct ItinStep **departures, int num_departures,
	const Itin_Calc_Data *calc_data, int max_routes, PoweredFlybyOptimizationStats *stats);

#endif
