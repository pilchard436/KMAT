#include "unit_tests.h"

static double seed_fixture_cost(const PoweredFlybySeed *seed, void *context) {
    if(context != NULL) return HUGE_VAL;
    /* The cheapest estimate is infeasible; the next date is physically usable. */
    return seed->dt == 10 ? HUGE_VAL : 42;
}

static int check_seed_fallback(void) {
    PoweredSeedBucket bucket = {0};
    retain_powered_seed(&bucket, (PoweredFlybySeed){.dt = 30, .estimated_burn = 3});
    retain_powered_seed(&bucket, (PoweredFlybySeed){.dt = 10, .estimated_burn = 1});
    retain_powered_seed(&bucket, (PoweredFlybySeed){.dt = 20, .estimated_burn = 2});
    retain_powered_seed(&bucket, (PoweredFlybySeed){.dt = 10, .estimated_burn = 1});
    retain_powered_seed(&bucket, (PoweredFlybySeed){.dt = 40, .estimated_burn = 4});
    PoweredFlybySeed selected;
    double cost;
    unsigned long long evaluations = 0;
    if(bucket.count != 3 || !select_powered_seed(&bucket, seed_fixture_cost, NULL,
            &selected, &cost, &evaluations) || selected.dt != 20 || cost != 42 || evaluations != 2) {
        printf("FAIL: powered seed fallback must survive an infeasible cheapest estimate\n");
        return 1;
    }
    evaluations = 0;
    if(select_powered_seed(&bucket, seed_fixture_cost, &bucket,
            &selected, &cost, &evaluations) || evaluations != POWERED_SEEDS_PER_BUCKET) {
        printf("FAIL: infeasible seed bucket must stop at its evaluation bound\n");
        return 1;
    }
    return 0;
}

