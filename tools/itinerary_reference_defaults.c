#include "tools/itinerary_reference_defaults.h"

#include <math.h>

int calculate_itinerary_hohmann_reference(
	Body *departure_body,
	Body *arrival_body,
	Body *central_body,
	double departure_periapsis_altitude,
	double arrival_periapsis_altitude,
	ItineraryHohmannReference *reference) {
	if(departure_body == NULL || arrival_body == NULL || central_body == NULL || reference == NULL ||
		departure_body == arrival_body || central_body->mu <= 0 ||
		departure_body->orbit.a <= 0 || arrival_body->orbit.a <= 0) return 0;

	Hohmann transfer = calc_hohmann_transfer(departure_body->orbit.a, arrival_body->orbit.a, central_body);
	ItineraryHohmannReference result = {
		.duration_seconds = transfer.dur,
		.departure_dv = dv_circ(departure_body,
			altatmo2radius(departure_body, departure_periapsis_altitude), transfer.dv_dep),
		.arrival_capture_dv = dv_capture(arrival_body,
			altatmo2radius(arrival_body, arrival_periapsis_altitude), transfer.dv_arr),
		.arrival_circularization_dv = dv_circ(arrival_body,
			altatmo2radius(arrival_body, arrival_periapsis_altitude), transfer.dv_arr)
	};
	if(!isfinite(result.duration_seconds) || !isfinite(result.departure_dv) ||
		!isfinite(result.arrival_capture_dv) || !isfinite(result.arrival_circularization_dv)) return 0;
	*reference = result;
	return 1;
}

int calculate_itinerary_hohmann_defaults(
	const ItineraryHohmannReference *reference,
	enum LastTransferType transfer_type,
	enum DateType date_type,
	double earliest_departure_jd,
	ItineraryHohmannDefaults *defaults) {
	if(reference == NULL || defaults == NULL || !isfinite(earliest_departure_jd) ||
		!isfinite(reference->duration_seconds) || reference->duration_seconds <= 0 ||
		!isfinite(reference->departure_dv) || !isfinite(reference->arrival_capture_dv) ||
		!isfinite(reference->arrival_circularization_dv) ||
		(transfer_type != TF_FLYBY && transfer_type != TF_CAPTURE && transfer_type != TF_CIRC)) return 0;

	double seconds_per_user_day = date_type == DATE_KERBAL ? 6 * 60 * 60 : 24 * 60 * 60;
	double duration_user_days = ceil(2.0 * reference->duration_seconds / seconds_per_user_day);
	double duration_jd_days = duration_user_days * (date_type == DATE_KERBAL ? 0.25 : 1.0);
	double satellite_dv = transfer_type == TF_CAPTURE ? reference->arrival_capture_dv :
		transfer_type == TF_CIRC ? reference->arrival_circularization_dv : 0;
	*defaults = (ItineraryHohmannDefaults) {
		.max_arrival_jd = earliest_departure_jd + duration_jd_days,
		.max_duration_user_days = duration_user_days,
		.max_departure_dv = reference->departure_dv,
		.max_satellite_dv = satellite_dv,
		.max_total_dv = reference->departure_dv + satellite_dv
	};
	return 1;
}