#include "unit_tests.h"

void run_math_tests(void) {
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    (void)pi;
    (void)tolerance;
    expect_near("deg2rad", deg2rad(180), pi, tolerance);
    expect_near("rad2deg", rad2deg(pi), 180, tolerance);
    expect_near("pi_norm negative", pi_norm(-pi / 2), 3 * pi / 2, tolerance);
    expect_near("pi_norm full rotation", pi_norm(2 * pi), 0, tolerance);
    expect_near("pi_norm overflow", pi_norm(5 * pi / 2), pi / 2, tolerance);

    Vector3 first = vec3(1, 2, 3);
    Vector3 second = vec3(4, -2, 1);

    expect_vector3("add_vec3", add_vec3(first, second), vec3(5, 0, 4), tolerance);
    expect_vector3("subtract_vec3", subtract_vec3(first, second), vec3(-3, 4, 2), tolerance);
    expect_vector3("scale_vec3", scale_vec3(first, 2), vec3(2, 4, 6), tolerance);
    expect_near("sq_mag_vec3", sq_mag_vec3(first), 14, tolerance);
    expect_near("mag_vec3", mag_vec3(first), sqrt(14), tolerance);
    expect_near("dot_vec3", dot_vec3(first, second), 3, tolerance);
    expect_vector3("cross_vec3", cross_vec3(first, second), vec3(8, 11, -10), tolerance);
    expect_near("angle_vec3_vec3", angle_vec3_vec3(vec3(1, 0, 0), vec3(0, 1, 0)), pi / 2, tolerance);

    DataArray2 *first_root_samples = data_array2_create();
    DataArray2 *second_root_samples = data_array2_create();
    data_array2_insert_new(first_root_samples, vec2(1, 10));
    data_array2_insert_new(second_root_samples, vec2(2, 20));
    expect_int("simultaneous root sample arrays remain independent",
                           data_array2_size(first_root_samples) == 1 &&
                           data_array2_size(second_root_samples) == 1 &&
                           data_array2_get_data(first_root_samples)[0].x == 1 &&
                           data_array2_get_data(second_root_samples)[0].x == 2, 1);
    data_array2_free(first_root_samples);
    data_array2_free(second_root_samples);
	DataArray2 *reused_root_samples = data_array2_create();
    expect_int("thread-local root sample storage is reusable",
						   data_array2_size(reused_root_samples) == 0, 1);
    data_array2_free(reused_root_samples);

}
