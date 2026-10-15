#include "geometrylib.h"
#include "geometrylib_vec.h"
#include "orbitlib_datetime.h"
#include "orbitlib_transfer.h"
#include "tools/tool_funcs.h"
#include "tools/file_io.h"
#include "tools/itinerary_date_limits.h"
#include "tools/itinerary_reference_defaults.h"
#include "gui/transfer_app/porkchop_analyzer_model.h"
#include "orbit_calculator/powered_seed_selection.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static int expect_near(const char *name, double actual, double expected, double tolerance) {
    if (fabs(actual - expected) <= tolerance) return 0;

    printf("FAIL: %s (expected %.12f, got %.12f)\n", name, expected, actual);
    return 1;
}

static int expect_vector3(const char *name, Vector3 actual, Vector3 expected, double tolerance) {
    return expect_near(name, actual.x, expected.x, tolerance)
           + expect_near(name, actual.y, expected.y, tolerance)
           + expect_near(name, actual.z, expected.z, tolerance);
}

static int expect_datetime(const char *name, Datetime actual, Datetime expected, double tolerance) {
    int failures = 0;
    failures += actual.y != expected.y;
    failures += actual.m != expected.m;
    failures += actual.d != expected.d;
    failures += actual.h != expected.h;
    failures += actual.min != expected.min;
    failures += actual.date_type != expected.date_type;
    failures += expect_near(name, actual.s, expected.s, tolerance);
    if (failures > 0) printf("FAIL: %s\n", name);
    return failures;
}

static int expect_contains(const char *name, const char *actual, const char *expected) {
    if (strstr(actual, expected) != NULL) return 0;

    printf("FAIL: %s (missing '%s')\n", name, expected);
    return 1;
}

static int expect_int(const char *name, int actual, int expected) {
    if (actual == expected) return 0;
    printf("FAIL: %s (expected %d, got %d)\n", name, expected, actual);
    return 1;
}

static int cancel_requested(void *context) {
    return *(int *)context;
}