void run_powered_flyby_tests(void) {
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    (void)pi;
    (void)tolerance;
    Body solar_central_body = {.mu = 1.3271244004193938e20};
    check_true("powered seed fallback survives an infeasible cheapest estimate", check_seed_fallback() == 0);

    Body powered_flyby_body = {
        .mu = 3.9860043543609598e14,
        .radius = 6371000,
        .orbit = {.a = 1.5e11, .cb = &solar_central_body}
    };
    double flyby_minimum_radius = powered_flyby_body.radius + 10000;
    Vector3 incoming_vinf = vec3(6000, 0, 0);
    double flyby_turn_angle = pi / 3;
    Vector3 equal_speed_outgoing_vinf = vec3(6000 * cos(flyby_turn_angle), 6000 * sin(flyby_turn_angle), 0);
    PoweredFlybyResult powered_flyby_result;
    int equal_speed_flyby_valid = calculate_powered_flyby(incoming_vinf, equal_speed_outgoing_vinf,
        vec3(0, 0, 0), &powered_flyby_body, flyby_minimum_radius, &powered_flyby_result);
    expect_int("equal-speed powered-flyby model is feasible", equal_speed_flyby_valid, 1);
    expect_near("equal-speed powered flyby requires effectively zero burn",
                            powered_flyby_result.burn_delta_v, 0, 1e-3);
    Vector3 near_equal_speed_outgoing_vinf = vec3(6000.5 * cos(flyby_turn_angle),
        6000.5 * sin(flyby_turn_angle), 0);
    int near_equal_speed_flyby_valid = calculate_powered_flyby(incoming_vinf,
        near_equal_speed_outgoing_vinf, vec3(0, 0, 0), &powered_flyby_body,
        flyby_minimum_radius, &powered_flyby_result);
    expect_int("sub-1 m/s speed difference retains a powered burn",
        near_equal_speed_flyby_valid && powered_flyby_result.burn_delta_v > 1e-3, 1);
    // Near-equal speeds in either direction must stay feasible with a small burn, not a one-periapsis overestimate.
    for(int near_equal_case = 0; near_equal_case < 4; near_equal_case++) {
        static const double speed_offsets[] = {-0.9, -0.5, 0.5, 0.9};
        double near_speed = 6000 + speed_offsets[near_equal_case];
        Vector3 near_outgoing = vec3(near_speed * cos(flyby_turn_angle), near_speed * sin(flyby_turn_angle), 0);
        PoweredFlybyResult near_result;
        int near_valid = calculate_powered_flyby(incoming_vinf, near_outgoing, vec3(0, 0, 0),
            &powered_flyby_body, flyby_minimum_radius, &near_result);
        expect_int("near-equal-speed flyby is feasible in either direction", near_valid, 1);
        expect_int("near-equal-speed flyby burn stays small",
            near_valid && near_result.burn_delta_v < 5.0, 1);
    }
    double flyby_inclination = deg2rad(20);
    Vector3 unequal_speed_outgoing_vinf = vec3(6500 * cos(flyby_turn_angle),
        6500 * sin(flyby_turn_angle) * cos(flyby_inclination),
        6500 * sin(flyby_turn_angle) * sin(flyby_inclination));
    int unequal_speed_flyby_valid = calculate_powered_flyby(incoming_vinf, unequal_speed_outgoing_vinf,
        vec3(0, 0, 0), &powered_flyby_body, flyby_minimum_radius, &powered_flyby_result);
    expect_int("unequal-speed powered flyby is feasible with finite burn",
        unequal_speed_flyby_valid && isfinite(powered_flyby_result.burn_delta_v) && powered_flyby_result.burn_delta_v > 0, 1);
    expect_near("powered flyby burn vector matches reported magnitude",
        mag_vec3(powered_flyby_result.burn_delta_v_vector), powered_flyby_result.burn_delta_v, 1e-8);
    Vector3 flyby_normal = norm_vec3(cross_vec3(powered_flyby_result.periapsis_direction,
        powered_flyby_result.incoming_tangent_direction));
    expect_near("powered periapsis burn has no radial component",
        dot_vec3(powered_flyby_result.burn_delta_v_vector,
                 powered_flyby_result.periapsis_direction), 0, 1e-8);
    expect_int("powered periapsis burn supports a normal component",
        fabs(dot_vec3(powered_flyby_result.burn_delta_v_vector, flyby_normal)) > 1e-3, 1);
    expect_near("powered flyby incoming state reaches SOI radius",
        mag_vec3(powered_flyby_result.incoming_soi_state.r), powered_flyby_result.soi_radius, 1e-3);
    expect_near("powered flyby outgoing state reaches SOI radius",
        mag_vec3(powered_flyby_result.outgoing_soi_state.r), powered_flyby_result.soi_radius, 1e-3);
    expect_int("powered flyby SOI travel times are positive",
        powered_flyby_result.incoming_soi_time > 0 && powered_flyby_result.outgoing_soi_time > 0, 1);
    expect_int("powered flyby SOI states point inward then outward",
        dot_vec3(powered_flyby_result.incoming_soi_state.r,
                 powered_flyby_result.incoming_soi_state.v) < 0 &&
        dot_vec3(powered_flyby_result.outgoing_soi_state.r,
                 powered_flyby_result.outgoing_soi_state.v) > 0, 1);
    expect_int("powered flyby SOI boundary states are finite",
        isfinite(powered_flyby_result.incoming_soi_state.v.x) &&
        isfinite(powered_flyby_result.incoming_soi_state.v.y) &&
        isfinite(powered_flyby_result.incoming_soi_state.v.z) &&
        isfinite(powered_flyby_result.outgoing_soi_state.v.x) &&
        isfinite(powered_flyby_result.outgoing_soi_state.v.y) &&
        isfinite(powered_flyby_result.outgoing_soi_state.v.z), 1);
    expect_int("powered flyby vector components are finite",
        isfinite(powered_flyby_result.burn_delta_v_vector.x) &&
        isfinite(powered_flyby_result.burn_delta_v_vector.y) &&
        isfinite(powered_flyby_result.burn_delta_v_vector.z), 1);

    DepartureBurn3D departure_burn;
    Vector3 departure_vinf = vec3(3000, 4000, 1000);
    double departure_periapsis_radius = powered_flyby_body.radius + 200000;
    int departure_burn_valid = calculate_free_plane_departure_burn(departure_vinf,
        &powered_flyby_body, departure_periapsis_radius, &departure_burn);
    expect_int("free-plane initial burn returns a finite 3D vector",
        departure_burn_valid && isfinite(departure_burn.burn_delta_v.x) &&
        isfinite(departure_burn.burn_delta_v.y) && isfinite(departure_burn.burn_delta_v.z), 1);
    expect_near("free-plane initial vector matches scalar departure delta-v",
        mag_vec3(departure_burn.burn_delta_v),
        dv_circ(&powered_flyby_body, departure_periapsis_radius, mag_vec3(departure_vinf)), 1e-8);
    expect_near("free-plane initial periapsis direction is normalized",
        mag_vec3(departure_burn.periapsis_direction), 1, 1e-12);
    Vector3 departure_orbit_normal = norm_vec3(cross_vec3(departure_burn.periapsis_direction,
        departure_burn.tangential_direction));
    expect_near("free-plane initial burn has no radial component",
        dot_vec3(departure_burn.burn_delta_v, departure_burn.periapsis_direction), 0, 1e-8);
    expect_near("free-plane initial burn has no normal component",
        dot_vec3(departure_burn.burn_delta_v, departure_orbit_normal), 0, 1e-8);

}
