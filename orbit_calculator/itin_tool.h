#ifndef KSP_ITIN_TOOL_H
#define KSP_ITIN_TOOL_H

#include "tools/celestial_systems.h"
#include "orbitlib_transfer.h"
#include <stdio.h>

struct ItinStep {
	Body *body;
	Vector3 r;
	Vector3 v_arr, v_body, v_dep;
	double date;
	int num_next_nodes;
	struct ItinStep *prev;
	struct ItinStep **next;
};

enum ItinSequenceInfoType {ITIN_SEQ_INFO_TO_TARGET, ITIN_SEQ_INFO_SPEC_SEQ};

typedef union ItinSequenceInfo {
	struct ItinSequenceInfoToTarget {
		enum ItinSequenceInfoType type;
		CelestSystem *system;
		Body *dep_body, *arr_body, **flyby_bodies;
		int num_flyby_bodies;
		int allow_resonant;	// also search flybys that return to the same body after revolutions
	} to_target;

	struct ItinSequenceInfoSpecItin {
		enum ItinSequenceInfoType type;
		CelestSystem *system;
		struct Body **bodies;
		int num_steps;
	} spec_seq;
} ItinSequenceInfo;

struct Dv_Filter {
	double max_totdv, max_depdv, max_satdv;
	double dep_periapsis, arr_periapsis;
	int last_transfer_type;
};

typedef struct Calc_Acc {
	double lambert, fb_finder;
} Calc_Acc;

#ifdef KMAT_ENABLE_SEARCH_PROFILE
typedef struct ItinFlybyProfileStats {
	unsigned long long searches;
	unsigned long long left_evaluations;
	unsigned long long right_evaluations;
} ItinFlybyProfileStats;
#endif

struct PorkchopPoint {
	struct ItinStep *arrival;
	double dep_date, dur;
	double dv_dep, dv_dsm, dv_flyby, dv_arr_cap, dv_arr_circ;
};

typedef struct PoweredSeedSearchStats {
	unsigned long long evaluations, alternatives_added, alternatives_skipped;
	unsigned long long seeds_added, searches_budget_skipped;
} PoweredSeedSearchStats;
#define POWERED_SEEDS_PER_DEPARTURE 512
#define POWERED_EXTRA_SEEDS_PER_DEPARTURE 8
void begin_powered_seed_search(PoweredSeedSearchStats *stats);


enum LastTransferType {TF_FLYBY, TF_CAPTURE, TF_CIRC};

// find viable flybys to next body with a given arrival trajectory; the same body gives resonant (multi-revolution) returns
void find_viable_flybys(struct ItinStep *tf, CelestSystem *system, Body *next_body, double min_dt, double max_dt);

// A leg is solved as variant 0 (no revolutions) or as 1..LEG_VARIANT_COUNT-1: revolutions (v+1)/2, branch (v-1)%2.
#define LEG_VARIANT_MAX_REVS 4
#define LEG_VARIANT_COUNT (1 + 2 * LEG_VARIANT_MAX_REVS)
Lambert3 calc_leg_variant_lambert(Body *cb, Vector3 r0, Vector3 r1, double dt, int variant,
		LambertCancelCheck cancel_check, void *cancel_context);
// Picks the variant whose departure velocity is closest to v_ref; returns 0 if none exists.
int nearest_leg_variant_lambert(Body *cb, Vector3 r0, Vector3 r1, double dt, Vector3 v_ref, Lambert3 *leg, int *variant);
// Samples of (duration, variant) for a first leg that returns to the departure body, around 1 and 2 of its periods.
int resonant_departure_sample_count(void);
void resonant_departure_sample(int index, double period_days, double *duration_days, int *variant);
#ifdef KMAT_ENABLE_SEARCH_PROFILE
void set_itin_search_lambert_counter(unsigned long long *counter);
void set_itin_search_flyby_profile(Body **bodies, int num_bodies, ItinFlybyProfileStats *stats);
#endif

// find viable flybys to next body with a given arrival trajectory

// return departure step
struct ItinStep * get_first(struct ItinStep *tf);

// return last step from itinerary (first branch)
struct ItinStep * get_last(struct ItinStep *tf);

// print itinerary dates (arrival first)
void print_itinerary(struct ItinStep *itin);

// returns the number of itineraries (departure first)
int get_number_of_itineraries(struct ItinStep *itin);

// returns number of total amount of stored ininerary steps (departure first)
int get_total_number_of_stored_steps(struct ItinStep *itin);

// stores all arrival nodes in array from itin (departure first)
void store_itineraries_in_array(struct ItinStep *itin, struct ItinStep **array, int *index);

// returns the itinerary duration in days (arrival first)
double get_itinerary_duration(struct ItinStep *itin);

