#include "test_support.h"
#include "orbit_calculator/flyby_policy.h"
#include "orbit_calculator/itin_tool.h"
#include "orbit_calculator/moon_flyby.h"
#include "orbitlib_transfer.h"

#include <math.h>

#define MOON_ORBIT_RADIUS 3.8e8
#define TEST_JD 2451545.0

typedef struct {
	Body sun, planet, moon;
	CelestSystem moons;
	Body *moon_list[1];
	PoweredFlybyResult flyby;
	Vector3 incoming, outgoing;
} MoonFixture;

static void place_moon(MoonFixture *fixture, double angle_offset) {
	Vector3 periapsis = scale_vec3(fixture->flyby.periapsis_direction, fixture->flyby.periapsis_radius);
	fixture->moon.orbit = constr_orbit_from_elements(MOON_ORBIT_RADIUS, 0, 0, 0, 0, 0, &fixture->planet);
	OSV state = osv_from_elements(fixture->moon.orbit, TEST_JD);
	double true_anomaly = atan2(periapsis.y, periapsis.x) + angle_offset - atan2(state.r.y, state.r.x);
	fixture->moon.orbit = constr_orbit_from_elements(MOON_ORBIT_RADIUS, 0, 0, 0, 0, true_anomaly, &fixture->planet);
}

static void init_fixture(MoonFixture *fixture) {
	*fixture = (MoonFixture){0};
	fixture->sun.mu = 1.3271244004193938e20;
	fixture->planet = (Body){.mu = 3.9860043543609598e14, .radius = 6371000,
		.orbit = {.a = 1.5e11, .cb = &fixture->sun}};
	fixture->moon = (Body){.mu = 4.9028e12, .radius = 1737100};
	fixture->moon_list[0] = &fixture->moon;
	fixture->moons = (CelestSystem){.num_bodies = 1, .prop_method = ORB_ELEMENTS, .bodies = fixture->moon_list,
		.cb = &fixture->planet};
	fixture->planet.system = &fixture->moons;
	// Equal excess speeds turn the flyby with periapsis near the moon's orbit radius.
	double speed = 1500;
	double sine_half_turn = 1 / (1 + MOON_ORBIT_RADIUS * speed * speed / fixture->planet.mu);
	double turn = 2 * asin(sine_half_turn);
	fixture->incoming = vec3(speed, 0, 0);
	fixture->outgoing = vec3(speed * cos(turn), speed * sin(turn), 0);
	calculate_powered_flyby(fixture->incoming, fixture->outgoing, vec3(0, 0, 0), &fixture->planet,
		fixture->planet.radius + 1e4, &fixture->flyby);
	place_moon(fixture, 0);
}

