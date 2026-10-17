#ifndef KMAT_ITINERARY_TEXT_H
#define KMAT_ITINERARY_TEXT_H

#include "itin_tool.h"

// store itineraries in text file from multiple departures (pre-order storing)
void store_itineraries_in_file(struct ItinStep **departures, int num_nodes, int num_deps);

void itinerary_short_overview_to_string(struct ItinStep *step, enum DateType date_type, double dep_periapsis, double arr_periapsis, char *string);

void itinerary_detailed_overview_to_string(struct ItinStep *step, enum DateType date_type, double dep_periapsis, double arr_periapsis, char *string);

// buffer size that fits every text produced by itinerary_step_parameters_to_string
#define ITIN_STEP_TEXT_SIZE 512

void itinerary_step_parameters_to_string(char *s_label, char *s_values, enum DateType date, double dep_periapsis, double arr_periapsis, struct ItinStep *step);

#endif
