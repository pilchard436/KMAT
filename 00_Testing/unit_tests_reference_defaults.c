#include "unit_tests.h"

void run_reference_defaults_tests(void) {
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    (void)pi;
    (void)tolerance;
    Body sun = {.mu = 1.3271244004193938e20, .radius = 696342000};
    Body earth_orbit = {
        .mu = 3.9860043543609598e14,
        .radius = 6371000,
        .atmo_alt = 140000,
        .orbit = {.a = 149598261150.4425}
    };
    Body mars_orbit = {
        .mu = 4.282837362069909e13,
        .radius = 3375800,
        .atmo_alt = 110000,
        .orbit = {.a = 227949699961.9763}
    };
    ItineraryHohmannReference reference = {0};
    expect_int("Hohmann reference accepts valid bodies",
                           calculate_itinerary_hohmann_reference(&earth_orbit, &mars_orbit, &sun, 50000, 50000, &reference), 1);
    expect_int("Hohmann reference has finite positive duration", reference.duration_seconds > 0, 1);
    expect_int("Hohmann circularization exceeds capture", reference.arrival_circularization_dv > reference.arrival_capture_dv, 1);
    expect_int("Hohmann reference rejects identical bodies",
                           calculate_itinerary_hohmann_reference(&earth_orbit, &earth_orbit, &sun, 50000, 50000, &reference), 0);
    expect_int("Hohmann reference rejects missing central body",
                           calculate_itinerary_hohmann_reference(&earth_orbit, &mars_orbit, NULL, 50000, 50000, &reference), 0);
    expect_int("Hohmann reference rejects non-positive central gravity",
                           calculate_itinerary_hohmann_reference(&earth_orbit, &mars_orbit, &(Body){0}, 50000, 50000, &reference), 0);

    ItineraryHohmannReference sample_reference = {
        .duration_seconds = 10 * 86400.0,
        .departure_dv = 1200,
        .arrival_capture_dv = 400,
        .arrival_circularization_dv = 600
    };
    ItineraryHohmannDefaults defaults;
    expect_int("flyby Hohmann defaults calculate",
                           calculate_itinerary_hohmann_defaults(&sample_reference, TF_FLYBY, DATE_ISO, 2450000, &defaults), 1);
    expect_near("Hohmann default duration is twice baseline", defaults.max_duration_user_days, 20, tolerance);
    expect_near("Hohmann default arrival follows earliest departure", defaults.max_arrival_jd, 2450020, tolerance);
    expect_near("Hohmann default departure cap includes 10 percent margin", defaults.max_departure_dv, 1320, tolerance);
    expect_near("flyby mode has no arrival insertion budget", defaults.max_satellite_dv, 0, tolerance);
    expect_near("flyby total cap includes 10 percent margin", defaults.max_total_dv, 1320, tolerance);

    calculate_itinerary_hohmann_defaults(&sample_reference, TF_CAPTURE, DATE_ISO, 2450000, &defaults);
    expect_near("capture mode satellite cap uses capture baseline", defaults.max_satellite_dv, 400, tolerance);
    expect_near("capture total cap includes 10 percent margin", defaults.max_total_dv, 1760, tolerance);
    calculate_itinerary_hohmann_defaults(&sample_reference, TF_CIRC, DATE_ISO, 2450000, &defaults);
    expect_near("circularization mode satellite cap uses circular baseline", defaults.max_satellite_dv, 600, tolerance);
    expect_near("circularization total cap includes 10 percent margin", defaults.max_total_dv, 1980, tolerance);

    calculate_itinerary_hohmann_defaults(&sample_reference, TF_FLYBY, DATE_KERBAL, 2450000, &defaults);
    expect_near("Kerbal Hohmann window converts to local days", defaults.max_duration_user_days, 80, tolerance);
    expect_near("Kerbal Hohmann arrival preserves physical duration", defaults.max_arrival_jd, 2450020, tolerance);

}
