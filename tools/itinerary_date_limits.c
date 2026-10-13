#include "tools/itinerary_date_limits.h"

ItineraryDateLimits calculate_itinerary_date_limits(
	int use_max_arrival_date,
	double min_departure_jd,
	double max_departure_jd,
	double max_arrival_jd,
	double user_max_duration,
	enum DateType date_type) {
	if(use_max_arrival_date) {
		double duration = max_arrival_jd - min_departure_jd;
		return (ItineraryDateLimits) {
			.max_arrival_jd = max_arrival_jd,
			.max_duration_days = duration > 0 ? duration : 0
		};
	}

	double duration_days = user_max_duration;
	if(date_type == DATE_KERBAL) duration_days /= 4.0;
	return (ItineraryDateLimits) {
		.max_arrival_jd = max_departure_jd + duration_days,
		.max_duration_days = duration_days
	};
}