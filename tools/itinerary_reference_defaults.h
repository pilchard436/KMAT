#ifndef KMAT_ITINERARY_REFERENCE_DEFAULTS_H
#define KMAT_ITINERARY_REFERENCE_DEFAULTS_H

#include "orbit_calculator/itin_tool.h"

typedef struct {
	double duration_seconds;
	double departure_dv;
	double arrival_capture_dv;
	double arrival_circularization_dv;
} ItineraryHohmannReference;

typedef struct {
	double max_arrival_jd;
	double max_duration_user_days;
	double max_departure_dv;
	double max_satellite_dv;
	double max_total_dv;
} ItineraryHohmannDefaults;

int calculate_itinerary_hohmann_reference(
	Body *departure_body,
	Body *arrival_body,
	Body *central_body,
	double departure_periapsis_altitude,
	double arrival_periapsis_altitude,
	ItineraryHohmannReference *reference);

int calculate_itinerary_hohmann_defaults(
	const ItineraryHohmannReference *reference,
	enum LastTransferType transfer_type,
	enum DateType date_type,
	double earliest_departure_jd,
	ItineraryHohmannDefaults *defaults);

#endif