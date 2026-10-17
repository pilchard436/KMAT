#include "test_support.h"
#include "orbit_calculator/itin_tool.h"
#include "orbit_calculator/search_rejections.h"
#include "tools/file_io.h"
#include "orbitlib_fileio.h"
#include "orbitlib_transfer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cancel_requested(void *context) {
	return *(int *)context;
}

static struct ItinStep *make_step(Body *body, double date, struct ItinStep *parent) {
	struct ItinStep *step = calloc(1, sizeof(*step));
	step->body = body;
	step->date = date;
	step->prev = parent;
	if(parent != NULL) {
		parent->next = realloc(parent->next, (parent->num_next_nodes + 1) * sizeof(*parent->next));
		parent->next[parent->num_next_nodes++] = step;
	}
	return step;
}

static void check_itinerary_tree(void) {
	Body earth = {.name = "A"}, venus = {.name = "B"}, mars = {.name = "C"};
	struct ItinStep *first = make_step(&earth, 100, NULL);
	struct ItinStep *middle = make_step(&venus, 140, first);
	struct ItinStep *last = make_step(&mars, 330, middle);

	check_true("get_first walks back to the departure", get_first(last) == first && get_first(first) == first);
	check_true("get_first and get_last accept NULL", get_first(NULL) == NULL && get_last(NULL) == NULL);
	check_true("get_last follows the first branch to the arrival", get_last(first) == last);
	check_true("layers count every step of the first branch", get_num_of_itin_layers(first) == 3);
	check_near("itinerary duration is arrival minus departure date", get_itinerary_duration(last), 230, 1e-12);
	check_true("a single chain is one itinerary", get_number_of_itineraries(first) == 1);
	check_true("a root without next steps is no itinerary", get_number_of_itineraries(make_step(&earth, 1, NULL)) == 0);
	check_true("stored steps count the whole tree", get_total_number_of_stored_steps(first) == 3);

	struct ItinStep *second_arrival = make_step(&mars, 350, middle);
	check_true("a branch adds an itinerary", get_number_of_itineraries(first) == 2);
	check_true("stored steps include the branch", get_total_number_of_stored_steps(first) == 4);
	struct ItinStep *arrivals[2];
	int index = 0;
	store_itineraries_in_array(first, arrivals, &index);
	check_true("arrival nodes are stored in tree order", index == 2 && arrivals[0] == last && arrivals[1] == second_arrival);

	check_true("a flyby itinerary is valid", is_valid_itinerary(last));
	check_true("a departure node alone is not a valid itinerary", !is_valid_itinerary(first));
	middle->body = NULL;
	check_true("a maneuver node without a position marker is invalid", !is_valid_itinerary(last));
	middle->v_body = vec3(1, 0, 0);
	check_true("a marked maneuver node is valid", is_valid_itinerary(last));
	middle->body = &venus;

	struct ItinStep *copy = create_itin_copy(first);
	check_true("tree copy is independent", copy != first && copy->next[0] != middle && copy->prev == NULL);
	check_true("tree copy keeps structure and dates", get_number_of_itineraries(copy) == 2 &&
		copy->next[0]->date == 140 && copy->next[0]->next[0]->date == 330 && copy->next[0]->next[0]->prev == copy->next[0]);
	copy->next[0]->date = 999;
	check_near("changing the copy leaves the original", middle->date, 140, 0);
	free_itinerary(copy);

	struct ItinStep *chain = create_itin_copy_from_arrival(last);
	check_true("arrival copy rebuilds the single route", get_num_of_itin_layers(get_first(chain)) == 3 &&
		get_first(chain)->date == 100 && chain->date == 330 && chain->body == &mars);
	check_true("arrival copy is detached from the original", get_first(chain) != first && chain->num_next_nodes == 0);
	free_itinerary(get_first(chain));

	struct ItinStep target = {0};
	middle->r = vec3(1, 2, 3);
	middle->v_arr = vec3(4, 5, 6);
	middle->v_dep = vec3(7, 8, 9);
	middle->v_body = vec3(10, 11, 12);
	copy_step_body_vectors_and_date(middle, &target);
	check_true("step copy transfers body, vectors and date", target.body == &venus && target.date == 140 &&
		target.r.z == 3 && target.v_arr.x == 4 && target.v_dep.y == 8 && target.v_body.z == 12);

	set_itinerary_step_date(middle, NULL, 400);
	check_near("a changed date is stored", middle->date, 400, 0);
	check_near("later steps are pushed behind the changed date", last->date, 401, 0);
	check_near("earlier steps stay", first->date, 100, 0);

	remove_step_from_itinerary(second_arrival);
	check_true("removing a branch leaf keeps its sibling", middle->num_next_nodes == 1 && middle->next[0] == last);
	free_itinerary(first);
}

