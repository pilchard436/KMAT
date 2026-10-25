#include "unit_tests.h"

static int cancel_requested(void *context) {
    return *(int *)context;
}

static void run_orbit_propagation_tests(void) {
    Body sun = {.mu = 1.3271244004193938e20};
    double eccentricities[] = {0, 0.0167, 0.2, 0.6, 0.9, 0.97};
    double worst_time_error = 0, worst_period_error = 0;
    for(int e_index = 0; e_index < 6; e_index++) {
        Orbit orbit = constr_orbit_from_elements(2.279e11, eccentricities[e_index], 0.03, 0.8, 5.0, 1.0, &sun);
        double period = calc_orbital_period(orbit);
        double start_time = calc_orbit_time_since_periapsis(orbit);
        for(int step = -40; step <= 400; step++) {
            double dt = step * period / 37;
            double expected = fmod(fmod(start_time + dt, period) + period, period);
            double actual = calc_orbit_time_since_periapsis(propagate_orbit_time(orbit, dt));
            double error = fabs(actual - expected);
            if(error > period / 2) error = period - error;
            if(error > worst_time_error) worst_time_error = error;
        }
        double period_error = fabs(pi_norm(propagate_orbit_time(orbit, period).ta - orbit.ta));
        if(period_error > M_PI) period_error = 2 * M_PI - period_error;
        if(period_error > worst_period_error) worst_period_error = period_error;
    }
    expect_near("elliptic propagation reaches the requested time since periapsis", worst_time_error, 0, 1e-2);
    expect_near("elliptic propagation over one period returns the true anomaly", worst_period_error, 0, 1e-6);
}

