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

    struct CelestSystem system = {.ut0 = 0};
    sun.system = &system;
    double worst_position_error = 0, worst_velocity_error = 0;
    for(int e_index = 0; e_index < 6; e_index++) {
        Orbit orbit = constr_orbit_from_elements(2.279e11, eccentricities[e_index], 0.03, 0.8, 5.0, 1.0, &sun);
        for(int day = -300; day <= 3000; day += 17) {
            OSV fast = osv_from_elements(orbit, day);
            OSV reference = osv_from_orbit(propagate_orbit_time(orbit, day * 86400.0));
            double position_error = mag_vec3(subtract_vec3(fast.r, reference.r)) / mag_vec3(reference.r);
            double velocity_error = mag_vec3(subtract_vec3(fast.v, reference.v)) / mag_vec3(reference.v);
            if(position_error > worst_position_error) worst_position_error = position_error;
            if(velocity_error > worst_velocity_error) worst_velocity_error = velocity_error;
        }
    }
    expect_near("osv_from_elements matches propagated orbit position", worst_position_error, 0, 1e-5);
    expect_near("osv_from_elements matches propagated orbit velocity", worst_velocity_error, 0, 1e-5);

    // Elements at an arbitrary epoch, more distinct orbits than the per-thread element cache holds.
    double worst_after_error = 0;
    for(int orbit_index = 0; orbit_index < 40; orbit_index++) {
        Orbit orbit = constr_orbit_from_elements(1.0e11 * (1 + 0.05 * orbit_index), 0.02 * (orbit_index % 9),
            0.01 * orbit_index, 0.3 * orbit_index, 0.7 * orbit_index, 0.11 * orbit_index, &sun);
        for(int step = 0; step < 5; step++) {
            double dt = (step - 2) * 2.3e7;
            OSV fast = osv_from_elements_after(&orbit, dt);
            OSV reference = osv_from_orbit(propagate_orbit_time(orbit, dt));
            double error = mag_vec3(subtract_vec3(fast.r, reference.r)) / mag_vec3(reference.r);
            if(error > worst_after_error) worst_after_error = error;
        }
    }
    expect_near("osv_from_elements_after propagates from the elements' epoch", worst_after_error, 0, 1e-5);

    // Ephemeris lookup: nearest entry (ties to the later one), then propagation from that entry.
    Orbit ephem_orbit = constr_orbit_from_elements(1.5e11, 0.1, 0.05, 0.4, 1.2, 0.9, &sun);
    Ephem ephem_list[12];
    for(int i = 0; i < 12; i++) {
        double epoch = 1000 + 30 * i;
        OSV state = osv_from_elements(ephem_orbit, epoch);
        ephem_list[i] = (Ephem){.epoch = epoch, .r = state.r, .v = state.v};
    }
    double probes[] = {900, 1000, 1014, 1015, 1016, 1157.2, 1330, 1800};
    int expected_index[] = {0, 0, 0, 1, 1, 5, 11, 11};
    int nearest_matches = 1;
    double worst_ephem_error = 0;
    for(int probe = 0; probe < 8; probe++) {
        Ephem nearest = get_closest_ephem(ephem_list, 12, probes[probe]);
        nearest_matches &= nearest.epoch == ephem_list[expected_index[probe]].epoch;
        OSV from_ephem = osv_from_ephem(ephem_list, 12, probes[probe], &sun);
        OSV reference = propagate_osv_time((OSV){nearest.r, nearest.v}, &sun, (probes[probe] - nearest.epoch) * 86400);
        double error = mag_vec3(subtract_vec3(from_ephem.r, reference.r)) / mag_vec3(reference.r);
        if(error > worst_ephem_error) worst_ephem_error = error;
    }
    expect_int("ephemeris lookup picks the nearest entry", nearest_matches, 1);
    expect_near("osv_from_ephem propagates the nearest entry", worst_ephem_error, 0, 1e-5);
    // Same entry address with new contents must not reuse the cached elements.
    OSV before_change = osv_from_ephem(ephem_list, 12, 1157.2, &sun);
    ephem_list[5].r = scale_vec3(ephem_list[5].r, 1.01);
    OSV after_change = osv_from_ephem(ephem_list, 12, 1157.2, &sun);
    expect_int("osv_from_ephem notices changed ephemeris contents",
        mag_vec3(subtract_vec3(before_change.r, after_change.r)) > 1e6, 1);
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

    int pair_matches = 1, pair_solutions = 0;
    for(int sample_index = 0; sample_index < 120; sample_index++) {
        int revolutions = 1 + sample_index % 3;
        double sample_start_radius = 1.5e11 * (1 + (sample_index % 7) * 0.08);
        double sample_end_radius = sample_start_radius * (0.9 + (sample_index % 5) * 0.1);
        double transfer_angle = 0.3 + (sample_index % 11) * 0.55;
        double circular_period = 2 * pi * sqrt(pow(sample_start_radius, 3) / solar_central_body.mu);
        double sample_duration = circular_period * (revolutions + 0.2 + (sample_index % 9) * 0.12);
        Vector3 sample_r0 = vec3(sample_start_radius, 0, 0);
        Vector3 sample_r1 = vec3(sample_end_radius * cos(transfer_angle), sample_end_radius * sin(transfer_angle), 0);
        Lambert3 pair[2];
        calc_lambert3_revs_pair_cancellable(sample_r0, sample_r1, sample_duration, &solar_central_body,
            revolutions, &pair[0], &pair[1], NULL, NULL);
        for(int branch = 0; branch < 2; branch++) {
            Lambert3 single = calc_lambert3_revs(sample_r0, sample_r1, sample_duration, &solar_central_body,
                revolutions, branch);
            pair_matches &= single.success == pair[branch].success;
            if(single.success != LAMBERT_SUCCESS) continue;
            pair_solutions++;
            pair_matches &= mag_vec3(subtract_vec3(single.v0, pair[branch].v0)) == 0 &&
                mag_vec3(subtract_vec3(single.v1, pair[branch].v1)) == 0;
        }
    }
    expect_int("paired multi-revolution Lambert equals the single-branch solutions", pair_matches && pair_solutions > 40, 1);
    Lambert3 pair_direct[2];
    calc_lambert3_revs_pair_cancellable(vec3(start_radius, 0, 0), vec3(0, end_radius, 0), hohmann_time, &earth_cb,
        0, &pair_direct[0], &pair_direct[1], NULL, NULL);
    expect_int("paired solve with zero revolutions gives the direct arc and no right branch",
        pair_direct[0].success == LAMBERT_SUCCESS &&
        mag_vec3(subtract_vec3(pair_direct[0].v0, hohmann_transfer.v0)) == 0 &&
        pair_direct[1].success != LAMBERT_SUCCESS, 1);

}