static void check_flyby_policy(void) {
	/* The powered-flyby model needs the sphere of influence, so the body orbits a central body. */
	Body sun = {.mu = 1.32712440018e20};
	Body venus = {.name = "V", .mu = 3.24859e14, .radius = 6.0518e6, .atmo_alt = 0};
	venus.orbit.a = 1.082e11;
	venus.orbit.cb = &sun;
	double safe = venus.radius + venus.atmo_alt + 1e4;
	check_near("minimum periapsis keeps the surface clearance", flyby_minimum_periapsis(&venus), safe, 1e-6);
	int previous = set_flyby_unrestricted_periapsis(1);
	check_true("unrestricted mode starts disabled", previous == 0);
	check_near("unrestricted periapsis is a tenth of the radius", flyby_minimum_periapsis(&venus), venus.radius * 0.1, 1e-6);
	check_true("setting the mode returns the previous one", set_flyby_unrestricted_periapsis(0) == 1);
	check_near("restricted periapsis is restored", flyby_minimum_periapsis(&venus), safe, 1e-6);

	Vector3 body_velocity = vec3(35000, 0, 0);
	struct ItinStep *departure = make_step(&venus, 0, NULL);
	struct ItinStep *flyby = make_step(&venus, 100, departure);
	struct ItinStep *arrival = make_step(&venus, 200, flyby);
	flyby->v_body = body_velocity;
	flyby->v_arr = add_vec3(body_velocity, vec3(3000, 0, 0));
	/* A 135 degree turn at the incoming speed needs a periapsis inside the planet; leaving slower makes it possible. */
	arrival->v_dep = add_vec3(body_velocity, vec3(1000 * cos(3 * M_PI / 4), 1000 * sin(3 * M_PI / 4), 0));

	double expected_shortfall = venus.radius + venus.atmo_alt - get_flyby_periapsis(flyby->v_arr, arrival->v_dep, body_velocity, &venus);
	check_true("the gravity-assist periapsis lies inside the planet", expected_shortfall > 0);
	check_near("shortfall is the depth below the surface", flyby_periapsis_shortfall(flyby), expected_shortfall, 1e-3);
	check_near("a departure step has no shortfall", flyby_periapsis_shortfall(departure), 0, 0);
	check_near("a missing step has no shortfall", flyby_periapsis_shortfall(NULL), 0, 0);
	check_true("the powered flyby can be flown at the minimum periapsis", flyby_is_viable(flyby->v_arr, arrival->v_dep, body_velocity, &venus));
	check_true("the powered flyby stays above the safe radius", !flyby_below_min_safe(flyby));
	check_true("no flyby of the route is below the safe radius", count_flybys_below_min_safe(arrival) == 0);
	double largest = 0;
	check_true("one flyby needs a burn", count_flybys_needing_burn(arrival, &largest) == 1);
	check_near("the largest shortfall is reported", largest, expected_shortfall, 1e-3);
	check_true("the largest shortfall is optional", count_flybys_needing_burn(arrival, NULL) == 1);

	PoweredFlybyResult unrestricted;
	int flown = calculate_powered_flyby(flyby->v_arr, arrival->v_dep, body_velocity, &venus, venus.radius * 0.1, &unrestricted);
	int dips = flown && unrestricted.periapsis_radius < safe - 1;
	set_flyby_unrestricted_periapsis(1);
	check_true("an unrestricted flyby is judged by the periapsis it flies", flyby_below_min_safe(flyby) == dips);
	check_true("the dip is counted", count_flybys_below_min_safe(arrival) == dips);
	set_flyby_unrestricted_periapsis(0);
	check_true("restricted mode is cached separately", !flyby_below_min_safe(flyby));

	arrival->v_dep = add_vec3(body_velocity, vec3(3000 * cos(1.7e-4), 3000 * sin(1.7e-4), 0));
	check_near("a nearly straight flyby has no shortfall", flyby_periapsis_shortfall(flyby), 0, 0);
	check_true("a nearly straight flyby needs no burn", count_flybys_needing_burn(arrival, NULL) == 0);
	free_itinerary(departure);
}