void check_moon_flybys(void) {
	MoonFixture fixture;
	init_fixture(&fixture);
	MoonFlybyEffect effect;
	check_true("the fixture flyby periapsis is near the moon orbit",
		fabs(fixture.flyby.periapsis_radius - MOON_ORBIT_RADIUS) < 0.05 * MOON_ORBIT_RADIUS);

	OSV state;
	check_true("a moon state is available", moon_state_at(&fixture.planet, &fixture.moon, TEST_JD, &state));
	check_near("the moon is on its orbit radius", mag_vec3(state.r), MOON_ORBIT_RADIUS, 1);

	check_true("a planet with a moon is analysed", analyze_moon_flyby(&fixture.planet, &fixture.flyby, TEST_JD, 1, &effect));
	check_true("a flyby through the moon hits it", effect.hits_moon && effect.moon_hit == &fixture.moon);
	check_true("the hit is not integrated", !effect.integrated);

	place_moon(&fixture, M_PI);
	analyze_moon_flyby(&fixture.planet, &fixture.flyby, TEST_JD, 1, &effect);
	check_true("a moon on the other side is not hit and not integrated", !effect.hits_moon && !effect.integrated);
	check_true("a moon on the other side does not perturb", effect.perturbing_moon == NULL);

	place_moon(&fixture, 0.1);
	analyze_moon_flyby(&fixture.planet, &fixture.flyby, TEST_JD, 1, &effect);
	check_true("a close pass is not a hit", !effect.hits_moon);
	check_true("a close pass is inside the moon's sphere of influence", effect.perturbing_moon == &fixture.moon);
	check_true("a close pass is closer than the moon's influence", effect.closest_moon_distance < 1.26e8);
	check_true("a close pass integrates the moon gravity", effect.integrated);
	double error = mag_vec3(effect.velocity_error);
	check_true("the moon changes the outgoing velocity", isfinite(error) && error > 1 && error < 1e4);
	place_moon(&fixture, 0.2);
	MoonFlybyEffect farther;
	analyze_moon_flyby(&fixture.planet, &fixture.flyby, TEST_JD, 1, &farther);
	check_true("a farther pass changes the velocity less", farther.integrated && mag_vec3(farther.velocity_error) < error);

	fixture.planet.system = NULL;
	check_true("a planet without moons is not analysed", !analyze_moon_flyby(&fixture.planet, &fixture.flyby, TEST_JD, 1, &effect));
	fixture.planet.system = &fixture.moons;

	double minimum = fixture.planet.radius + 1e4;
	PoweredFlybyResult plain, with_moons;
	calculate_powered_flyby(fixture.incoming, fixture.outgoing, vec3(0, 0, 0), &fixture.planet, minimum, &plain);
	place_moon(&fixture, M_PI);
	check_true("a free flyby stays viable", calculate_flyby_with_moons(fixture.incoming, fixture.outgoing, vec3(0, 0, 0),
		&fixture.planet, minimum, TEST_JD, &with_moons));
	check_near("a free flyby keeps its burn", with_moons.burn_delta_v, plain.burn_delta_v, 1e-9);
	place_moon(&fixture, 0.1);
	check_true("the search re-plans a flyby that passes close to a moon", calculate_flyby_with_moons(fixture.incoming, fixture.outgoing,
		vec3(0, 0, 0), &fixture.planet, minimum, TEST_JD, &with_moons) && with_moons.burn_delta_v >= plain.burn_delta_v);
	analyze_moon_flyby(&fixture.planet, &with_moons, TEST_JD, 0, &effect);
	check_true("the re-planned flyby clears the moon", !effect.hits_moon && effect.perturbing_moon == NULL);
	int relaxed = set_flyby_unrestricted_periapsis(1);
	check_true("a close pass adds the correction to the burn in the planner", calculate_flyby_with_moons(fixture.incoming, fixture.outgoing,
		vec3(0, 0, 0), &fixture.planet, minimum, TEST_JD, &with_moons) && with_moons.burn_delta_v > plain.burn_delta_v);
	set_flyby_unrestricted_periapsis(relaxed);
	place_moon(&fixture, 0);
	double planned_radius;
	int planned_side;
	check_true("a flyby that hits a moon is re-planned around it", calculate_flyby_with_moons(fixture.incoming, fixture.outgoing,
		vec3(0, 0, 0), &fixture.planet, minimum, TEST_JD, &with_moons) && with_moons.burn_delta_v >= plain.burn_delta_v &&
		fabs(with_moons.periapsis_radius - plain.periapsis_radius) > 1e6);
	analyze_moon_flyby(&fixture.planet, &with_moons, TEST_JD, 0, &effect);
	check_true("the re-planned flyby does not hit the moon", !effect.hits_moon && effect.perturbing_moon == NULL);
	check_true("the planned periapsis and side are reported", plan_flyby_around_moons(fixture.incoming, fixture.outgoing,
		vec3(0, 0, 0), &fixture.planet, minimum, TEST_JD, NULL, &planned_radius, &planned_side) &&
		planned_radius > 0 && (planned_side == 1 || planned_side == -1));

	struct ItinStep departure = {.body = &fixture.planet, .date = TEST_JD - 2,
		.v_body = {0}, .v_dep = {1500, 0, 0}};
	struct ItinStep flyby = {.body = &fixture.planet, .date = TEST_JD,
		.v_body = {0}, .v_arr = fixture.incoming, .v_dep = fixture.incoming, .prev = &departure};
	struct ItinStep arrival = {.body = &fixture.planet, .date = TEST_JD + 2,
		.v_body = {0}, .v_arr = {100, 0, 0}, .v_dep = fixture.outgoing, .prev = &flyby};
	struct ItinStep *departure_next[] = {&flyby}, *flyby_next[] = {&arrival};
	departure.next = departure_next;
	departure.num_next_nodes = 1;
	flyby.next = flyby_next;
	flyby.num_next_nodes = 1;
	double periapsis_altitude = 1e4;
	int previous_exact = set_porkchop_exact_flybys(0);
	struct PorkchopPoint approximate = create_porkchop_point(&arrival, periapsis_altitude, periapsis_altitude);
	double component_burn = itinerary_step_dv(&flyby, periapsis_altitude, periapsis_altitude, TF_FLYBY);
	check_true("the moon-avoidance fixture distinguishes summary and component flyby burns",
		component_burn > approximate.dv_flyby + 1);
	char overview[8192];
	itinerary_detailed_overview_to_string(&departure, DATE_ISO, periapsis_altitude, periapsis_altitude, overview);
	const char *flyby_total = strstr(overview, "Powered flyby burns: ");
	double displayed_burn = HUGE_VAL;
	if(flyby_total != NULL) sscanf(flyby_total, "Powered flyby burns: %lf", &displayed_burn);
	check_near("detailed overview total matches moon-aware flyby components", displayed_burn, component_burn, 0.01);
	check_true("detailed overview restores the search flyby mode", set_porkchop_exact_flybys(previous_exact) == 0);
	int previous = set_flyby_unrestricted_periapsis(1);
	check_true("the planner keeps it to show the problem", calculate_flyby_with_moons(fixture.incoming, fixture.outgoing,
		vec3(0, 0, 0), &fixture.planet, minimum, TEST_JD, &with_moons));
	set_flyby_unrestricted_periapsis(previous);

	place_moon(&fixture, 0.1);
	check_true("a pass inside a moon's sphere of influence blocks the flyby", flyby_blocked_by_moon(&fixture.planet, &fixture.flyby, TEST_JD));
	place_moon(&fixture, M_PI);
	check_true("a distant moon does not block the flyby", !flyby_blocked_by_moon(&fixture.planet, &fixture.flyby, TEST_JD));
	place_moon(&fixture, 0.1);
	int relaxed_blocking = set_flyby_unrestricted_periapsis(1);
	check_true("the planner is not blocked", !flyby_blocked_by_moon(&fixture.planet, &fixture.flyby, TEST_JD));
	set_flyby_unrestricted_periapsis(relaxed_blocking);

	double capture_radius = fixture.planet.radius + 2e5;
	double plain_capture = dv_capture(&fixture.planet, capture_radius, 3000);
	MoonAssist assist;
	int capture_assists = 0, phases = 24;
	for(int phase = 0; phase < phases; phase++) {
		place_moon(&fixture, 2 * M_PI * phase / phases);
		capture_assists += moon_assist_arrival(&fixture.planet, vec3(3000, 0, 0), capture_radius, dv_capture,
			plain_capture, TEST_JD, &assist) && assist.total_dv < plain_capture && assist.moon == &fixture.moon;
	}
	check_true("a moon lowers the capture burn at some moon phases", capture_assists > 0 && capture_assists < phases);

	place_moon(&fixture, 0);
	fixture.moon.mu = 1e3;
	check_true("a tiny moon cannot help", !moon_assist_arrival(&fixture.planet, vec3(3000, 0, 0), capture_radius,
		dv_capture, plain_capture, TEST_JD, &assist));
	fixture.moon.mu = 4.9028e12;
	fixture.planet.system = NULL;
	check_true("no moons means no assist", !moon_assist_arrival(&fixture.planet, vec3(3000, 0, 0), capture_radius,
		dv_capture, plain_capture, TEST_JD, &assist));
}
