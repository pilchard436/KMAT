#ifndef KMAT_TRANSFER_SEARCH_INTERNAL_H
#define KMAT_TRANSFER_SEARCH_INTERNAL_H

/* Types and helpers shared by the itinerary search modules (search driver, worker, progress). */

#include "transfer_calc.h"
#include "powered_flyby_optimizer.h"

#ifdef KMAT_ENABLE_SEARCH_PROFILE
typedef struct Itin_Search_Profile {
	double initial_grid_seconds;
	double recursive_search_seconds;
	double recursive_end_node_seconds;
	double recursive_dv_pruning_seconds;
	unsigned long long initial_lambert_calls;
	unsigned long long recursive_lambert_calls;
	ItinFlybyProfileStats *flyby_stats;
} Itin_Search_Profile;

extern _Thread_local Itin_Search_Profile *itin_search_profile_tls;
#endif

typedef struct Itin_Calc_Thread_Args {
	struct ItinStep **departures;
	PoweredSeedSearchStats *seed_stats;
	Itin_Calc_Data *calc_data;
#ifdef KMAT_ENABLE_SEARCH_PROFILE
	Itin_Search_Profile *profiles;
	Body **profile_bodies;
	int num_profile_bodies;
#endif
} Itin_Calc_Thread_Args;

struct Thread_Progress_and_Prop_Info {
	int num_departures;
	Transfer_Progress_Phase phase;
	int phase_done;
	int phase_total;
	int phase_transfer_type;
};
extern struct Thread_Progress_and_Prop_Info thread_progress_and_prop_info;

extern const int NUM_INITIAL_TRANSFERS_PER_BODY;

/* transfer_search_progress.c */
double progress_clock(void);
void show_departure_progress(double total_departures);
void set_calculation_progress_phase(Transfer_Progress_Phase phase, int transfer_type,
	int phase_done, int phase_total);
void increment_calculation_progress(void);

/* transfer_search_worker.c: thread entry that searches the itineraries of departure dates */
void *calc_itins_from_departure(void *args);

#endif
