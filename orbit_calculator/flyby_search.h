#ifndef KMAT_FLYBY_SEARCH_H
#define KMAT_FLYBY_SEARCH_H

#include "itin_tool.h"

#ifdef KMAT_ENABLE_SEARCH_PROFILE
typedef struct ItinFlybyProfileStats {
	unsigned long long searches;
	unsigned long long left_evaluations;
	unsigned long long right_evaluations;
	double search_seconds;
} ItinFlybyProfileStats;
#endif

typedef struct PoweredSeedSearchStats {
	unsigned long long evaluations, alternatives_added, alternatives_skipped;
	unsigned long long seeds_added, searches_budget_skipped;
} PoweredSeedSearchStats;
#define POWERED_SEEDS_PER_DEPARTURE 2048
#define POWERED_EXTRA_SEEDS_PER_DEPARTURE 8
#define POWERED_SEED_BUDGET_PRESET_COUNT 5
void begin_powered_seed_search(PoweredSeedSearchStats *stats);
/* Whether find_viable_flybys may add seeds that need a flyby burn; on by default. Per thread; returns the previous value. */
int set_powered_flybys_enabled(int enabled);
// Seeds the current subtree may still use (the budget left above the active share floor).
int get_powered_seed_available(void);
// Limits the next subtree so that half of the seeds still available stay reserved for the other `shares`-1 siblings.
int begin_powered_seed_share(int shares);
void end_powered_seed_share(int previous_floor);
/* Applies to searches started afterwards; values below 1 are ignored. */
void set_powered_seed_budget(int seeds);
int get_powered_seed_budget(void);
/* User-selectable budgets, ascending; the default is POWERED_SEEDS_PER_DEPARTURE. */
int get_powered_seed_budget_preset(int index);
/* Index of seeds in the presets, or -1 when it is not a preset. */
int find_powered_seed_budget_preset(int seeds);

// find viable flybys to next body with a given arrival trajectory; the same body gives resonant (multi-revolution) returns
void find_viable_flybys(struct ItinStep *tf, CelestSystem *system, Body *next_body, double min_dt, double max_dt);
// Same, but max_dt is only a look-ahead horizon, not a deadline, so samples at it are kept like any other.
void find_viable_flybys_within_horizon(struct ItinStep *tf, CelestSystem *system, Body *next_body, double min_dt, double max_dt);

// Time until the body of osv0_next is next in conjunction with / opposite to r0, seen from the central body.
void calc_time_to_next_conjunction_and_opposition(Vector3 r0, OSV osv0_next, Body *cb, double *next_conjunction_dt, double *next_opposition_dt);

#ifdef KMAT_ENABLE_SEARCH_PROFILE
void set_itin_search_lambert_counter(unsigned long long *counter);
void set_itin_search_flyby_profile(Body **bodies, int num_bodies, ItinFlybyProfileStats *stats);
#endif

#endif