static void check_leg_variants(void) {
	Body sun = {.mu = 1.32712440018e20};
	Vector3 r0 = vec3(1.496e11, 0, 0);
	double angle = 1.7;
	Vector3 r1 = vec3(1.08e11 * cos(angle), 1.08e11 * sin(angle), 0);
	double dt = 150 * 86400.0;

	Lambert3 plain = calc_lambert3(r0, r1, dt, &sun);
	Lambert3 variant_zero = calc_leg_variant_lambert(&sun, r0, r1, dt, 0, NULL, NULL);
	check_true("variant 0 is the direct Lambert arc", variant_zero.success == plain.success &&
		mag_vec3(subtract_vec3(variant_zero.v0, plain.v0)) == 0 && mag_vec3(subtract_vec3(variant_zero.v1, plain.v1)) == 0);
	check_true("a negative variant also gives the direct arc", mag_vec3(subtract_vec3(
		calc_leg_variant_lambert(&sun, r0, r1, dt, -3, NULL, NULL).v0, plain.v0)) == 0);

	int cancel = 1;
	check_true("a cancelled variant solve is not usable", !is_usable_lambert(
		calc_leg_variant_lambert(&sun, r0, r1, dt, 0, cancel_requested, &cancel)));

	check_true("a converged arc is usable", is_usable_lambert(plain));
	Lambert3 broken = plain;
	broken.v0.x = NAN;
	check_true("non-finite velocities are not usable", !is_usable_lambert(broken));
	broken = plain;
	broken.success = LAMBERT_MAX_ITERATIONS;
	check_true("an unconverged arc is not usable", !is_usable_lambert(broken));

	Lambert3 nearest;
	int variant = -1;
	check_true("the nearest variant is found", nearest_leg_variant_lambert(&sun, r0, r1, dt, plain.v0, &nearest, &variant));
	check_true("the nearest variant to the direct arc is the direct arc", variant == 0 &&
		mag_vec3(subtract_vec3(nearest.v0, plain.v0)) == 0);
	check_true("the variant output is optional", nearest_leg_variant_lambert(&sun, r0, r1, dt, plain.v0, &nearest, NULL));
	check_true("variant count covers the revolution limit", LEG_VARIANT_COUNT == 1 + 2 * LEG_VARIANT_MAX_REVS);
}

