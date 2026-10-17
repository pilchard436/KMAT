#include "unit_tests.h"

static double test_random(unsigned long long *state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(*state >> 11) / 9007199254740992.0;
}


/* Recomputes the outgoing asymptote from a state vector alone, independent of the burn solver:
 * v_inf_vector = (v_inf / e^2) * (-E + (v_inf / mu) * (h x E)). */
static Vector3 asymptote_from_state(Vector3 r, Vector3 v, double mu, double v_inf) {
    Vector3 h = cross_vec3(r, v);
    Vector3 e_vector = subtract_vec3(scale_vec3(cross_vec3(v, h), 1 / mu), scale_vec3(r, 1 / mag_vec3(r)));
    double e_squared = dot_vec3(e_vector, e_vector);
    Vector3 inner = add_vec3(scale_vec3(e_vector, -1), scale_vec3(cross_vec3(h, e_vector), v_inf / mu));
    return scale_vec3(inner, v_inf / e_squared);
}

static void check_starting_orbit(void) {
    const double pi = 3.14159265358979323846;
#define STARTING_CHECK(call) do { (void)(call); } while(0)
    Body body = {.mu = 3.986004418e14, .radius = 6371000};
    double r0 = body.radius + 300000;
    double v_inf = 4000;
    double expected_free = dv_circ(&body, r0, v_inf);
    double burn, anomaly;

    StartingOrbit circular = starting_orbit_circular(r0);
    Vector3 in_plane = vec3(v_inf * cos(0.7), v_inf * sin(0.7), 0);
    STARTING_CHECK(expect_int("equatorial circular starting orbit reaches an in-plane asymptote",
        starting_orbit_best_departure_burn(&circular, &body, in_plane, &burn, &anomaly), 1));
    STARTING_CHECK(expect_near("in-plane departure from a circular orbit matches dv_circ", burn, expected_free, 1e-3));

    StartingOrbit tilted = circular;
    tilted.inclination = deg2rad(60);
    STARTING_CHECK(expect_int("inclined circular orbit reaches an asymptote along its line of nodes",
        starting_orbit_best_departure_burn(&tilted, &body, vec3(v_inf, 0, 0), &burn, &anomaly), 1));
    STARTING_CHECK(expect_near("asymptote inside the orbit plane matches dv_circ", burn, expected_free, 1e-3));

    /* Out of plane the new velocity is perpendicular to the circular velocity: burn = sqrt(vc^2 + vh^2). */
    double circular_speed = sqrt(body.mu / r0);
    double hyperbolic_speed = sqrt(v_inf * v_inf + 2 * body.mu / r0);
    STARTING_CHECK(expect_int("equatorial orbit reaches a polar asymptote",
        starting_orbit_best_departure_burn(&circular, &body, vec3(0, 0, v_inf), &burn, &anomaly), 1));
    STARTING_CHECK(expect_near("polar asymptote from an equatorial orbit costs sqrt(vc^2 + vh^2)", burn,
        sqrt(circular_speed * circular_speed + hyperbolic_speed * hyperbolic_speed), 1e-3));
    STARTING_CHECK(expect_int("out-of-plane departure costs more than the free-plane burn", burn > expected_free + 100, 1));

    StartingOrbit invalid = circular;
    invalid.eccentricity = 1;
    STARTING_CHECK(expect_int("eccentricity 1 is rejected", starting_orbit_is_valid(&invalid, &body), 0));
    invalid = circular;
    invalid.semi_major_axis = body.radius * 0.9;
    STARTING_CHECK(expect_int("periapsis inside the body is rejected", starting_orbit_is_valid(&invalid, &body), 0));
    invalid = circular;
    invalid.inclination = NAN;
    STARTING_CHECK(expect_int("non-finite elements are rejected", starting_orbit_is_valid(&invalid, &body), 0));
    STARTING_CHECK(expect_int("a circular orbit above the surface is accepted", starting_orbit_is_valid(&circular, &body), 1));

    unsigned long long seed = 12345;
    int solved = 0, speed_violations = 0, asymptote_violations = 0, bound_violations = 0,
        clearance_violations = 0, best_violations = 0;
    double worst_speed_error = 0, worst_angle_error = 0;
    for(int c = 0; c < 300; c++) {
        double periapsis = body.radius + 3e5 + test_random(&seed) * 2e7;
        double eccentricity = test_random(&seed) * 0.7;
        StartingOrbit orbit = {.semi_major_axis = periapsis / (1 - eccentricity), .eccentricity = eccentricity,
            .inclination = test_random(&seed) * pi, .raan = test_random(&seed) * 2 * pi,
            .arg_periapsis = test_random(&seed) * 2 * pi, .true_anomaly = test_random(&seed) * 2 * pi};
        double z = 2 * test_random(&seed) - 1, phi = test_random(&seed) * 2 * pi, s = sqrt(1 - z * z);
        Vector3 direction = vec3(s * cos(phi), s * sin(phi), z);
        double excess = 1000 + test_random(&seed) * 6000;
        double at_anomaly = test_random(&seed) * 2 * pi;
        double magnitude;
        Vector3 burn_vector;
        OSV starting;
        if(!starting_orbit_departure_burn_at(&orbit, &body, at_anomaly, scale_vec3(direction, excess),
            &magnitude, &burn_vector) || !starting_orbit_state_at(&orbit, &body, at_anomaly, &starting)) continue;
        solved++;
        Vector3 velocity = add_vec3(starting.v, burn_vector);
        double radius = mag_vec3(starting.r);
        double target_speed_squared = excess * excess + 2 * body.mu / radius;
        double speed_error = fabs(dot_vec3(velocity, velocity) - target_speed_squared) / target_speed_squared;
        Vector3 asymptote = asymptote_from_state(starting.r, velocity, body.mu, excess);
        double cosine = dot_vec3(norm_vec3(asymptote), direction);
        double angle_error = acos(fmax(-1, fmin(1, cosine)));
        worst_speed_error = fmax(worst_speed_error, speed_error);
        worst_angle_error = fmax(worst_angle_error, angle_error);
        speed_violations += speed_error > 1e-9;
        asymptote_violations += !(angle_error < 1e-6);
        bound_violations += magnitude + 1e-6 < fabs(mag_vec3(velocity) - mag_vec3(starting.v)) ||
            fabs(magnitude - mag_vec3(burn_vector)) > 1e-9;
        Vector3 angular_momentum = cross_vec3(starting.r, velocity);
        double hyperbola_periapsis = dot_vec3(angular_momentum, angular_momentum) / body.mu /
            (1 + mag_vec3(subtract_vec3(scale_vec3(cross_vec3(velocity, angular_momentum), 1 / body.mu),
                scale_vec3(starting.r, 1 / radius))));
        clearance_violations += dot_vec3(starting.r, velocity) < 0 && hyperbola_periapsis < body.radius * (1 - 1e-9);
        double best, best_at;
        if(starting_orbit_best_departure_burn(&orbit, &body, scale_vec3(direction, excess), &best, &best_at))
            best_violations += best > magnitude + 1e-6;
    }
    STARTING_CHECK(expect_int("most random starting-orbit departures are solvable", solved >= 200, 1));
    STARTING_CHECK(expect_int("solved burns reach the required speed", speed_violations, 0));
    STARTING_CHECK(expect_int("solved burns reach the required asymptote (independent check)", asymptote_violations, 0));
    STARTING_CHECK(expect_int("burns respect the speed-change lower bound", bound_violations, 0));
    STARTING_CHECK(expect_int("hyperbolas never pass below the surface", clearance_violations, 0));
    STARTING_CHECK(expect_int("the best burn is never worse than a burn at a fixed anomaly", best_violations, 0));
    STARTING_CHECK(expect_near("worst relative speed error", worst_speed_error, 0, 1e-9));
    STARTING_CHECK(expect_near("worst asymptote angle error in radians", worst_angle_error, 0, 1e-6));

    double dv;
    starting_orbit_set_active(NULL, NULL);
    STARTING_CHECK(expect_int("without a starting orbit the circular burn is used",
        starting_orbit_departure_dv(&body, vec3(0, 0, v_inf), r0, &dv), 1));
    STARTING_CHECK(expect_near("circular default matches dv_circ", dv, expected_free, 1e-6));
    starting_orbit_set_active(&circular, &body);
    starting_orbit_departure_dv(&body, vec3(0, 0, v_inf), r0, &dv);
    STARTING_CHECK(expect_near("an active orbit changes the departure burn", dv,
        sqrt(circular_speed * circular_speed + hyperbolic_speed * hyperbolic_speed), 1e-3));
    Body other_body = body;
    starting_orbit_departure_dv(&other_body, vec3(0, 0, v_inf), r0, &dv);
    STARTING_CHECK(expect_near("an active orbit does not apply to another body", dv, expected_free, 1e-6));
    starting_orbit_set_active(NULL, NULL);
    starting_orbit_departure_dv(&body, vec3(0, 0, v_inf), r0, &dv);
    STARTING_CHECK(expect_near("clearing the active orbit restores the default", dv, expected_free, 1e-6));
#undef STARTING_CHECK
}

void run_starting_orbit_tests(void) {
    check_starting_orbit();
}
