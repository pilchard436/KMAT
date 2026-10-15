#ifndef KMAT_POWERED_FLYBY_OPTIMIZER_H
#define KMAT_POWERED_FLYBY_OPTIMIZER_H

#include "orbit_calculator/transfer_calc.h"
#include <stddef.h>

#define POWERED_ROUTE_REFINEMENT_LIMIT 500
#define POWERED_AUDIT_SAMPLE_SIZE 32
#define POWERED_AUDIT_OBJECTIVES_PER_ROUTE 192
#define POWERED_REPAIR_SLOTS 64
#define POWERED_CHALLENGE_SLOTS POWERED_AUDIT_SAMPLE_SIZE

#define POWERED_PROGRESS_DONE_COUNTER 6
#define POWERED_PROGRESS_TOTAL_COUNTER 7

/* One progress unit is the cost of scoring one route (about 0.8 ms); refining one route is about 12 units. */
#define POWERED_PROGRESS_SECONDS_PER_UNIT 0.0008
#define POWERED_REFINE_PROGRESS_WEIGHT 12
#define POWERED_PROGRESS_REFINE_UNITS (POWERED_REFINE_PROGRESS_WEIGHT * \
	(POWERED_REPAIR_SLOTS + POWERED_ROUTE_REFINEMENT_LIMIT + POWERED_CHALLENGE_SLOTS))

typedef struct PoweredRejectionAuditGroup {
	unsigned long long population;
	int sampled, feasible, improved, beats_retained_best, beats_retained_worst;
	unsigned long long evaluations;
	double best_delta_v;
} PoweredRejectionAuditGroup;

typedef struct PoweredRejectionAudit {
	PoweredRejectionAuditGroup initial_failure, refinement_cutoff;
	double retained_best, retained_worst, seconds;
	int completed;
} PoweredRejectionAudit;

/* Opt-in diagnostic: private route copies, fixed deterministic sampling and work budgets. */
void set_powered_rejection_audit_enabled(int enabled);
PoweredRejectionAudit get_powered_rejection_audit(void);

typedef struct PoweredFlybyOptimizationStats {
	int routes_considered;
	int routes_valid;
	int routes_corrected;
	int routes_improved;
	double best_delta_v_saving;
	double best_unpowered_seed_total_dv;
	double best_initial_total_dv;
	double best_refined_total_dv;
	double exact_scoring_seconds;
	double timing_refinement_seconds;
	unsigned long long objective_evaluations;
	int refinement_budgets_exhausted;
	int multi_start_routes;
	unsigned long long initial_rejections, challenge_population;
	int repairs_attempted, repairs_recovered, repairs_competing;
	int challenges_refined, challenges_beat_cutoff;
} PoweredFlybyOptimizationStats;

int optimize_powered_flyby_routes(struct ItinStep **departures, int num_departures,
	const Itin_Calc_Data *calc_data, int max_routes, PoweredFlybyOptimizationStats *stats);

double evaluate_powered_flyby_route_objective(const Itin_Calc_Data *calc_data,
	Body **bodies, const double *times, size_t num_steps);

#endif