static void check_resonant_samples(void) {
	int count = resonant_departure_sample_count();
	check_true("resonant samples cover one and two revolutions", count == 224);
	double period = 365.25;
	double duration;
	int variant;
	resonant_departure_sample(0, period, &duration, &variant);
	check_true("the first sample is a direct arc", variant == 0);
	check_near("the first sample lies in the first period window", duration, period * (0.75 + 0.5 * 0.5 / 16), 1e-9);
	int in_range = 1, window_ok = 1;
	for(int index = 0; index < count; index++) {
		resonant_departure_sample(index, period, &duration, &variant);
		int revolutions = index < 80 ? 1 : 2;
		in_range &= variant >= 0 && variant < LEG_VARIANT_COUNT;
		window_ok &= duration >= 0.75 * revolutions * period && duration <= 1.25 * revolutions * period;
	}
	check_true("every sample uses a supported leg variant", in_range);
	check_true("every sample duration stays within 25 percent of the revolutions", window_ok);
	resonant_departure_sample(count - 1, period, &duration, &variant);
	check_true("the last sample is the highest variant of the second window", variant == 8 && duration > 1.7 * period);
}

static double polar_angle(Vector3 position) {
	double angle = atan2(position.y, position.x);
	return angle < 0 ? angle + 2 * M_PI : angle;
}

static void check_conjunction_times(void) {
	Body sun = {.mu = 1.32712440018e20};
	double radius = 1.496e11;
	/* A slightly eccentric orbit, because the true anomaly of a circular one is undefined. */
	OSV target = {vec3(radius, 0, 0), vec3(0, 1.03 * sqrt(sun.mu / radius), 0)};
	double period = calc_orbital_period(constr_orbit_from_osv(target.r, target.v, &sun));
	double conjunction, opposition;

	calc_time_to_next_conjunction_and_opposition(vec3(0, 1.08e11, 0), target, &sun, &conjunction, &opposition);
	check_near("the target reaches the observer's direction at the conjunction",
		polar_angle(propagate_osv_time(target, &sun, conjunction).r), M_PI / 2, 1e-6);
	check_near("the target is opposite the observer at the opposition",
		polar_angle(propagate_osv_time(target, &sun, opposition).r), 3 * M_PI / 2, 1e-6);
	check_true("both events lie within one orbit ahead", conjunction > 0 && opposition > 0 &&
		conjunction < period && opposition < period);

	calc_time_to_next_conjunction_and_opposition(vec3(0, -1.08e11, 0), target, &sun, &conjunction, &opposition);
	check_near("an observer behind the target meets it three quarters of a turn later",
		polar_angle(propagate_osv_time(target, &sun, conjunction).r), 3 * M_PI / 2, 1e-6);
	check_near("its opposition lies a quarter turn ahead",
		polar_angle(propagate_osv_time(target, &sun, opposition).r), M_PI / 2, 1e-6);
}

static struct ItinStep *build_route(CelestSystem *system, double departure_jd) {
	struct ItinStep *first = make_step(get_body_by_name("EARTH", system), departure_jd, NULL);
	struct ItinStep *middle = make_step(get_body_by_name("VENUS", system), departure_jd + 140, first);
	make_step(get_body_by_name("MARS", system), departure_jd + 330, middle);
	update_itin_body_osvs(first, system);
	calc_itin_v_vectors_from_dates_and_r(first, system);
	return first;
}

/* Names the first field in which two routes differ, or NULL if they are equal. */
static const char *route_difference(struct ItinStep *a, struct ItinStep *b) {
	while(a != NULL && b != NULL) {
		if(a->body == NULL || b->body == NULL || strcmp(a->body->name, b->body->name) != 0) return "body";
		if(a->date != b->date) return "date";
		if(mag_vec3(subtract_vec3(a->r, b->r)) != 0) return "position";
		if(mag_vec3(subtract_vec3(a->v_dep, b->v_dep)) != 0) return "departure velocity";
		if(mag_vec3(subtract_vec3(a->v_arr, b->v_arr)) != 0) return "arrival velocity";
		if(mag_vec3(subtract_vec3(a->v_body, b->v_body)) != 0) return "body velocity";
		a = a->num_next_nodes > 0 ? a->next[0] : NULL;
		b = b->num_next_nodes > 0 ? b->next[0] : NULL;
	}
	return a == NULL && b == NULL ? NULL : "length";
}

