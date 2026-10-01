#include "geometrylib.h"
#include "geometrylib_vec.h"
#include "orbitlib_datetime.h"
#include "tools/tool_funcs.h"

#include <math.h>
#include <stdio.h>

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

int main(void) {
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    const int total_checks = 22;
    int failures = 0;

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

    if (failures == 0) {
        printf("KMAT unit checks: %d total, %d passed, 0 failed.\n", total_checks, total_checks);
        return 0;
    }

    printf("KMAT unit checks: %d total, %d passed, %d failed.\n",
           total_checks, total_checks - failures, failures);
    return 1;
}
