#ifndef KMAT_FLYBY_SEARCH_H
#define KMAT_FLYBY_SEARCH_H

#include "itin_tool.h"

#ifdef KMAT_ENABLE_SEARCH_PROFILE
typedef struct ItinFlybyProfileStats {
	unsigned long long searches;
	unsigned long long left_evaluations;
	unsigned long long right_evaluations;
} ItinFlybyProfileStats;
#endif

typedef struct PoweredSeedSearchStats {
	unsigned long long evaluations, alternatives_added, alternatives_skipped;
	unsigned long long seeds_added, searches_budget_skipped;
} PoweredSeedSearchStats;
#define POWERED_SEEDS_PER_DEPARTURE 512
#define POWERED_EXTRA_SEEDS_PER_DEPARTURE 8
void begin_powered_seed_search(PoweredSeedSearchStats *stats);

// find viable flybys to next body with a given arrival trajectory; the same body gives resonant (multi-revolution) returns
void find_viable_flybys(struct ItinStep *tf, CelestSystem *system, Body *next_body, double min_dt, double max_dt);

// Time until the body of osv0_next is next in conjunction with / opposite to r0, seen from the central body.
void calc_time_to_next_conjunction_and_opposition(Vector3 r0, OSV osv0_next, Body *cb, double *next_conjunction_dt, double *next_opposition_dt);

#ifdef KMAT_ENABLE_SEARCH_PROFILE
void set_itin_search_lambert_counter(unsigned long long *counter);
void set_itin_search_flyby_profile(Body **bodies, int num_bodies, ItinFlybyProfileStats *stats);
#endif

#endif