static void check_routes_equal(const char *name, struct ItinStep *a, struct ItinStep *b) {
	const char *difference = route_difference(a, b);
	if(difference != NULL) printf("route differs in %s\n", difference);
	check_true(name, difference == NULL);
}

static void check_itinerary_files(void) {
	char system_path[] = "../Celestial_Systems/solar_system.cfg";
	CelestSystem *system = load_celestial_system_from_cfg_file(system_path);
	check_true("the test system loads", system != NULL);
	if(system == NULL) return;

	double departure_jd = system->ut0 + 100;
	struct ItinStep *route = build_route(system, departure_jd);
	OSV earth = osv_from_elements(get_body_by_name("EARTH", system)->orbit, departure_jd);
	check_near("body states are written to the steps", mag_vec3(subtract_vec3(route->r, earth.r)), 0, 1e-6);
	check_near("body velocity is written to the steps", mag_vec3(subtract_vec3(route->v_body, earth.v)), 0, 1e-9);
	Lambert3 first_leg = calc_lambert3(route->r, route->next[0]->r, 140 * 86400.0, system->cb);
	check_near("leg velocities come from the dates and positions", mag_vec3(subtract_vec3(route->next[0]->v_dep, first_leg.v0)), 0, 1e-9);
	check_near("arrival velocities come from the dates and positions", mag_vec3(subtract_vec3(route->next[0]->v_arr, first_leg.v1)), 0, 1e-9);

	struct ItinStep *venus_step = route->next[0];
	struct ItinStep *maneuver = make_step(NULL, departure_jd + 70, route);
	route->next[0] = maneuver;
	route->num_next_nodes = 1;
	update_itin_body_osvs(route, system);
	check_near("a maneuver node is marked by a unit body velocity", maneuver->v_body.x, 1, 0);
	route->next[0] = venus_step;
	free(maneuver->next);
	free(maneuver);

	check_true("the field counter counts separators", amt_of_fields("a,b,c") == 3 && amt_of_fields("a") == 1);
	check_true("the newest binary file type is the default", get_current_bin_file_type() == 5);

	char single_path[] = "unit_test_route.itin";
	store_single_itinerary_in_bfile(route, system, single_path);
	struct ItinLoadFileResults loaded = load_single_itinerary_from_bfile(single_path);
	check_true("a stored route is loaded again", loaded.itin != NULL && loaded.system != NULL);
	if(loaded.itin != NULL) {
		check_routes_equal("the loaded route equals the stored one", route, get_first(loaded.itin));
		free_itinerary(get_first(loaded.itin));
	}
	remove(single_path);

	struct ItinStep *departures[1] = {route};
	Body *flyby_bodies[1] = {get_body_by_name("VENUS", system)};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd + 5;
	calc_data.jd_max_arr = departure_jd + 400;
	calc_data.max_duration = 330;
	calc_data.step_dep_date = 1;
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {.type = ITIN_SEQ_INFO_TO_TARGET, .system = system,
		.dep_body = get_body_by_name("EARTH", system), .arr_body = get_body_by_name("MARS", system),
		.flyby_bodies = flyby_bodies, .num_flyby_bodies = 1};
	char multi_path[] = "unit_test_routes.itins";
	store_itineraries_in_bfile(departures, 3, 1, 1, calc_data, system, multi_path, get_current_bin_file_type());

	FILE *file = fopen(multi_path, "rb");
	check_true("the itineraries file is written", file != NULL);
	if(file != NULL) {
		ItinStepBinHeaderData header = get_itins_bfile_header(file);
		check_true("the header keeps the counts", header.num_deps == 1 && header.num_nodes == 3 && header.num_itins == 1);
		check_near("the header keeps the departure window", header.calc_data.jd_min_dep, departure_jd, 0);
		check_true("the header keeps the route bodies", header.calc_data.seq_info.to_target.num_flyby_bodies == 1);
		fclose(file);
	}
	struct ItinsLoadFileResults results = load_itineraries_from_bfile(multi_path);
	check_true("the itineraries are loaded again", results.departures != NULL && results.header.num_deps == 1);
	if(results.departures != NULL) {
		check_routes_equal("the loaded itinerary equals the stored one", route, results.departures[0]);
		free_itinerary(results.departures[0]);
		free(results.departures);
	}
	remove(multi_path);
	free_itinerary(route);
	free_celestial_system(system);
}

