#include "unit_tests.h"

typedef struct {
	ItineraryDatePickerState *state;
	int destroy_count;
} FakeDatePicker;

static void destroy_fake_date_picker(void *dialog) {
	FakeDatePicker *picker = dialog;
	picker->destroy_count++;
	itinerary_date_picker_closed(picker->state, dialog);
}

void run_itinerary_date_input_tests(void) {
	expect_int("Solar System dates use ISO", itinerary_date_type_for_system_name("Solar System"), DATE_ISO);
	expect_int("Kerbin dates use the 426-day calendar", itinerary_date_type_for_system_name("Kerbin"), DATE_KERBAL);

	ItineraryStartupDateDefaults iso_defaults = calculate_itinerary_startup_date_defaults(DATE_ISO);
	expect_datetime("ISO startup departure default", iso_defaults.departure_date,
		(Datetime) {.y = 1950, .m = 1, .d = 1, .date_type = DATE_ISO}, 0);
	expect_datetime("ISO startup arrival matches departure", iso_defaults.latest_arrival_date,
		iso_defaults.departure_date, 0);
	expect_int("ISO startup duration is zero", iso_defaults.max_duration_user_days, 0);

	ItineraryStartupDateDefaults kerbal_defaults = calculate_itinerary_startup_date_defaults(DATE_KERBAL);
	expect_datetime("Kerbal startup departure default", kerbal_defaults.departure_date,
		(Datetime) {.y = 1, .d = 1, .date_type = DATE_KERBAL}, 0);
	expect_datetime("Kerbal startup arrival matches departure", kerbal_defaults.latest_arrival_date,
		kerbal_defaults.departure_date, 0);
	expect_int("Kerbal startup duration is zero", kerbal_defaults.max_duration_user_days, 0);
	expect_int("Kerbal picker has 426 days", itinerary_date_picker_day_limit(DATE_KERBAL), 426);
	expect_int("KerbalISO picker has 365 days", itinerary_date_picker_day_limit(DATE_KERBALISO), 365);

	expect_int("ISO accepts leap day", itinerary_date_entry_is_complete("2024-02-29", DATE_ISO), 1);
	expect_int("ISO rejects invalid leap day", itinerary_date_entry_is_complete("2023-02-29", DATE_ISO), 0);
	expect_int("ISO rejects unpadded month", itinerary_date_entry_is_complete("2024-2-29", DATE_ISO), 0);
	expect_int("ISO rejects trailing text", itinerary_date_entry_is_complete("2024-02-29x", DATE_ISO), 0);
	expect_int("ISO accepts an incomplete date prefix",
		itinerary_date_entry_is_valid_prefix("2024-02-", DATE_ISO), 1);
	expect_int("ISO rejects invalid date prefix characters",
		itinerary_date_entry_is_valid_prefix("2024/02", DATE_ISO), 0);

	expect_int("Kerbal accepts day 426", itinerary_date_entry_is_complete("1-426", DATE_KERBAL), 1);
	expect_int("Kerbal rejects day 427", itinerary_date_entry_is_complete("1-427", DATE_KERBAL), 0);
	expect_int("Kerbal rejects unpadded day", itinerary_date_entry_is_complete("1-42", DATE_KERBAL), 0);
	expect_int("Kerbal accepts a partial day prefix",
		itinerary_date_entry_is_valid_prefix("1-42", DATE_KERBAL), 1);
	expect_int("Kerbal short complete day stays a valid input prefix",
		itinerary_date_entry_is_valid_prefix("1-55", DATE_KERBAL), 1);
	char normalized_date[32];
	expect_int("Kerbal short day normalizes", itinerary_date_entry_normalize(
		"1-55", DATE_KERBAL, normalized_date, sizeof(normalized_date)), 1);
	expect_int("Kerbal short day gets three digits", strcmp(normalized_date, "1-055") == 0, 1);
	expect_int("Kerbal rejects normalization past day 426",
		itinerary_date_entry_normalize("1-427", DATE_KERBAL, normalized_date, sizeof(normalized_date)), 0);
	expect_int("Kerbal rejects trailing text during normalization",
		itinerary_date_entry_normalize("1-55x", DATE_KERBAL, normalized_date, sizeof(normalized_date)), 0);
	expect_int("KerbalISO accepts day 365",
		itinerary_date_entry_is_complete("1-365", DATE_KERBALISO), 1);
	expect_int("KerbalISO rejects day 366",
		itinerary_date_entry_is_complete("1-366", DATE_KERBALISO), 0);

	expect_int("Equal startup dates are a valid range", validate_itinerary_date_range(
		"1-001", "1-001", "1-001", DATE_KERBAL, 1), ITINERARY_DATE_RANGE_VALID);
	expect_int("Earlier latest departure is reported", validate_itinerary_date_range(
		"2024-01-02", "2024-01-01", "2024-01-03", DATE_ISO, 1),
		ITINERARY_DATE_RANGE_LATEST_DEPARTURE_TOO_EARLY);
	expect_int("Earlier latest arrival is reported", validate_itinerary_date_range(
		"2024-01-01", "2024-01-03", "2024-01-02", DATE_ISO, 1),
		ITINERARY_DATE_RANGE_LATEST_ARRIVAL_BEFORE_LATEST_DEPARTURE);
	expect_int("Both date range warnings can be active", validate_itinerary_date_range(
		"2024-01-03", "2024-01-01", "2023-12-31", DATE_ISO, 1),
		ITINERARY_DATE_RANGE_LATEST_DEPARTURE_TOO_EARLY |
		ITINERARY_DATE_RANGE_LATEST_ARRIVAL_BEFORE_LATEST_DEPARTURE);
	expect_int("Duration mode ignores latest arrival date", validate_itinerary_date_range(
		"2024-01-02", "2024-01-03", "2024-01-01", DATE_ISO, 0), ITINERARY_DATE_RANGE_VALID);
	expect_int("Invalid date range input is rejected", validate_itinerary_date_range(
		"2024-02-30", "2024-03-01", "2024-03-02", DATE_ISO, 1),
		ITINERARY_DATE_RANGE_INVALID_FORMAT);

	expect_near("Blank departure DV limit is unbounded",
		parse_itinerary_dv_limit(""), ITINERARY_UNBOUNDED_DV_LIMIT, 0);
	expect_near("Whitespace-only satellite DV limit is unbounded",
		parse_itinerary_dv_limit(" \t"), ITINERARY_UNBOUNDED_DV_LIMIT, 0);
	expect_near("Explicit zero DV limit remains zero", parse_itinerary_dv_limit("0"), 0, 0);
	expect_near("Explicit DV limit is parsed", parse_itinerary_dv_limit("1250"), 1250, 0);

	ItineraryDatePickerState picker_state = {0};
	FakeDatePicker first_picker = {.state = &picker_state};
	FakeDatePicker second_picker = {.state = &picker_state};
	itinerary_date_picker_track(&picker_state, &first_picker, destroy_fake_date_picker);
	itinerary_date_picker_track(&picker_state, &second_picker, destroy_fake_date_picker);
	expect_int("Opening a new picker closes the previous one", first_picker.destroy_count, 1);
	expect_int("New picker remains active", picker_state.dialog == &second_picker, 1);
	itinerary_date_picker_close(&picker_state);
	expect_int("Closing for a system change destroys the active picker", second_picker.destroy_count, 1);
	expect_int("Closing clears active picker state", picker_state.dialog == NULL, 1);
	itinerary_date_picker_close(&picker_state);
	expect_int("Closing an already closed picker is harmless", second_picker.destroy_count, 1);
}
