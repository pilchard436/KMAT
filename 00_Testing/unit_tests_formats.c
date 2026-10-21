#include "unit_tests.h"

void run_formats_tests(void) {
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    (void)pi;
    (void)tolerance;
    Datetime iso_date = {2024, 2, 29, 12, 30, 15, DATE_ISO};
    Datetime kerbal_date = {3, 0, 42, 0, 0, 0, DATE_KERBAL};
    char iso_date_string[] = "2024-02-29";
    char kerbal_date_string[] = "3-042";

    expect_datetime("date_from_string ISO", date_from_string(iso_date_string, DATE_ISO),
                                (Datetime) {2024, 2, 29, 0, 0, 0, DATE_ISO}, tolerance);
    expect_datetime("date_from_string Kerbal", date_from_string(kerbal_date_string, DATE_KERBAL),
                                kerbal_date, tolerance);
    expect_datetime("convert_JD_date ISO", convert_JD_date(convert_date_JD(iso_date), DATE_ISO),
                                iso_date, 1e-3);
    check_true("case-insensitive compare equal strings", strcicmp("KMAT", "kmat") == 0);
    check_true("case-insensitive compare different strings", strcicmp("KMAT", "KMATx") != 0);
    check_true("date format accepts ISO 2024-02-29", is_string_valid_date_format("2024-02-29", DATE_ISO) == 1);
    check_true("date format accepts an ISO day of the right shape", is_string_valid_date_format("2024-02-30", DATE_ISO) == 1);
    check_true("date format accepts Kerbal 3-042", is_string_valid_date_format("3-042", DATE_KERBAL) == 1);
    check_true("date format rejects slashes in ISO", is_string_valid_date_format("2024/02/29", DATE_ISO) == 0);

    Body departure_body = {.name = "Earth"};
    Body arrival_body = {.name = "Mars"};
    Body flyby_body = {.name = "Venus"};
    Body *flyby_bodies[] = {&flyby_body};
    ItinStepBinHeaderData header = {
        .file_type = 5,
        .num_nodes = 3,
        .num_deps = 1,
        .num_itins = 1,
        .calc_data = {
            .jd_min_dep = DBL_MAX,
            .jd_max_dep = convert_date_JD((Datetime) {2024, 1, 1, 0, 0, 0, DATE_ISO}),
            .jd_max_arr = DBL_MAX,
            .max_duration = DBL_MAX,
            .seq_info = {
                .to_target = {
                    .type = ITIN_SEQ_INFO_TO_TARGET,
                    .dep_body = &departure_body,
                    .arr_body = &arrival_body,
                    .flyby_bodies = flyby_bodies,
                    .num_flyby_bodies = 1
                }
            }
        }
    };
    char header_text[1024];
    print_header_data_to_string(header, header_text, sizeof(header_text), DATE_ISO);
    expect_contains("header formats legacy unbounded arrival", header_text, "Max arrival date: Unbounded");
    expect_contains("header formats legacy unbounded duration", header_text, "Max duration: Unbounded");
    expect_contains("header includes itinerary body names", header_text, "  - Venus");

    char short_header[12];
    print_header_data_to_string(header, short_header, sizeof(short_header), DATE_ISO);
    check_true("truncated header text stays terminated", memchr(short_header, '\0', sizeof(short_header)) != NULL);

    ItineraryDateLimits arrival_date_limits = calculate_itinerary_date_limits(1, 100, 200, 160, 999, DATE_ISO);
    expect_near("max-arrival mode preserves absolute arrival JD", arrival_date_limits.max_arrival_jd, 160, tolerance);
    expect_near("max-arrival mode derives elapsed duration", arrival_date_limits.max_duration_days, 60, tolerance);

    ItineraryDateLimits kerbal_duration_limits = calculate_itinerary_date_limits(0, 100, 150, 0, 28, DATE_KERBAL);
    expect_near("Kerbal duration converts to Earth days", kerbal_duration_limits.max_duration_days, 7, tolerance);
    expect_near("Kerbal duration derives arrival JD", kerbal_duration_limits.max_arrival_jd, 157, tolerance);

    ItineraryDateLimits iso_duration_limits = calculate_itinerary_date_limits(0, 100, 150, 0, 28, DATE_ISO);
    expect_near("ISO duration remains Earth days", iso_duration_limits.max_duration_days, 28, tolerance);
    expect_near("ISO duration derives arrival JD", iso_duration_limits.max_arrival_jd, 178, tolerance);

    ItineraryDateLimits iso_like_duration_limits = calculate_itinerary_date_limits(0, 100, 150, 0, 28, DATE_KERBALISO);
    expect_near("ISO-like Kerbal duration remains Earth days", iso_like_duration_limits.max_duration_days, 28, tolerance);
    expect_near("ISO-like Kerbal duration derives arrival JD", iso_like_duration_limits.max_arrival_jd, 178, tolerance);

}