// returns an array of porkchop points analyzed from the given departures (allocates porkchop array memory --> needs to be freed)
struct PorkchopPoint *create_porkchop_array_from_departures(struct ItinStep **departures, int num_deps, double dep_periapsis, double arr_periapsis);

// add itinerary departure date, duration, departure dv, deep-space maneuvre dv and arrival dv in porkchop array
struct PorkchopPoint create_porkchop_point(struct ItinStep *itin, double dep_periapsis, double arr_periapsis);

// from current step and given information, initiate calculation of next steps
int calc_next_spec_itin_step(struct ItinStep *curr_step, CelestSystem *system, Body **bodies, double jd_max_arr, struct Dv_Filter *dv_filter, int num_steps, int step);

// from current step and given information, initiate calculation of next steps
int calc_next_itin_to_target_step(struct ItinStep *curr_step, struct ItinSequenceInfoToTarget *seq_info, double jd_max_arr, double max_total_duration, struct Dv_Filter *dv_filter);

// initiate calc of next itinerary steps and return 0 if no steps are remaining in this itinerary (1 otherwise)
int continue_to_next_steps_and_check_for_valid_itins(struct ItinStep *curr_step, int num_of_end_nodes, struct ItinSequenceInfoToTarget *seq_info, double jd_max_arr, double max_total_duration, struct Dv_Filter *dv_filter);

// find end nodes (next step at arrival body) and copy to the end of the next steps array of curr_step
int find_copy_and_store_end_nodes(struct ItinStep *curr_step, Body *arr_body);

// removes end nodes from initerary that do not satisfy dv requirements (returns new number of end nodes)
int remove_end_nodes_that_do_not_satisfy_dv_requirements(struct ItinStep *curr_step, int num_of_end_nodes, struct Dv_Filter *dv_filter);

// get the number of layers/steps in the given itinerary (departure first)
int get_num_of_itin_layers(struct ItinStep *step);

// update r and v_body vectors of itinerary steps (departure first)
void update_itin_body_osvs(struct ItinStep *step, CelestSystem *system);

// calculate from velocity vectors for itinerary steps from date and r vector (departure first)
void calc_itin_v_vectors_from_dates_and_r(struct ItinStep *step, CelestSystem *system);

// copy the body reference, r and v vectors and date from orig_step to step_copy
void copy_step_body_vectors_and_date(struct ItinStep *orig_step, struct ItinStep *step_copy);

// create and return copy of itineraries from given step (rising date)
struct ItinStep * create_itin_copy(struct ItinStep *step);

// create and return copy of single itinerary from arrival (falling date)
struct ItinStep * create_itin_copy_from_arrival(struct ItinStep *step);

// returns 1 if itinerary is valid and 0 if not (arrival first)
int is_valid_itinerary(struct ItinStep *step);

// store itineraries in text file from multiple departures (pre-order storing)
void store_itineraries_in_file(struct ItinStep **departures, int num_nodes, int num_deps);

void itinerary_short_overview_to_string(struct ItinStep *step, enum DateType date_type, double dep_periapsis, double arr_periapsis, char *string);

void itinerary_detailed_overview_to_string(struct ItinStep *step, enum DateType date_type, double dep_periapsis, double arr_periapsis, char *string);

// buffer size that fits every text produced by itinerary_step_parameters_to_string
#define ITIN_STEP_TEXT_SIZE 512

void itinerary_step_parameters_to_string(char *s_label, char *s_values, enum DateType date, double dep_periapsis, double arr_periapsis, struct ItinStep *step);

// removes this and all now unneeded steps from itineraries (no next node before arrival)
void remove_step_from_itinerary(struct ItinStep *step);

// delta-v of one step of a single itinerary, using the same burns as the itinerary overview texts
double itinerary_step_dv(struct ItinStep *step, double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type);

// Metres by which the unpowered periapsis of a flyby step lies below the minimum safe radius (surface, atmosphere
// and 10 km); 0 if it can be flown as a gravity assist or the step is not a flyby. A positive value means the
// unpowered trajectory would enter the planet or its atmosphere and the route only works with the flyby burn.
double flyby_periapsis_shortfall(struct ItinStep *step);
// 1 if the flyby step cannot be flown even with a burn and its unpowered periapsis lies below the body's surface.
int flyby_hits_surface(struct ItinStep *step);
// Number of flybys in the route with a shortfall, and the largest shortfall in metres (largest may be NULL).
int count_flybys_needing_burn(struct ItinStep *step_in_route, double *largest_shortfall);

// sets a step's date and pushes later steps back if needed; a maneuver node slides along its incoming arc
void set_itinerary_step_date(struct ItinStep *step, CelestSystem *system, double date);

// free to itinerary allocated memory
void free_itinerary(struct ItinStep *step);



#endif //KSP_ITIN_TOOL_H
