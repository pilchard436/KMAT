#include "unit_tests.h"

int main(void) {
    run_math_tests();
    run_lambert_tests();
    run_powered_flyby_tests();
    run_starting_orbit_tests();
    run_formats_tests();
    run_porkchop_tests();
    run_reference_defaults_tests();
    return finish_tests("KMAT unit checks");
}
