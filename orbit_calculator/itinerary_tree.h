#ifndef KMAT_ITINERARY_TREE_H
#define KMAT_ITINERARY_TREE_H

#include "itin_tool.h"

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

// sets a step's date and pushes later steps back if needed; a maneuver node slides along its incoming arc
void set_itinerary_step_date(struct ItinStep *step, CelestSystem *system, double date);

// removes this and all now unneeded steps from itineraries (no next node before arrival)
void remove_step_from_itinerary(struct ItinStep *step);

// free to itinerary allocated memory
void free_itinerary(struct ItinStep *step);

#endif