static void check_minimum_leg(void) {
	Body sun = {.mu = 1.32712440018e20};
	Body mercury = {.orbit = {.a = 5.79e10, .e = 0.2}};
	Body earth = {.orbit = {.a = 1.496e11}};
	Body *planets[] = {&earth, &mercury};
	CelestSystem solar = {.cb = &sun, .num_bodies = 2, .bodies = planets};
	check_near("a system without fast orbits keeps the 10 day minimum", minimum_leg_days(&solar), 10, 1e-9);
	check_near("no system keeps the 10 day minimum", minimum_leg_days(NULL), 10, 1e-9);

	Body jupiter = {.mu = 1.26686534e17};
	Body io = {.orbit = {.a = 4.217e8}};
	Body europa = {.orbit = {.a = 6.711e8}};
	Body *moons[] = {&europa, &io};
	CelestSystem jovian = {.cb = &jupiter, .num_bodies = 2, .bodies = moons};
	double io_period_days = 2 * M_PI * sqrt(io.orbit.a * io.orbit.a * io.orbit.a / jupiter.mu) / 86400;
	check_near("a moon system uses an eighth of the shortest period", minimum_leg_days(&jovian), io_period_days / 8, 1e-9);
	check_true("moon legs are allowed below 10 days", minimum_leg_days(&jovian) < 1);

	Body tiny = {.orbit = {.a = 1e6}};
	Body *tiny_bodies[] = {&tiny};
	CelestSystem compact = {.cb = &jupiter, .num_bodies = 1, .bodies = tiny_bodies};
	check_near("the minimum never drops below the floor", minimum_leg_days(&compact), 0.05, 1e-12);
	check_near("a system change is picked up", minimum_leg_days(&solar), 10, 1e-9);
}

static void check_search_rejections(void) {
	char text[200];
	reset_search_rejections();
	Itin_Rejection_Counts counts = get_search_rejections();
	describe_search_rejections(&counts, text, sizeof(text));
	check_true("nothing is rejected at the start", counts.by_cause[REJECT_TOTAL_DV] == 0 && text[0] == '\0');

	for(int i = 0; i < 3; i++) count_search_rejection(REJECT_TOTAL_DV);
	count_search_rejection(REJECT_DEPARTURE_DV);
	count_search_rejection(SEARCH_REJECTION_COUNT);
	counts = get_search_rejections();
	check_true("rejections are counted per cause", counts.by_cause[REJECT_TOTAL_DV] == 3 &&
		counts.by_cause[REJECT_DEPARTURE_DV] == 1 && counts.by_cause[REJECT_SATELLITE_DV] == 0);
	describe_search_rejections(&counts, text, sizeof(text));
	check_true("the limit behind most rejections is named", strstr(text, "total delta-v") != NULL && strstr(text, "3 of 4") != NULL);

	char tiny[1];
	describe_search_rejections(&counts, tiny, sizeof(tiny));
	check_true("a short buffer stays terminated", tiny[0] == '\0');

	reset_search_rejections();
	counts = get_search_rejections();
	check_true("a new search starts from zero", counts.by_cause[REJECT_TOTAL_DV] == 0 && counts.by_cause[REJECT_DEPARTURE_DV] == 0);
}

int main(void) {
	check_itinerary_tree();
	check_flyby_policy();
	check_leg_variants();
	check_resonant_samples();
	check_conjunction_times();
	check_itinerary_files();
	check_search_rejections();
	check_minimum_leg();
	return finish_tests("KMAT itinerary unit checks");
}
