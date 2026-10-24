#include "tools/itinerary_date_input.h"

#include <glib.h>
#include <string.h>

static gboolean has_only_digits(const char *text, gsize length) {
	for(gsize i = 0; i < length; i++)
		if(!g_ascii_isdigit(text[i])) return FALSE;
	return TRUE;
}

enum DateType itinerary_date_type_for_system_name(const char *system_name) {
	return system_name != NULL && g_str_has_prefix(system_name, "Solar System") ?
		DATE_ISO : DATE_KERBAL;
}

ItineraryStartupDateDefaults calculate_itinerary_startup_date_defaults(enum DateType date_type) {
	Datetime departure_date = date_type == DATE_ISO ?
		(Datetime) {.y = 1950, .m = 1, .d = 1, .date_type = DATE_ISO} :
		(Datetime) {.y = 1, .d = 1, .date_type = date_type};
	return (ItineraryStartupDateDefaults) {
		.departure_date = departure_date,
		.latest_arrival_date = departure_date,
		.max_duration_user_days = 0
	};
}

int itinerary_date_picker_day_limit(enum DateType date_type) {
	return date_type == DATE_KERBAL ? 426 : 365;
}

int itinerary_date_picker_step_date(Datetime *date, int day_delta) {
	if(date == NULL || date->y < 1 || date->y > 9999 || day_delta == 0) return 0;
	Datetime stepped_date = *date;
	if(date->date_type == DATE_ISO) {
		if(!g_date_valid_dmy(date->d, date->m, date->y)) return 0;
		double stepped_jd = jd_change_date(convert_date_JD(*date), 0, 0, day_delta, DATE_ISO);
		stepped_date = convert_JD_date(stepped_jd, DATE_ISO);
		if(stepped_date.y < 1 || stepped_date.y > 9999 ||
			!g_date_valid_dmy(stepped_date.d, stepped_date.m, stepped_date.y)) return 0;
	} else {
		int days_per_year = itinerary_date_picker_day_limit(date->date_type);
		if(date->d < 1 || date->d > days_per_year) return 0;
		long long day_index = (long long)(date->y - 1) * days_per_year + date->d - 1 + day_delta;
		long long year_count = (long long)9999 * days_per_year;
		if(day_index < 0 || day_index >= year_count) return 0;
		stepped_date.y = (int)(day_index / days_per_year) + 1;
		stepped_date.d = (int)(day_index % days_per_year) + 1;
	}
	*date = stepped_date;
	return 1;
}

int itinerary_date_entry_is_complete(const char *text, enum DateType date_type) {
	if(text == NULL || !is_string_valid_date_format(text, date_type)) return 0;
	Datetime date = date_from_string((char *)text, date_type);
	char canonical_text[32];
	date_to_string(date, canonical_text, 0);
	if(g_strcmp0(text, canonical_text) != 0 || date.y < 1 || date.y > 9999) return 0;
	if(date_type == DATE_ISO) return g_date_valid_dmy(date.d, date.m, date.y);
	return date.d <= itinerary_date_picker_day_limit(date_type);
}

ItineraryDateRangeStatus validate_itinerary_date_range(
	const char *earliest_departure,
	const char *latest_departure,
	const char *latest_arrival,
	enum DateType date_type,
	int use_latest_arrival_date) {
	ItineraryDateRangeStatus status = ITINERARY_DATE_RANGE_VALID;
	if(!itinerary_date_entry_is_complete(earliest_departure, date_type) ||
		!itinerary_date_entry_is_complete(latest_departure, date_type) ||
		(use_latest_arrival_date && !itinerary_date_entry_is_complete(latest_arrival, date_type)))
		status |= ITINERARY_DATE_RANGE_INVALID_FORMAT;
	else {
		double earliest_jd = convert_date_JD(date_from_string((char *)earliest_departure, date_type));
		double latest_departure_jd = convert_date_JD(date_from_string((char *)latest_departure, date_type));
		if(latest_departure_jd < earliest_jd)
			status |= ITINERARY_DATE_RANGE_LATEST_DEPARTURE_TOO_EARLY;
		if(use_latest_arrival_date) {
			double latest_arrival_jd = convert_date_JD(date_from_string((char *)latest_arrival, date_type));
			if(latest_arrival_jd < latest_departure_jd)
				status |= ITINERARY_DATE_RANGE_LATEST_ARRIVAL_BEFORE_LATEST_DEPARTURE;
		}
	}
	return status;
}

int itinerary_date_entry_is_valid_prefix(const char *text, enum DateType date_type) {
	if(text == NULL) return 0;
	gsize length = strlen(text);
	if(length == 0) return 1;

	const char *first_dash = strchr(text, '-');
	if(date_type == DATE_ISO) {
		if(first_dash == NULL)
			return length <= 4 && has_only_digits(text, length);
		gsize year_length = (gsize)(first_dash - text);
		if(year_length == 0 || year_length > 4 || !has_only_digits(text, year_length)) return 0;
		const char *second_dash = strchr(first_dash + 1, '-');
		if(second_dash == NULL) {
			gsize month_length = strlen(first_dash + 1);
			return month_length <= 2 && has_only_digits(first_dash + 1, month_length);
		}
		if(strchr(second_dash + 1, '-') != NULL || (gsize)(second_dash - first_dash - 1) != 2)
			return 0;
		gsize day_length = strlen(second_dash + 1);
		if(day_length > 2 || !has_only_digits(second_dash + 1, day_length)) return 0;
		return day_length < 2 || itinerary_date_entry_is_complete(text, date_type);
	}

	if(first_dash == NULL) return length <= 4 && has_only_digits(text, length);
	gsize year_length = (gsize)(first_dash - text);
	if(year_length == 0 || year_length > 4 || !has_only_digits(text, year_length) ||
		strchr(first_dash + 1, '-') != NULL) return 0;
	gsize day_length = strlen(first_dash + 1);
	if(day_length > 3 || !has_only_digits(first_dash + 1, day_length)) return 0;
	return day_length < 3 || itinerary_date_entry_is_complete(text, date_type);
}

int itinerary_date_entry_normalize(
	const char *text,
	enum DateType date_type,
	char *normalized_text,
	size_t normalized_text_size) {
	if(text == NULL || normalized_text == NULL || normalized_text_size == 0) return 0;
	if(itinerary_date_entry_is_complete(text, date_type)) {
		g_strlcpy(normalized_text, text, normalized_text_size);
		return 1;
	}
	if(date_type == DATE_ISO) return 0;

	const char *dash = strchr(text, '-');
	if(dash == NULL || strchr(dash + 1, '-') != NULL) return 0;
	gsize year_length = (gsize)(dash - text);
	gsize day_length = strlen(dash + 1);
	if(year_length == 0 || year_length > 4 || day_length == 0 || day_length > 3 ||
		!has_only_digits(text, year_length) || !has_only_digits(dash + 1, day_length)) return 0;

	char *year_text = g_strndup(text, year_length);
	char *day_text = g_strdup(dash + 1);
	char *year_end = NULL, *day_end = NULL;
	long year = strtol(year_text, &year_end, 10);
	long day = strtol(day_text, &day_end, 10);
	gboolean valid = year_end != year_text && *year_end == '\0' &&
		day_end != day_text && *day_end == '\0';
	g_free(year_text);
	g_free(day_text);
	if(!valid ||
		year < 1 || year > 9999 || day < 1 || day > itinerary_date_picker_day_limit(date_type)) return 0;

	gint written = g_snprintf(normalized_text, normalized_text_size, "%ld-%03ld", year, day);
	return written >= 0 && (gsize)written < normalized_text_size;
}