int main(void) {
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    const int total_checks = 93;
    int failures = check_seed_fallback();

    failures += expect_int("Izzo is the default Lambert solver", get_lambert_solver_method(), LAMBERT_METHOD_IZZO);
    set_lambert_solver_method(LAMBERT_METHOD_LEGACY);
    failures += expect_int("Legacy Lambert solver can be selected", get_lambert_solver_method(), LAMBERT_METHOD_LEGACY);
    set_lambert_solver_method(LAMBERT_METHOD_IZZO);
    failures += expect_int("Izzo Lambert solver can be selected", get_lambert_solver_method(), LAMBERT_METHOD_IZZO);

    failures += expect_near("deg2rad", deg2rad(180), pi, tolerance);
    failures += expect_near("rad2deg", rad2deg(pi), 180, tolerance);
    failures += expect_near("pi_norm negative", pi_norm(-pi / 2), 3 * pi / 2, tolerance);
    failures += expect_near("pi_norm full rotation", pi_norm(2 * pi), 0, tolerance);
    failures += expect_near("pi_norm overflow", pi_norm(5 * pi / 2), pi / 2, tolerance);

    Vector3 first = vec3(1, 2, 3);
    Vector3 second = vec3(4, -2, 1);

    failures += expect_vector3("add_vec3", add_vec3(first, second), vec3(5, 0, 4), tolerance);
    failures += expect_vector3("subtract_vec3", subtract_vec3(first, second), vec3(-3, 4, 2), tolerance);
    failures += expect_vector3("scale_vec3", scale_vec3(first, 2), vec3(2, 4, 6), tolerance);
    failures += expect_near("sq_mag_vec3", sq_mag_vec3(first), 14, tolerance);
    failures += expect_near("mag_vec3", mag_vec3(first), sqrt(14), tolerance);
    failures += expect_near("dot_vec3", dot_vec3(first, second), 3, tolerance);
    failures += expect_vector3("cross_vec3", cross_vec3(first, second), vec3(8, 11, -10), tolerance);
    failures += expect_near("angle_vec3_vec3", angle_vec3_vec3(vec3(1, 0, 0), vec3(0, 1, 0)), pi / 2, tolerance);

    DataArray2 *first_root_samples = data_array2_create();
    DataArray2 *second_root_samples = data_array2_create();
    data_array2_insert_new(first_root_samples, vec2(1, 10));
    data_array2_insert_new(second_root_samples, vec2(2, 20));
    failures += expect_int("simultaneous root sample arrays remain independent",
                           data_array2_size(first_root_samples) == 1 &&
                           data_array2_size(second_root_samples) == 1 &&
                           data_array2_get_data(first_root_samples)[0].x == 1 &&
                           data_array2_get_data(second_root_samples)[0].x == 2, 1);
    data_array2_free(first_root_samples);
    data_array2_free(second_root_samples);
	DataArray2 *reused_root_samples = data_array2_create();
    failures += expect_int("thread-local root sample storage is reusable",
						   data_array2_size(reused_root_samples) == 0, 1);
    data_array2_free(reused_root_samples);

    Body earth_cb = {.mu = 3.9860043543609598e14};
    double start_radius = 7000000;
    double end_radius = 8000000;
    double transfer_semi_major_axis = (start_radius + end_radius) / 2;
    double hohmann_time = pi * sqrt(pow(transfer_semi_major_axis, 3) / earth_cb.mu);
    Lambert3 hohmann_transfer = calc_lambert3(
        vec3(start_radius, 0, 0), vec3(0, end_radius, 0), hohmann_time, &earth_cb);
    Lambert3 izzo_transfer = calc_lambert3_izzo(
        vec3(start_radius, 0, 0), vec3(0, end_radius, 0), hohmann_time, &earth_cb);
    failures += expect_int("Lambert Hohmann transfer converges", hohmann_transfer.success, LAMBERT_SUCCESS);
    failures += expect_int("Lambert Hohmann velocities are finite",
                           isfinite(hohmann_transfer.v0.x) && isfinite(hohmann_transfer.v0.y) &&
                           isfinite(hohmann_transfer.v0.z) && isfinite(hohmann_transfer.v1.x) &&
                           isfinite(hohmann_transfer.v1.y) && isfinite(hohmann_transfer.v1.z), 1);
    failures += expect_int("Izzo Lambert Hohmann transfer converges", izzo_transfer.success, LAMBERT_SUCCESS);
    failures += expect_vector3("Izzo Lambert departure velocity matches existing solver",
                               izzo_transfer.v0, hohmann_transfer.v0, 10);
    failures += expect_vector3("Izzo Lambert arrival velocity matches existing solver",
                               izzo_transfer.v1, hohmann_transfer.v1, 10);
    failures += expect_int("Izzo Lambert velocities are finite",
                           isfinite(izzo_transfer.v0.x) && isfinite(izzo_transfer.v0.y) &&
                           isfinite(izzo_transfer.v0.z) && isfinite(izzo_transfer.v1.x) &&
                           isfinite(izzo_transfer.v1.y) && isfinite(izzo_transfer.v1.z), 1);
    int lambert_cancel_requested = 1;
    Lambert3 cancelled_transfer = calc_lambert3_selected_cancellable(
        vec3(start_radius, 0, 0), vec3(0, end_radius, 0), hohmann_time, &earth_cb,
        cancel_requested, &lambert_cancel_requested);
    failures += expect_int("cancellable Lambert solver stops before solving",
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
        Lambert3 izzo_solution = calc_lambert3_izzo(sample_r0, sample_r1, sample_duration, &solar_central_body);
        OSV initial_state = {sample_r0, izzo_solution.v0};
        OSV propagated_arrival = propagate_osv_time(initial_state, &solar_central_body, sample_duration);
        double relative_endpoint_miss = mag_vec3(subtract_vec3(propagated_arrival.r, sample_r1)) /
            mag_vec3(sample_r1);
        if(izzo_solution.success != LAMBERT_SUCCESS || !isfinite(relative_endpoint_miss) ||
            relative_endpoint_miss > 1e-4)
            izzo_corpus_closes = 0;
    }
    failures += expect_int("Izzo solver reaches endpoints across representative inclined corpus",
                           izzo_corpus_closes, 1);

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
    failures += expect_int("equal-speed powered-flyby model is feasible", equal_speed_flyby_valid, 1);
    failures += expect_near("equal-speed powered flyby requires effectively zero burn",
                            powered_flyby_result.burn_delta_v, 0, 1e-3);
    double flyby_inclination = deg2rad(20);
    Vector3 unequal_speed_outgoing_vinf = vec3(6500 * cos(flyby_turn_angle),
        6500 * sin(flyby_turn_angle) * cos(flyby_inclination),
        6500 * sin(flyby_turn_angle) * sin(flyby_inclination));
    int unequal_speed_flyby_valid = calculate_powered_flyby(incoming_vinf, unequal_speed_outgoing_vinf,
        vec3(0, 0, 0), &powered_flyby_body, flyby_minimum_radius, &powered_flyby_result);
    failures += expect_int("unequal-speed powered flyby is feasible with finite burn",
        unequal_speed_flyby_valid && isfinite(powered_flyby_result.burn_delta_v) && powered_flyby_result.burn_delta_v > 0, 1);
    failures += expect_near("powered flyby burn vector matches reported magnitude",
        mag_vec3(powered_flyby_result.burn_delta_v_vector), powered_flyby_result.burn_delta_v, 1e-8);
    Vector3 flyby_normal = norm_vec3(cross_vec3(powered_flyby_result.periapsis_direction,
        powered_flyby_result.incoming_tangent_direction));
    failures += expect_near("powered periapsis burn has no radial component",
        dot_vec3(powered_flyby_result.burn_delta_v_vector,
                 powered_flyby_result.periapsis_direction), 0, 1e-8);
    failures += expect_int("powered periapsis burn supports a normal component",
        fabs(dot_vec3(powered_flyby_result.burn_delta_v_vector, flyby_normal)) > 1e-3, 1);
    failures += expect_near("powered flyby incoming state reaches SOI radius",
        mag_vec3(powered_flyby_result.incoming_soi_state.r), powered_flyby_result.soi_radius, 1e-3);
    failures += expect_near("powered flyby outgoing state reaches SOI radius",
        mag_vec3(powered_flyby_result.outgoing_soi_state.r), powered_flyby_result.soi_radius, 1e-3);
    failures += expect_int("powered flyby SOI travel times are positive",
        powered_flyby_result.incoming_soi_time > 0 && powered_flyby_result.outgoing_soi_time > 0, 1);
    failures += expect_int("powered flyby SOI states point inward then outward",
        dot_vec3(powered_flyby_result.incoming_soi_state.r,
                 powered_flyby_result.incoming_soi_state.v) < 0 &&
        dot_vec3(powered_flyby_result.outgoing_soi_state.r,
                 powered_flyby_result.outgoing_soi_state.v) > 0, 1);
    failures += expect_int("powered flyby SOI boundary states are finite",
        isfinite(powered_flyby_result.incoming_soi_state.v.x) &&
        isfinite(powered_flyby_result.incoming_soi_state.v.y) &&
        isfinite(powered_flyby_result.incoming_soi_state.v.z) &&
        isfinite(powered_flyby_result.outgoing_soi_state.v.x) &&
        isfinite(powered_flyby_result.outgoing_soi_state.v.y) &&
        isfinite(powered_flyby_result.outgoing_soi_state.v.z), 1);
    failures += expect_int("powered flyby vector components are finite",
        isfinite(powered_flyby_result.burn_delta_v_vector.x) &&
        isfinite(powered_flyby_result.burn_delta_v_vector.y) &&
        isfinite(powered_flyby_result.burn_delta_v_vector.z), 1);

    DepartureBurn3D departure_burn;
    Vector3 departure_vinf = vec3(3000, 4000, 1000);
    double departure_periapsis_radius = powered_flyby_body.radius + 200000;
    int departure_burn_valid = calculate_free_plane_departure_burn(departure_vinf,
        &powered_flyby_body, departure_periapsis_radius, &departure_burn);
    failures += expect_int("free-plane initial burn returns a finite 3D vector",
        departure_burn_valid && isfinite(departure_burn.burn_delta_v.x) &&
        isfinite(departure_burn.burn_delta_v.y) && isfinite(departure_burn.burn_delta_v.z), 1);
    failures += expect_near("free-plane initial vector matches scalar departure delta-v",
        mag_vec3(departure_burn.burn_delta_v),
        dv_circ(&powered_flyby_body, departure_periapsis_radius, mag_vec3(departure_vinf)), 1e-8);
    failures += expect_near("free-plane initial periapsis direction is normalized",
        mag_vec3(departure_burn.periapsis_direction), 1, 1e-12);
    Vector3 departure_orbit_normal = norm_vec3(cross_vec3(departure_burn.periapsis_direction,
        departure_burn.tangential_direction));
    failures += expect_near("free-plane initial burn has no radial component",
        dot_vec3(departure_burn.burn_delta_v, departure_burn.periapsis_direction), 0, 1e-8);
    failures += expect_near("free-plane initial burn has no normal component",
        dot_vec3(departure_burn.burn_delta_v, departure_orbit_normal), 0, 1e-8);

    Datetime iso_date = {2024, 2, 29, 12, 30, 15, DATE_ISO};
    Datetime kerbal_date = {3, 0, 42, 0, 0, 0, DATE_KERBAL};
    char iso_date_string[] = "2024-02-29";
    char kerbal_date_string[] = "3-042";

    failures += expect_datetime("date_from_string ISO", date_from_string(iso_date_string, DATE_ISO),
                                (Datetime) {2024, 2, 29, 0, 0, 0, DATE_ISO}, tolerance);
    failures += expect_datetime("date_from_string Kerbal", date_from_string(kerbal_date_string, DATE_KERBAL),
                                kerbal_date, tolerance);
    failures += expect_datetime("convert_JD_date ISO", convert_JD_date(convert_date_JD(iso_date), DATE_ISO),
                                iso_date, 1e-3);
    failures += strcicmp("KMAT", "kmat") != 0;
    failures += strcicmp("KMAT", "KMATx") == 0;
    failures += is_string_valid_date_format("2024-02-29", DATE_ISO) != 1;
    failures += is_string_valid_date_format("2024-02-30", DATE_ISO) != 1;
    failures += is_string_valid_date_format("3-042", DATE_KERBAL) != 1;
    failures += is_string_valid_date_format("2024/02/29", DATE_ISO) != 0;

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
    failures += expect_contains("header formats legacy unbounded arrival", header_text, "Max arrival date: Unbounded");
    failures += expect_contains("header formats legacy unbounded duration", header_text, "Max duration: Unbounded");
    failures += expect_contains("header includes itinerary body names", header_text, "  - Venus");

    char short_header[12];
    print_header_data_to_string(header, short_header, sizeof(short_header), DATE_ISO);
    failures += memchr(short_header, '\0', sizeof(short_header)) == NULL;

    ItineraryDateLimits arrival_date_limits = calculate_itinerary_date_limits(1, 100, 200, 160, 999, DATE_ISO);
    failures += expect_near("max-arrival mode preserves absolute arrival JD", arrival_date_limits.max_arrival_jd, 160, tolerance);
    failures += expect_near("max-arrival mode derives elapsed duration", arrival_date_limits.max_duration_days, 60, tolerance);

    ItineraryDateLimits kerbal_duration_limits = calculate_itinerary_date_limits(0, 100, 150, 0, 28, DATE_KERBAL);
    failures += expect_near("Kerbal duration converts to Earth days", kerbal_duration_limits.max_duration_days, 7, tolerance);
    failures += expect_near("Kerbal duration derives arrival JD", kerbal_duration_limits.max_arrival_jd, 157, tolerance);

    ItineraryDateLimits iso_duration_limits = calculate_itinerary_date_limits(0, 100, 150, 0, 28, DATE_ISO);
    failures += expect_near("ISO duration remains Earth days", iso_duration_limits.max_duration_days, 28, tolerance);
    failures += expect_near("ISO duration derives arrival JD", iso_duration_limits.max_arrival_jd, 178, tolerance);

    struct PorkchopGroup hidden_group = {.show_group = 0};
    struct PorkchopGroup visible_group = {.show_group = 1};
    struct PorkchopAnalyzerPoint porkchop_points[] = {
        {.inside_filter = 0, .group = &visible_group},
        {.inside_filter = 1, .group = &hidden_group},
        {.inside_filter = 1, .group = &visible_group}
    };
    failures += expect_int("visible porkchop selection skips filtered and hidden points",
                           find_first_visible_porkchop_point(porkchop_points, 3), 2);
    porkchop_points[2].inside_filter = 0;
    failures += expect_int("visible porkchop selection returns no-match sentinel",
                           find_first_visible_porkchop_point(porkchop_points, 3), -1);

    Body earth = {.name = "Earth"};
    Body mars = {.name = "Mars"};
    Body venus = {.name = "Venus"};
    Body jupiter = {.name = "Jupiter"};
    Body *system_bodies[] = {&earth, &mars, &venus, &jupiter};
    CelestSystem system = {.num_bodies = 4, .bodies = system_bodies};
    struct ItinStep departure = {.body = &earth};
    struct ItinStep flyby = {.body = &venus, .prev = &departure};
    struct ItinStep arrival = {.body = &mars, .prev = &flyby};
    int body_visibility[] = {0, 0, 0, 1};
    mark_itinerary_bodies_visible(body_visibility, 4, &arrival, &system);
    failures += expect_int("route marks departure body visible", body_visibility[0], 1);
    failures += expect_int("route marks flyby body visible", body_visibility[2], 1);
    failures += expect_int("route marks arrival body visible", body_visibility[1], 1);
    failures += expect_int("route leaves unrelated body hidden", body_visibility[3], 0);

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
    failures += expect_int("Hohmann reference accepts valid bodies",
                           calculate_itinerary_hohmann_reference(&earth_orbit, &mars_orbit, &sun, 50000, 50000, &reference), 1);
    failures += expect_int("Hohmann reference has finite positive duration", reference.duration_seconds > 0, 1);
    failures += expect_int("Hohmann circularization exceeds capture", reference.arrival_circularization_dv > reference.arrival_capture_dv, 1);
    failures += expect_int("Hohmann reference rejects identical bodies",
                           calculate_itinerary_hohmann_reference(&earth_orbit, &earth_orbit, &sun, 50000, 50000, &reference), 0);
    failures += expect_int("Hohmann reference rejects missing central body",
                           calculate_itinerary_hohmann_reference(&earth_orbit, &mars_orbit, NULL, 50000, 50000, &reference), 0);
    failures += expect_int("Hohmann reference rejects non-positive central gravity",
                           calculate_itinerary_hohmann_reference(&earth_orbit, &mars_orbit, &(Body){0}, 50000, 50000, &reference), 0);

    ItineraryHohmannReference sample_reference = {
        .duration_seconds = 10 * 86400.0,
        .departure_dv = 1200,
        .arrival_capture_dv = 400,
        .arrival_circularization_dv = 600
    };
    ItineraryHohmannDefaults defaults;
    failures += expect_int("flyby Hohmann defaults calculate",
                           calculate_itinerary_hohmann_defaults(&sample_reference, TF_FLYBY, DATE_ISO, 2450000, &defaults), 1);
    failures += expect_near("Hohmann default duration is twice baseline", defaults.max_duration_user_days, 20, tolerance);
    failures += expect_near("Hohmann default arrival follows earliest departure", defaults.max_arrival_jd, 2450020, tolerance);
    failures += expect_near("Hohmann default departure cap includes 10 percent margin", defaults.max_departure_dv, 1320, tolerance);
    failures += expect_near("flyby mode has no arrival insertion budget", defaults.max_satellite_dv, 0, tolerance);
    failures += expect_near("flyby total cap includes 10 percent margin", defaults.max_total_dv, 1320, tolerance);

    calculate_itinerary_hohmann_defaults(&sample_reference, TF_CAPTURE, DATE_ISO, 2450000, &defaults);
    failures += expect_near("capture mode satellite cap uses capture baseline", defaults.max_satellite_dv, 400, tolerance);
    failures += expect_near("capture total cap includes 10 percent margin", defaults.max_total_dv, 1760, tolerance);
    calculate_itinerary_hohmann_defaults(&sample_reference, TF_CIRC, DATE_ISO, 2450000, &defaults);
    failures += expect_near("circularization mode satellite cap uses circular baseline", defaults.max_satellite_dv, 600, tolerance);
    failures += expect_near("circularization total cap includes 10 percent margin", defaults.max_total_dv, 1980, tolerance);

    calculate_itinerary_hohmann_defaults(&sample_reference, TF_FLYBY, DATE_KERBAL, 2450000, &defaults);
    failures += expect_near("Kerbal Hohmann window converts to local days", defaults.max_duration_user_days, 80, tolerance);
    failures += expect_near("Kerbal Hohmann arrival preserves physical duration", defaults.max_arrival_jd, 2450020, tolerance);

    if (failures == 0) {
        printf("KMAT unit checks: %d total, %d passed, 0 failed.\n", total_checks, total_checks);
        return 0;
    }

    printf("KMAT unit checks: %d total, %d passed, %d failed.\n",
           total_checks, total_checks - failures, failures);
    return 1;
}
