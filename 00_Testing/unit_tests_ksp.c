#include "unit_tests.h"
#include "tools/celestial_systems.h"
#include "ksp/ksp_nodes.h"
#include "ksp/ksp_node_internal.h"
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

	Body planet = test_body("Test", 3.9860043543609598e14, 6371000);
	struct ItinStep departure = {0};
	struct ItinStep arrival = {
		.body = &planet,
		.v_arr = vec3(0, 6000, 0),
		.v_body = vec3(0, 0, 0),
		.prev = &departure
	};
	double arrival_altitude = 100000, periapsis_radius = planet.radius + arrival_altitude;
	Vector3 final_offset = planned_aim_offset(&arrival, arrival_altitude);
	double expected_impact_parameter = periapsis_radius *
		sqrt(1 + 2 * planet.mu / (periapsis_radius * 6000 * 6000));
	expect_near("KSP final-arrival aim targets the requested periapsis impact parameter",
		mag_vec3(final_offset), expected_impact_parameter, 1e-8);
	expect_near("KSP final-arrival aim preserves the expected side of the body", final_offset.x,
		-expected_impact_parameter, 1e-8);
	expect_near("KSP final-arrival aim is perpendicular to the incoming asymptote", final_offset.y, 0, 1e-8);
	expect_near("KSP final-arrival aim stays in the transfer plane", final_offset.z, 0, 1e-8);

	Body central = test_body("Central", 1.3271244004193938e20, 6.9e8);
	planet.orbit = (Orbit){.a = 1.5e11, .cb = &central};
	struct ItinStep flyby = {.body = &planet, .v_arr = vec3(6000, 0, 0)};
	struct ItinStep outbound = {.v_dep = vec3(3000, 5196.152422706632, 0)};
	struct ItinStep *outbound_next[] = {&outbound};
	arrival.v_arr = flyby.v_arr;
	arrival.next = outbound_next;
	flyby.next = outbound_next;
	PoweredFlybyResult planned_flyby;
	int flyby_valid = calculate_powered_flyby(flyby.v_arr, outbound.v_dep, flyby.v_body, &planet,
		planet.radius + 10000, &planned_flyby);
	expect_int("KSP intermediate-flyby aim fixture is feasible", flyby_valid, 1);
	if(flyby_valid) {
		Vector3 flyby_offset = planned_aim_offset(&arrival, arrival_altitude);
		Vector3 incoming = norm_vec3(flyby.v_arr);
		Vector3 intended_side = subtract_vec3(planned_flyby.periapsis_direction,
			scale_vec3(incoming, dot_vec3(planned_flyby.periapsis_direction, incoming)));
		double flyby_impact_parameter = planned_flyby.periapsis_radius *
			sqrt(1 + 2 * planet.mu / (planned_flyby.periapsis_radius * 6000 * 6000));
		expect_near("KSP intermediate-flyby aim uses the planned impact parameter",
			mag_vec3(flyby_offset), flyby_impact_parameter, 1e-8);
		expect_near("KSP intermediate-flyby aim uses the planned periapsis side",
			dot_vec3(flyby_offset, norm_vec3(intended_side)), flyby_impact_parameter, 1e-8);
	}

	const double mu = 3.9860043543609598e14;
	Vector3 burn_state = vec3(7e6, 0, 0), old_velocity = vec3(0, 7500, 0), desired_excess = vec3(0, 3000, 0);
	Vector3 candidate_burns[4];
	int burn_count = burn_to_asymptote(mu, burn_state, old_velocity, desired_excess, 6.9e6, candidate_burns, 4);
	expect_int("KSP asymptote solver finds a burn for a reachable outbound direction", burn_count > 0, 1);
	double previous_burn_size = -1;
	for(int i = 0; i < burn_count; i++) {
		Vector3 outgoing_direction;
		double outgoing_periapsis;
		double burn_size = mag_vec3(candidate_burns[i]);
		expect_int("KSP asymptote solver returns finite burns",
			isfinite(candidate_burns[i].x) && isfinite(candidate_burns[i].y) && isfinite(candidate_burns[i].z), 1);
		expect_int("KSP asymptote solver returns burns in increasing cost order", burn_size >= previous_burn_size, 1);
		expect_int("KSP asymptote solver produces a valid outgoing hyperbola",
			outgoing_asymptote(mu, burn_state, add_vec3(old_velocity, candidate_burns[i]),
				&outgoing_direction, &outgoing_periapsis), 1);
		if(isfinite(outgoing_direction.x + outgoing_direction.y + outgoing_direction.z)) {
			double angular_error = acos(fmax(-1, fmin(1, dot_vec3(outgoing_direction, norm_vec3(desired_excess)))));
			expect_near("KSP asymptote solver meets its direction tolerance", angular_error, 0, 1e-6);
			int already_past_periapsis = dot_vec3(burn_state,
				add_vec3(old_velocity, candidate_burns[i])) >= 0;
			expect_int("KSP asymptote solver respects the periapsis floor before periapsis",
				already_past_periapsis || outgoing_periapsis >= 6.9e6 * 0.999, 1);
		}
		previous_burn_size = burn_size;
	}
	expect_int("KSP asymptote solver rejects collinear radius and excess velocity",
		burn_to_asymptote(mu, burn_state, old_velocity, vec3(3000, 0, 0), 6.9e6, candidate_burns, 4), 0);
}
