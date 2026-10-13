#ifndef KMAT_ITINERARY_DATE_LIMITS_H
#define KMAT_ITINERARY_DATE_LIMITS_H

#include "orbitlib_datetime.h"

typedef struct {
	double max_arrival_jd;
	double max_duration_days;
} ItineraryDateLimits;

ItineraryDateLimits calculate_itinerary_date_limits(
	int use_max_arrival_date,
	double min_departure_jd,
	double max_departure_jd,
	double max_arrival_jd,
	double user_max_duration,
	enum DateType date_type);

#endif