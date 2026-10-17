#ifndef KMAT_POWERED_ROUTE_H
#define KMAT_POWERED_ROUTE_H

/* Internal types and helpers shared by the powered-flyby optimizer modules. */

#include "powered_flyby_optimizer.h"
#include <float.h>
#include <math.h>
#include <stdint.h>

#define POWERED_MIN_LEG_DAYS 10.0
#define POWERED_IMPROVEMENT_THRESHOLD 0.1

typedef struct PoweredDsmResult {
	int used;
	double date, delta_v;
	Vector3 position, velocity_before, velocity_after;
} PoweredDsmResult;

typedef struct PoweredRouteCandidate {
	int departure_index;
	size_t num_steps;
	size_t mutable_start;
	Body **bodies;
	double *signature_dates;
	Vector3 *signature_positions;
	Vector3 *signature_v_arr;
	Vector3 *signature_v_dep;
	double *times;
	OSV *best_states;
	Lambert3 *best_legs;
	double original_total_dv;
	double best_total_dv;
	PoweredLegDsm *dsms;
	PoweredDsmResult *dsm_results;
	/* Per-leg revolution variant, fixed from the stored route; NULL means try every variant. */
	int *leg_variants;
	const Vector3 *seed_incoming, *seed_outgoing;
	/* Accept the last iterate when the flyby iteration does not settle; converged_out reports which happened. */
	int accept_unconverged;
	int *converged_out;
} PoweredRouteCandidate;

typedef struct SampleReservoir {
	PoweredRouteCandidate *samples[POWERED_CHALLENGE_SLOTS];
	unsigned long long population;
	uint64_t random;
	int count;
} SampleReservoir;

typedef struct RepairPool {
	PoweredRouteCandidate *slots[POWERED_REPAIR_SLOTS];
	unsigned long long population[POWERED_REPAIR_SLOTS];
	unsigned long long total;
	uint64_t random;
} RepairPool;

typedef struct ExcludedPools {
	SampleReservoir *challenge;
} ExcludedPools;

static inline int is_finite_vector(Vector3 value) {
	return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}


/* Objective evaluation budget of the current thread; -1 means unlimited. */
extern _Thread_local int objective_budget;
extern _Thread_local unsigned long long objective_calls;

/* powered_route_objective.c */
double evaluate_route(const PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		const double *times, OSV *states, Lambert3 *legs);
double evaluate_route_core(const PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		const double *times, OSV *states, Lambert3 *legs, PoweredDsmResult *dsm_out);

/* powered_route_dsm.c */
double optimize_candidate_dsm(PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data);

/* powered_route_candidates.c */
int initialize_candidate(PoweredRouteCandidate *candidate, struct ItinStep *arrival,
		struct ItinStep **departures, int num_departures, CelestSystem *system);
void free_candidate(PoweredRouteCandidate *candidate);
void destroy_candidate(PoweredRouteCandidate *candidate);
uint64_t sample_random(uint64_t *state);
void reservoir_retain(SampleReservoir *pool, PoweredRouteCandidate *candidate);
void retain_excluded(ExcludedPools *pools, PoweredRouteCandidate *candidate);
void repair_pool_retain(RepairPool *pool, PoweredRouteCandidate *candidate, int num_departures);
int leg_has_dsm(const PoweredRouteCandidate *candidate, size_t leg);
struct ItinStep *make_optimized_suffix(const PoweredRouteCandidate *candidate);
int replace_candidate_route(struct ItinStep **departures, PoweredRouteCandidate *candidate);
int insert_top_candidate(PoweredRouteCandidate *candidate, double cost,
		int max_routes, int *count,
		PoweredRouteCandidate *candidates[POWERED_ROUTE_REFINEMENT_LIMIT],
		double costs[POWERED_ROUTE_REFINEMENT_LIMIT], ExcludedPools *excluded);

#endif
