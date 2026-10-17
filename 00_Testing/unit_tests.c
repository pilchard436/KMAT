#include "unit_tests.h"

int run_itinerary_unit_tests(void);
int run_optimizer_unit_tests(void);

// Runs every unit suite and returns 1 if any check failed.
int run_unit_tests(void) {
    run_math_tests();
    run_lambert_tests();
    run_powered_flyby_tests();
    run_starting_orbit_tests();
    run_formats_tests();
    run_porkchop_tests();
    run_reference_defaults_tests();
    int failed = finish_tests("KMAT unit checks");
    checks = failures = 0;
    failed |= run_itinerary_unit_tests();
    failed |= run_optimizer_unit_tests();
    return failed;
}
