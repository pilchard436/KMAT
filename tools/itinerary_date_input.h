#ifndef KMAT_ITINERARY_DATE_INPUT_H
#define KMAT_ITINERARY_DATE_INPUT_H

#include "orbitlib_datetime.h"
#include <stddef.h>

typedef struct {
	Datetime departure_date;
	Datetime latest_arrival_date;
	int max_duration_user_days;
} ItineraryStartupDateDefaults;

typedef enum {
	ITINERARY_DATE_RANGE_VALID = 0,
	ITINERARY_DATE_RANGE_INVALID_FORMAT = 1 << 0,
	ITINERARY_DATE_RANGE_LATEST_DEPARTURE_TOO_EARLY = 1 << 1,
	ITINERARY_DATE_RANGE_LATEST_ARRIVAL_BEFORE_LATEST_DEPARTURE = 1 << 2
} ItineraryDateRangeStatus;

enum DateType itinerary_date_type_for_system_name(const char *system_name);
ItineraryStartupDateDefaults calculate_itinerary_startup_date_defaults(enum DateType date_type);
int itinerary_date_picker_day_limit(enum DateType date_type);
int itinerary_date_entry_is_complete(const char *text, enum DateType date_type);
int itinerary_date_entry_is_valid_prefix(const char *text, enum DateType date_type);
ItineraryDateRangeStatus validate_itinerary_date_range(
	const char *earliest_departure,
	const char *latest_departure,
	const char *latest_arrival,
	enum DateType date_type,
	int use_latest_arrival_date);
int itinerary_date_entry_normalize(
	const char *text,
	enum DateType date_type,
	char *normalized_text,
	size_t normalized_text_size);

#endif