void run_lambert_tests(void) {
    run_orbit_propagation_tests();
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    (void)pi;
    (void)tolerance;
    Body earth_cb = {.mu = 3.9860043543609598e14};
    double start_radius = 7000000;
    double end_radius = 8000000;
    double transfer_semi_major_axis = (start_radius + end_radius) / 2;
    double hohmann_time = pi * sqrt(pow(transfer_semi_major_axis, 3) / earth_cb.mu);
    Lambert3 hohmann_transfer = calc_lambert3(
        vec3(start_radius, 0, 0), vec3(0, end_radius, 0), hohmann_time, &earth_cb);
    OSV hohmann_start = {vec3(start_radius, 0, 0), hohmann_transfer.v0};
    double hohmann_miss = mag_vec3(subtract_vec3(propagate_osv_time(hohmann_start, &earth_cb, hohmann_time).r, vec3(0, end_radius, 0)));
    expect_int("Lambert Hohmann transfer converges", hohmann_transfer.success, LAMBERT_SUCCESS);
    expect_int("Lambert Hohmann velocities are finite",
                           isfinite(hohmann_transfer.v0.x) && isfinite(hohmann_transfer.v0.y) &&
                           isfinite(hohmann_transfer.v0.z) && isfinite(hohmann_transfer.v1.x) &&
                           isfinite(hohmann_transfer.v1.y) && isfinite(hohmann_transfer.v1.z), 1);
    expect_near("Lambert transfer propagates to the target position", hohmann_miss, 0, 8000);
    int lambert_cancel_requested = 1;
    Lambert3 cancelled_transfer = calc_lambert3_cancellable(
        vec3(start_radius, 0, 0), vec3(0, end_radius, 0), hohmann_time, &earth_cb,
        cancel_requested, &lambert_cancel_requested);
    expect_int("cancellable Lambert solver stops before solving",
                           cancelled_transfer.success, LAMBERT_FAIL_NAN);

    Body solar_central_body = {.mu = 1.3271244004193938e20};
    int izzo_corpus_closes = 1;
    for(int sample_index = 0; sample_index < 96; sample_index++) {
        double sample_start_radius = 1e10 * (1 + (sample_index % 17) * 0.11);
        double sample_end_radius = sample_start_radius * (1.08 + (sample_index % 11) * 0.047);
        double transfer_angle = 0.18 + (sample_index % 120) * 0.045;
		double inclination = deg2rad((sample_index % 15) - 7);
        double transfer_semimajor_axis = (sample_start_radius + sample_end_radius) / 2;
        double baseline_duration = pi * sqrt(pow(transfer_semimajor_axis, 3) / solar_central_body.mu);
        double sample_duration = baseline_duration * (0.4 + (sample_index % 100) * 0.012);
        Vector3 sample_r0 = vec3(sample_start_radius, 0, 0);
        Vector3 sample_r1 = vec3(sample_end_radius * cos(transfer_angle),
                     sample_end_radius * sin(transfer_angle) * cos(inclination),
                     sample_end_radius * sin(transfer_angle) * sin(inclination));
        Lambert3 izzo_solution = calc_lambert3(sample_r0, sample_r1, sample_duration, &solar_central_body);
        OSV initial_state = {sample_r0, izzo_solution.v0};
        OSV propagated_arrival = propagate_osv_time(initial_state, &solar_central_body, sample_duration);
        double relative_endpoint_miss = mag_vec3(subtract_vec3(propagated_arrival.r, sample_r1)) /
            mag_vec3(sample_r1);
        if(izzo_solution.success != LAMBERT_SUCCESS || !isfinite(relative_endpoint_miss) ||
            relative_endpoint_miss > 1e-4)
            izzo_corpus_closes = 0;
    }
    expect_int("Izzo solver reaches endpoints across representative inclined corpus",
                           izzo_corpus_closes, 1);

    int revolution_solutions = 0, revolution_misses = 0, branches_differ = 0;
    for(int sample_index = 0; sample_index < 120; sample_index++) {
        int revolutions = 1 + sample_index % 3;
        double sample_start_radius = 1.5e11 * (1 + (sample_index % 7) * 0.08);
        double sample_end_radius = sample_start_radius * (0.9 + (sample_index % 5) * 0.1);
        double transfer_angle = 0.3 + (sample_index % 11) * 0.55;
        double circular_period = 2 * pi * sqrt(pow(sample_start_radius, 3) / solar_central_body.mu);
        double sample_duration = circular_period * (revolutions + 0.2 + (sample_index % 9) * 0.12);
        Vector3 sample_r0 = vec3(sample_start_radius, 0, 0);
        Vector3 sample_r1 = vec3(sample_end_radius * cos(transfer_angle),
                     sample_end_radius * sin(transfer_angle), 0);
        Lambert3 branch_solutions[2];
        for(int branch = 0; branch < 2; branch++) {
            branch_solutions[branch] = calc_lambert3_revs(sample_r0, sample_r1, sample_duration,
                &solar_central_body, revolutions, branch);
            if(branch_solutions[branch].success != LAMBERT_SUCCESS) continue;
            revolution_solutions++;
            OSV arrival = propagate_osv_time((OSV){sample_r0, branch_solutions[branch].v0},
                &solar_central_body, sample_duration);
            if(!(mag_vec3(subtract_vec3(arrival.r, sample_r1)) / sample_end_radius < 1e-4)) revolution_misses++;
        }
        if(branch_solutions[0].success == LAMBERT_SUCCESS && branch_solutions[1].success == LAMBERT_SUCCESS &&
            mag_vec3(subtract_vec3(branch_solutions[0].v0, branch_solutions[1].v0)) > 1) branches_differ++;
    }
    expect_int("multi-revolution Lambert finds solutions", revolution_solutions > 40, 1);
    expect_int("multi-revolution Lambert solutions reach the endpoint", revolution_misses, 0);
    expect_int("multi-revolution Lambert left and right branches differ", branches_differ > 10, 1);
    Lambert3 too_fast = calc_lambert3_revs(vec3(1.5e11, 0, 0), vec3(0, 1.5e11, 0), 1e6,
        &solar_central_body, 2, 0);
    expect_int("multi-revolution Lambert rejects a time below the minimum",
                           too_fast.success != LAMBERT_SUCCESS, 1);

}
