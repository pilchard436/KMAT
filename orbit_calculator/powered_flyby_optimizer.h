#ifndef KMAT_POWERED_FLYBY_OPTIMIZER_H
#define KMAT_POWERED_FLYBY_OPTIMIZER_H

#include "orbit_calculator/transfer_calc.h"
#include <stddef.h>

#define POWERED_ROUTE_REFINEMENT_LIMIT 500

typedef struct PoweredFlybyOptimizationStats {
	int routes_considered;
	int routes_valid;
	int routes_corrected;
	int routes_improved;
	double best_delta_v_saving;
	double best_initial_total_dv;
	double best_refined_total_dv;
} PoweredFlybyOptimizationStats;

int optimize_powered_flyby_routes(struct ItinStep **departures, int num_departures,
	const Itin_Calc_Data *calc_data, int max_routes, PoweredFlybyOptimizationStats *stats);

double evaluate_powered_flyby_route_objective(const Itin_Calc_Data *calc_data,
	Body **bodies, const double *times, size_t num_steps);

#endif
