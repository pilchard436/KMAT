#include "test_support.h"
#include "orbit_calculator/itin_tool.h"
#include "orbit_calculator/flyby_policy.h"
#include "orbit_calculator/search_rejections.h"
#include "orbit_calculator/soi_encounter.h"
#include "tools/file_io.h"
#include "tools/file_io_internal.h"
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
	struct ItinStep empty_departure = {0};
	struct ItinStep *departure_trees[] = {first, &empty_departure};
	struct ItinStep **collected_arrivals = NULL;
	int collected_count = 0, collected_nodes = 0;
	check_true("one-pass collection succeeds for branched departure trees",
		collect_itineraries_from_departures(departure_trees, 2, &collected_arrivals, &collected_count, &collected_nodes));
	check_true("one-pass collection preserves arrival order and skips empty roots",
		collected_count == 2 && collected_arrivals[0] == last && collected_arrivals[1] == second_arrival);
	check_true("one-pass collection counts every stored node", collected_nodes == 5);
	free(collected_arrivals);

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
	check_true("the powered flyby can be flown at the minimum periapsis", flyby_is_viable(flyby->v_arr, arrival->v_dep, body_velocity, &venus, flyby->date));
	check_true("the powered flyby stays above the safe radius", !flyby_below_min_safe(flyby));
	check_true("no flyby of the route is below the safe radius", count_flybys_below_min_safe(arrival) == 0);
	double largest = 0;
	check_true("one flyby needs a burn", count_flybys_needing_burn(arrival, &largest) == 1);
	check_near("the largest shortfall is reported", largest, expected_shortfall, 1e-3);
	check_true("the largest shortfall is optional", count_flybys_needing_burn(arrival, NULL) == 1);

	PoweredFlybyResult safe_flyby;
	check_true("a powered burn keeps periapsis safe when the unpowered turn would not",
		calculate_powered_flyby(flyby->v_arr, arrival->v_dep, body_velocity, &venus,
			flyby_minimum_periapsis(&venus), &safe_flyby) && safe_flyby.periapsis_radius >= safe - 1 &&
			safe_flyby.burn_delta_v > 0);
	check_true("safe periapsis is the only flyby policy", !flyby_below_min_safe(flyby));
	check_true("no flyby of the route is below the safe radius", count_flybys_below_min_safe(arrival) == 0);

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
	check_true("dual-mode itineraries are the default", get_current_bin_file_type() == 8);

	ItinSearchOptions search_options = {
		.known = 1,
		.allow_dsm = 1,
		.moon_aware_flybys = 1,
		.moon_assist_arrival = 1,
		.allow_resonant = 1,
		.last_transfer_type = TF_CAPTURE,
		.use_custom_starting_orbit = 1,
		.starting_orbit_body_id = get_body_system_id(route->body, system),
		.starting_orbit = {
			.semi_major_axis = route->body->radius + route->body->atmo_alt + 200e3,
			.eccentricity = 0.1,
			.inclination = 0.2,
			.raan = 0.3,
			.arg_periapsis = 0.4,
			.true_anomaly = 0.5
		}
	};
	char single_path[] = "unit_test_route.itin";
	struct ItinStep *single_arrival = get_last(route);
	double single_dep_periapsis = route->body->atmo_alt + 50e3;
	double single_arr_periapsis = single_arrival->body->atmo_alt + 50e3;
	int previous_moon_assist = set_moon_assist_enabled(1);
	single_arrival->arrival_dv_cache = (struct ItinArrivalDvCache) {
		.dv_arr_cap = 321,
		.dv_arr_circ = 654,
		.incoming_excess = subtract_vec3(single_arrival->v_arr, single_arrival->v_body),
		.date = single_arrival->date,
		.arr_periapsis = single_arr_periapsis,
		.moon_assist_enabled = 1,
		.valid = 1
	};
	struct PorkchopPoint single_saved_values = create_porkchop_point(single_arrival,
		single_dep_periapsis, single_arr_periapsis);
	store_single_itinerary_in_bfile_with_options(route, system, single_path, search_options,
		single_dep_periapsis, single_arr_periapsis);
	struct ItinLoadFileResults loaded = load_single_itinerary_from_bfile(single_path);
	check_true("a stored route is loaded again", loaded.itin != NULL && loaded.system != NULL);
	check_true("single-route files preserve delta-v results",
		loaded.arrival_dv_cache_stored && get_last(loaded.itin)->arrival_dv_cache.valid &&
		get_last(loaded.itin)->arrival_dv_cache.moon_assist_enabled);
	if(loaded.itin != NULL) {
		struct ItinStep *loaded_arrival = get_last(loaded.itin);
		check_near("single-route files preserve departure delta-v",
			loaded_arrival->arrival_dv_cache.dv_dep, single_saved_values.dv_dep, 1e-9);
		check_near("single-route files preserve DSM delta-v",
			loaded_arrival->arrival_dv_cache.dv_dsm, single_saved_values.dv_dsm, 1e-9);
		check_near("single-route files preserve flyby delta-v",
			loaded_arrival->arrival_dv_cache.dv_flyby, single_saved_values.dv_flyby, 1e-9);
		check_near("single-route files preserve assisted capture delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_cap, single_saved_values.dv_arr_cap, 1e-9);
		check_near("single-route files preserve assisted circularization delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_circ, single_saved_values.dv_arr_circ, 1e-9);
		check_near("single-route files preserve departure periapsis",
			loaded.departure_periapsis, single_dep_periapsis, 0);
		check_near("single-route files preserve arrival periapsis",
			loaded.arrival_periapsis, single_arr_periapsis, 0);
		cache_itinerary_arrival_dvs(loaded_arrival, single_arr_periapsis);
		check_near("loaded single-route arrival delta-v cache is reused",
			loaded_arrival->arrival_dv_cache.dv_arr_cap, 321, 0);
	}
	check_true("single-route files preserve search options",
		loaded.search_options.known && loaded.search_options.allow_dsm &&
		loaded.search_options.moon_aware_flybys && loaded.search_options.moon_assist_arrival &&
		loaded.search_options.allow_resonant && loaded.search_options.last_transfer_type == TF_CAPTURE &&
		loaded.search_options.use_custom_starting_orbit &&
		loaded.search_options.starting_orbit_body_id == search_options.starting_orbit_body_id);
	check_near("single-route files preserve the custom starting orbit",
		loaded.search_options.starting_orbit.semi_major_axis, search_options.starting_orbit.semi_major_axis, 0);
	if(loaded.itin != NULL) {
		check_routes_equal("the loaded route equals the stored one", route, get_first(loaded.itin));
		free_itinerary(get_first(loaded.itin));
		free_celestial_system(loaded.system);
	}
	remove(single_path);
	set_moon_assist_enabled(previous_moon_assist);

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
	calc_data.search_options = search_options;
	calc_data.search_options.last_transfer_type = TF_BOTH;
	calc_data.dv_filter.dep_periapsis = single_dep_periapsis;
	calc_data.dv_filter.arr_periapsis = get_last(route)->body->atmo_alt + 50e3;

	struct ItinStep *capture_departures[1] = {route};
	Itin_Calc_Results both_results = {
		.departures = departures,
		.num_deps = 1,
		.num_nodes = 3,
		.num_itins = 1,
		.capture_departures = capture_departures,
		.capture_num_deps = 1,
		.capture_num_nodes = 3,
		.capture_num_itins = 1
	};
	struct ItinStep *arrival = get_last(route);
	int previous_itins_moon_assist = set_moon_assist_enabled(1);
	struct PorkchopPoint itins_saved_values = create_porkchop_point(arrival,
		calc_data.dv_filter.dep_periapsis, calc_data.dv_filter.arr_periapsis);
	char dual_path[] = "unit_test_routes_both.itins";
	store_itineraries_in_bfile_both_modes(both_results, calc_data, system, dual_path);
	struct ItinsLoadFileResults both_loaded = load_itineraries_from_bfile(dual_path);
	check_true("dual-mode itinerary results are loaded",
		both_loaded.header.file_type == 8 && both_loaded.header.calc_data.dv_filter.last_transfer_type == TF_BOTH &&
		both_loaded.header.calc_data.search_options.known &&
		both_loaded.header.calc_data.search_options.allow_dsm &&
		both_loaded.header.calc_data.search_options.moon_aware_flybys &&
		both_loaded.header.calc_data.search_options.moon_assist_arrival &&
		both_loaded.header.calc_data.search_options.allow_resonant &&
		both_loaded.departures != NULL && both_loaded.capture_departures != NULL &&
		both_loaded.capture_num_deps == 1 && both_loaded.capture_num_itins == 1);
	if(both_loaded.departures != NULL) {
		check_routes_equal("dual-mode Fly-By routes round-trip", route, both_loaded.departures[0]);
		struct ItinStep *loaded_arrival = get_last(both_loaded.departures[0]);
		check_true("dual-mode files preserve route delta-v results",
			loaded_arrival->arrival_dv_cache.valid &&
			loaded_arrival->arrival_dv_cache.moon_assist_enabled);
		check_near("dual-mode files preserve departure delta-v",
			loaded_arrival->arrival_dv_cache.dv_dep, itins_saved_values.dv_dep, 1e-9);
		check_near("dual-mode files preserve DSM delta-v",
			loaded_arrival->arrival_dv_cache.dv_dsm, itins_saved_values.dv_dsm, 1e-9);
		check_near("dual-mode files preserve flyby delta-v",
			loaded_arrival->arrival_dv_cache.dv_flyby, itins_saved_values.dv_flyby, 1e-9);
		check_near("dual-mode files preserve assisted capture delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_cap, itins_saved_values.dv_arr_cap, 1e-9);
		check_near("dual-mode files preserve assisted circularization delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_circ, itins_saved_values.dv_arr_circ, 1e-9);
		int previous_moon_assist = set_moon_assist_enabled(1);
		cache_itinerary_arrival_dvs(loaded_arrival, calc_data.dv_filter.arr_periapsis);
		set_moon_assist_enabled(previous_moon_assist);
		check_near("loaded arrival delta-v cache matches route inputs and is reused",
			loaded_arrival->arrival_dv_cache.dv_arr_cap, 321, 0);
		free_itinerary(both_loaded.departures[0]);
		free(both_loaded.departures);
	}
	set_moon_assist_enabled(previous_itins_moon_assist);
	if(both_loaded.capture_departures != NULL) {
		check_routes_equal("dual-mode Capture routes round-trip", route, both_loaded.capture_departures[0]);
		free_itinerary(both_loaded.capture_departures[0]);
		free(both_loaded.capture_departures);
	}
	remove(dual_path);
	free_itinerary(route);
	free_celestial_system(system);
}

static void check_moons_are_saved(void) {
	char path[] = "../Celestial_Systems/stock_system.cfg";
	CelestSystem *system = load_celestial_system_from_cfg_file(path);
	check_true("the moon test system loads", system != NULL);
	if(system == NULL) return;
	char file_path[] = "unit_test_system.bin";
	FILE *file = fopen(file_path, "w+b");
	check_true("the system file opens", file != NULL);
	if(file == NULL) { free_celestial_system(system); return; }
	store_celestial_system_in_bfile(system, file, *current_itin_file_types);
	rewind(file);
	CelestSystem *loaded = load_celestial_system_from_bfile(file, *current_itin_file_types);
	fclose(file);
	remove(file_path);
	check_true("the system is loaded again", loaded != NULL && loaded->num_bodies == system->num_bodies);
	int planets_with_moons = 0, moons_match = 1;
	for(int i = 0; loaded != NULL && i < system->num_bodies; i++) {
		Body *planet = system->bodies[i], *copy = loaded->bodies[i];
		if(strcmp(planet->name, copy->name) != 0) moons_match = 0;
		int expected = planet->system != NULL ? planet->system->num_bodies : 0;
		if(expected > 0) planets_with_moons++;
		if((copy->system != NULL ? copy->system->num_bodies : 0) != expected) { moons_match = 0; continue; }
		for(int j = 0; j < expected; j++) {
			Body *moon = planet->system->bodies[j], *moon_copy = copy->system->bodies[j];
			if(strcmp(moon->name, moon_copy->name) != 0 || moon_copy->orbit.cb != copy || fabs(moon->orbit.a - moon_copy->orbit.a) > 1e-6 ||
				fabs(moon->mu - moon_copy->mu) > 1e-6 || fabs(moon->radius - moon_copy->radius) > 1e-6) moons_match = 0;
		}
	}
	check_true("the test system has planets with moons", planets_with_moons > 0);
	check_true("the moons are saved with their planets", moons_match);
	if(loaded != NULL) free_celestial_system(loaded);
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

static void check_unmodeled_soi_detection(void) {
	Body sun = {.mu = 1e14};
	Body planet = {.mu = 1e10, .orbit = {.a = 1e8, .cb = &sun}};
	Body *planets[] = {&planet};
	CelestSystem solar = {.cb = &sun, .num_bodies = 1, .bodies = planets,
		.prop_method = ORB_ELEMENTS, .ut0 = 0};
	sun.system = &solar;
	double planet_soi = planet.orbit.a * pow(planet.mu / sun.mu, 0.4);
	OSV inside_planet_soi = {{1e8 + planet_soi / 2, 0, 0}, {0, 1e3, 0}};
	OSV clear_of_planet = {{1e8 + 1e7, 0, 0}, {0, 1e3, 0}};
	check_true("an unmodeled planet SOI crossing is detected",
		coast_leg_crosses_unmodeled_soi(&solar, NULL, NULL, inside_planet_soi, 0, 1e-5));
	check_true("a clear interplanetary coast is accepted",
		!coast_leg_crosses_unmodeled_soi(&solar, NULL, NULL, clear_of_planet, 0, 1e-5));
	check_true("a modeled endpoint body is exempt from SOI rejection",
		!coast_leg_crosses_unmodeled_soi(&solar, &planet, NULL, inside_planet_soi, 0, 1e-5));

	Body moon = {.mu = 1e6, .orbit = {.a = 1e6, .cb = &planet}};
	Body *moons[] = {&moon};
	CelestSystem planetary = {.cb = &planet, .num_bodies = 1, .bodies = moons,
		.prop_method = ORB_ELEMENTS, .ut0 = 0};
	planet.system = &planetary;
	double moon_soi = moon.orbit.a * pow(moon.mu / planet.mu, 0.4);
	OSV inside_moon_soi = {{1e8 + 1e6 + moon_soi / 2, 0, 0}, {0, 1e3, 0}};
	check_true("nested moon SOIs are detected in central-body coordinates",
		coast_leg_crosses_unmodeled_soi(&solar, &planet, NULL, inside_moon_soi, 0, 1e-5));
}

void check_moon_flybys(void);

int run_itinerary_unit_tests(void) {
	check_itinerary_tree();
	check_flyby_policy();
	check_leg_variants();
	check_resonant_samples();
	check_conjunction_times();
	check_itinerary_files();
	check_search_rejections();
	check_minimum_leg();
	check_moons_are_saved();
	check_moon_flybys();
	check_unmodeled_soi_detection();
	int failed = finish_tests("KMAT itinerary unit checks");
	checks = failures = 0;
	return failed;
}
