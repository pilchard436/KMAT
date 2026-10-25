#ifndef KMAT_POWERED_FLYBY_OPTIMIZER_H
#define KMAT_POWERED_FLYBY_OPTIMIZER_H

#include "orbit_calculator/transfer_calc.h"
#include <stddef.h>

#define POWERED_ROUTE_REFINEMENT_LIMIT 500
#define POWERED_REPAIR_SLOTS 64
#define POWERED_CHALLENGE_SLOTS 32

/* Opt-in deep-space maneuvers run only on this many of the best-ranked routes. */
#define POWERED_DSM_ROUTE_LIMIT 50
#define POWERED_DSM_RESONANT_ROUTE_LIMIT 30
#define POWERED_DSM_OBJECTIVES_PER_ROUTE 900

/* One impulsive maneuver on a leg: the leg's departure velocity changes by delta_v, the coast
 * continues for eta of the leg time, and a second Lambert arc reaches the next encounter. */
typedef struct PoweredLegDsm {
	int enabled;
	double eta;
	Vector3 delta_v;
	/* Instead of delta_v: the maneuver happens at this fixed position and epoch (JD). */
	int use_position;
	Vector3 position;
	double date;
} PoweredLegDsm;

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
	int dsm_routes_optimized, dsm_routes_improved;
	double best_dsm_saving;
} PoweredFlybyOptimizationStats;

int optimize_powered_flyby_routes(struct ItinStep **departures, int num_departures,
	const Itin_Calc_Data *calc_data, int max_routes, PoweredFlybyOptimizationStats *stats);

double evaluate_powered_flyby_route_objective(const Itin_Calc_Data *calc_data,
	Body **bodies, const double *times, size_t num_steps);
int powered_route_has_unmodeled_soi_crossing(const Itin_Calc_Data *calc_data,
	struct ItinStep *arrival, int *crosses);

/* Re-solves a single stored route at its current dates with the powered-flyby model the search uses
 * (SOI-corrected arcs), keeping maneuver nodes at their positions, and writes the velocities back.
 * Returns 0 and leaves the route untouched if it has fewer than three encounters or does not converge. */
int resolve_itinerary_with_powered_flybys(CelestSystem *system, struct ItinStep *first,
	double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type);

/* Same, but if the flyby iteration does not settle it writes back the last iterate and sets *converged to 0.
 * Still returns 0, leaving the route untouched, if the route cannot be evaluated at all. */
int resolve_itinerary_with_powered_flybys_approximate(CelestSystem *system, struct ItinStep *first,
	double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type, int *converged);

/* Replaces the maneuver nodes of a single stored route (three or more encounters) by the best
 * one-maneuver-per-leg solution at its current encounter dates; frees the old nodes after the first.
 * Returns 0 and leaves the route untouched if it cannot be evaluated. */
int optimize_itinerary_maneuvers(CelestSystem *system, struct ItinStep *first,
	double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type);

/* Same objective with optional per-leg DSMs (num_steps - 1 entries, NULL for none). DSM delta-v
 * counts toward the satellite delta-v limit, like every other deep-space maneuver. */
double evaluate_powered_flyby_route_objective_with_dsm(const Itin_Calc_Data *calc_data,
	Body **bodies, const double *times, size_t num_steps, const PoweredLegDsm *dsms);

/* Local search over one DSM per leg on a fixed-date route. Returns the best objective and fills
 * dsms (num_steps - 1 entries); a leg is only enabled if it lowers the objective. */
double optimize_powered_flyby_route_dsm(const Itin_Calc_Data *calc_data, Body **bodies,
	const double *times, size_t num_steps, PoweredLegDsm *dsms);

#endif
