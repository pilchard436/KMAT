#include "unit_tests.h"
#include "tools/celestial_systems.h"
#include "ksp/ksp_nodes.h"
#include <string.h>

static Body test_body(const char *name, double mu, double radius) {
	Body body = {0};
	snprintf(body.name, sizeof(body.name), "%s", name);
	body.mu = mu;
	body.radius = radius;
	body.rotation_period = 10;
	return body;
}

void run_ksp_tests(void) {
	Body root_a = test_body("Sun", 1.3e20, 6.9e8);
	Body kerbin_a = test_body("Kerbin", 3.5e12, 6e5);
	Body mun_a = test_body("Mun", 6.5e10, 2e5);
	Body duna_a = test_body("Duna", 3e11, 3.2e5);
	Body root_b = test_body("Sun", 1.3e20, 6.9e8);
	Body kerbin_b = test_body("Kerbin", 3.5e12, 6e5);
	Body mun_b = test_body("Mun", 6.5e10, 2e5);
	Body duna_b = test_body("Duna", 3e11, 3.2e5);
	kerbin_a.orbit = (Orbit){.cb = &root_a, .a = 1e7, .e = 0.01, .i = 0.1, .raan = 0.2, .arg_peri = 0.3, .ta = 0.4};
	mun_a.orbit = (Orbit){.cb = &kerbin_a, .a = 1e6, .e = 0.02, .i = 0.03, .raan = 0.04, .arg_peri = 0.05, .ta = 0.06};
	duna_a.orbit = (Orbit){.cb = &root_a, .a = 2e7, .e = 0.03, .i = 0.04, .raan = 0.05, .arg_peri = 0.06, .ta = 0.07};
	kerbin_b.orbit = (Orbit){.cb = &root_b, .a = 1e7, .e = 0.01, .i = 0.1, .raan = 0.2, .arg_peri = 0.3, .ta = 0.4};
	mun_b.orbit = (Orbit){.cb = &kerbin_b, .a = 1e6, .e = 0.02, .i = 0.03, .raan = 0.04, .arg_peri = 0.05, .ta = 0.06};
	duna_b.orbit = (Orbit){.cb = &root_b, .a = 2e7, .e = 0.03, .i = 0.04, .raan = 0.05, .arg_peri = 0.06, .ta = 0.07};
	Body *kerbin_moons_a[] = {&mun_a};
	Body *kerbin_moons_b[] = {&mun_b};
	CelestSystem kerbin_system_a = {.num_bodies = 1, .cb = &kerbin_a, .bodies = kerbin_moons_a};
	CelestSystem kerbin_system_b = {.num_bodies = 1, .cb = &kerbin_b, .bodies = kerbin_moons_b};
	kerbin_a.system = &kerbin_system_a;
	kerbin_b.system = &kerbin_system_b;
	Body *root_bodies_a[] = {&kerbin_a, &duna_a};
	Body *root_bodies_b[] = {&duna_b, &kerbin_b};
	CelestSystem system_a = {.name = "KSP config", .num_bodies = 2, .cb = &root_a, .bodies = root_bodies_a};
	CelestSystem system_b = {.name = "Different display name", .num_bodies = 2, .cb = &root_b, .bodies = root_bodies_b};
	root_a.system = &system_a;
	root_b.system = &system_b;

	expect_int("KSP system match ignores display name and body order", celestial_systems_equivalent(&system_a, &system_b), 1);
	mun_b.orbit.a += 1;
	expect_int("KSP system match detects nested moon orbit changes", celestial_systems_equivalent(&system_a, &system_b), 0);
	mun_b.orbit.a -= 1;
	mun_b.mu += 1e5;
	expect_int("KSP system match detects nested body gravity changes", celestial_systems_equivalent(&system_a, &system_b), 0);
	mun_b.mu -= 1e5;
	double original_axis = duna_b.orbit.a;
	duna_b.orbit.a += 1e4;
	expect_int("KSP system match detects planet orbit changes", celestial_systems_equivalent(&system_a, &system_b), 0);
	duna_b.orbit.a = original_axis;
	expect_int("KSP system equivalence rejects null systems", celestial_systems_equivalent(NULL, &system_b), 0);

	Vector3 components;
	expect_int("KSP RTN node projection accepts a nondegenerate orbit",
		ksp_project_delta_v_to_node(vec3(2, 0, 0), vec3(0, 3, 0), vec3(5, 7, 11), &components), 1);
	expect_near("KSP node projection orders prograde first", components.x, 7, 1e-12);
	expect_near("KSP node projection orders normal second", components.y, 11, 1e-12);
	expect_near("KSP node projection orders radial third", components.z, 5, 1e-12);
	expect_int("KSP node projection accepts an eccentric orbit state",
		ksp_project_delta_v_to_node(vec3(2, 0, 0), vec3(1, 3, 0), vec3(1, 3, 0), &components), 1);
	expect_near("KSP node prograde follows the velocity, not the horizontal", components.x, sqrt(10), 1e-12);
	expect_near("KSP node radial is zero for a burn along the velocity", components.z, 0, 1e-12);
	expect_int("KSP RTN node projection rejects a zero position",
		ksp_project_delta_v_to_node(vec3(0, 0, 0), vec3(0, 1, 0), vec3(1, 2, 3), &components), 0);
	expect_int("KSP RTN node projection rejects a radial-only velocity",
		ksp_project_delta_v_to_node(vec3(1, 0, 0), vec3(2, 0, 0), vec3(1, 2, 3), &components), 0);
}
