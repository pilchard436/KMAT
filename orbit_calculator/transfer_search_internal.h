#ifndef KMAT_TRANSFER_SEARCH_INTERNAL_H
#define KMAT_TRANSFER_SEARCH_INTERNAL_H

/* Types and helpers shared by the itinerary search modules (search driver, worker, progress). */

#include "transfer_calc.h"
#include "powered_flyby_optimizer.h"

#ifdef KMAT_ENABLE_SEARCH_PROFILE
typedef struct Itin_Search_Profile {
	double initial_grid_seconds;
	double recursive_search_seconds;
	unsigned long long initial_lambert_calls;
	unsigned long long recursive_lambert_calls;
	ItinFlybyProfileStats *flyby_stats;
} Itin_Search_Profile;
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
	double jd_min_dep;
	double jd_max_dep;
	int total_initial_samples;
	int has_post_processing;
	double search_start_time;
	double search_end_time;
	double max_progress;
};
extern struct Thread_Progress_and_Prop_Info thread_progress_and_prop_info;

#define INITIAL_SAMPLE_PROGRESS_COUNTER 5

extern const int NUM_INITIAL_TRANSFERS_PER_BODY;

/* transfer_search_progress.c */
double progress_clock(void);
void show_departure_progress(double total_departures);

/* transfer_search_worker.c: thread entry that searches the itineraries of departure dates */
void *calc_itins_from_departure(void *args);

#endif
