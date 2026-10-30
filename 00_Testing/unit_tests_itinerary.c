#include "test_support.h"
#include "orbit_calculator/itin_tool.h"
#include "orbit_calculator/flyby_policy.h"
#include "orbit_calculator/flyby_search.h"
#include "orbit_calculator/search_rejections.h"
#include "orbit_calculator/soi_encounter.h"
#include "tools/file_io.h"
#include "tools/file_io_internal.h"
#include "tools/thread_pool.h"
#include "orbitlib_fileio.h"
#include "orbitlib_transfer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

static atomic_int thread_pool_test_calls;

static void *count_thread_pool_test_call(void *args) {
	(void)args;
	atomic_fetch_add(&thread_pool_test_calls, 1);
	return NULL;
}

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
	middle->body = NULL;
	middle->v_arr = vec3(1, 2, 3);
	last->v_dep = vec3(4, 2, 3);
	second_arrival->v_dep = vec3(1, 7, 3);
	check_near("branch-specific DSM cost uses the selected child", itinerary_transition_dv(middle, last, 0), 3, 1e-12);
	check_near("sibling DSM branch has its own cost", itinerary_transition_dv(middle, second_arrival, 0), 5, 1e-12);
	middle->body = &venus;
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

	last->dv_from_prev = 42;
	last->dv_from_prev_valid = 1;
	struct ItinStep *chain = create_itin_copy_from_arrival(last);
	check_true("arrival copy rebuilds the single route", get_num_of_itin_layers(get_first(chain)) == 3 &&
		get_first(chain)->date == 100 && chain->date == 330 && chain->body == &mars);
	check_true("arrival copy is detached from the original", get_first(chain) != first && chain->num_next_nodes == 0);
	check_true("arrival copy preserves the selected branch's cached step delta-v",
		chain->dv_from_prev_valid && chain->dv_from_prev == 42);
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

	// A near-resonant return two years later, where several revolution counts have solutions.
	Vector3 resonant_r1 = vec3(1.496e11 * cos(0.4), 1.496e11 * sin(0.4), 2e9);
	double resonant_dt = 2.1 * 365.25 * 86400.0;
	Lambert3 batch[LEG_VARIANT_COUNT];
	calc_leg_variant_lamberts(&sun, r0, resonant_r1, resonant_dt, LEG_VARIANT_COUNT, batch, NULL, NULL);
	int batch_matches = 1, batch_usable = 0;
	for(int v = 0; v < LEG_VARIANT_COUNT; v++) {
		Lambert3 single = calc_leg_variant_lambert(&sun, r0, resonant_r1, resonant_dt, v, NULL, NULL);
		batch_matches &= single.success == batch[v].success;
		if(!is_usable_lambert(single)) continue;
		batch_usable++;
		batch_matches &= mag_vec3(subtract_vec3(single.v0, batch[v].v0)) == 0 &&
			mag_vec3(subtract_vec3(single.v1, batch[v].v1)) == 0;
	}
	check_true("batch leg variants equal the single-variant solutions", batch_matches);
	check_true("batch leg variants include multi-revolution solutions", batch_usable >= 3);
	Lambert3 partial[2];
	calc_leg_variant_lamberts(&sun, r0, resonant_r1, resonant_dt, 2, partial, NULL, NULL);
	check_true("a partial batch solves only the requested variants",
		mag_vec3(subtract_vec3(partial[1].v0, batch[1].v0)) == 0 && partial[1].success == batch[1].success);
}

// Pruning end nodes computes the same route costs as create_porkchop_point and leaves them cached on the node.
static void check_end_node_pruning(void) {
	Body sun = {.mu = 1.32712440018e20, .radius = 6.96e8};
	Body earth = {.name = "E", .mu = 3.986e14, .radius = 6.371e6, .atmo_alt = 1e5};
	Body venus = {.name = "V", .mu = 3.24859e14, .radius = 6.0518e6, .atmo_alt = 0};
	Body mars = {.name = "M", .mu = 4.2828e13, .radius = 3.3895e6, .atmo_alt = 0};
	earth.orbit.a = 1.496e11; earth.orbit.cb = &sun;
	venus.orbit.a = 1.082e11; venus.orbit.cb = &sun;
	mars.orbit.a = 2.279e11; mars.orbit.cb = &sun;
	struct ItinStep *departure = make_step(&earth, 0, NULL);
	struct ItinStep *first_flyby = make_step(&venus, 150, departure);
	struct ItinStep *second_flyby = make_step(&earth, 400, first_flyby);
	struct ItinStep *arrivals[2] = {make_step(&mars, 700, second_flyby), make_step(&mars, 760, second_flyby)};
	departure->v_body = vec3(0, 29800, 0);
	first_flyby->v_dep = vec3(2500, 27000, 0);
	first_flyby->v_body = vec3(-35000, 0, 0);
	first_flyby->v_arr = add_vec3(first_flyby->v_body, vec3(4000, 1500, 200));
	second_flyby->v_dep = add_vec3(first_flyby->v_body, vec3(3900, -1800, -300));
	second_flyby->v_body = vec3(0, -29800, 0);
	second_flyby->v_arr = add_vec3(second_flyby->v_body, vec3(6000, 2000, 100));
	for(int i = 0; i < 2; i++) {
		arrivals[i]->v_dep = add_vec3(second_flyby->v_body, vec3(5800 + 200 * i, 2300, -100 + 50 * i));
		arrivals[i]->v_body = vec3(24000, 0, 0);
		arrivals[i]->v_arr = add_vec3(arrivals[i]->v_body, vec3(2500, 1000 * i, 0));
	}
	double dep_periapsis = 2e5, arr_periapsis = 1e5;
	struct Dv_Filter generous = {.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = dep_periapsis, .arr_periapsis = arr_periapsis, .last_transfer_type = TF_FLYBY};
	struct PorkchopPoint expected[2];
	for(int i = 0; i < 2; i++) {
		expected[i] = create_porkchop_point(arrivals[i], dep_periapsis, arr_periapsis);
		arrivals[i]->arrival_dv_cache = (struct ItinArrivalDvCache){0};
	}
	check_true("generous limits keep both end nodes",
		remove_end_nodes_that_do_not_satisfy_dv_requirements(second_flyby, 2, &generous) == 2 && second_flyby->num_next_nodes == 2);
	int cached_match = 1;
	for(int i = 0; i < 2; i++) {
		struct ItinArrivalDvCache cache = arrivals[i]->arrival_dv_cache;
		cached_match &= cache.route_values_valid && fabs(cache.dv_dep - expected[i].dv_dep) < 1e-9 &&
			fabs(cache.dv_flyby - expected[i].dv_flyby) < 1e-9 && fabs(cache.dv_dsm - expected[i].dv_dsm) < 1e-9 &&
			fabs(cache.dv_arr_cap - expected[i].dv_arr_cap) < 1e-9;
	}
	check_true("pruning stores the route costs create_porkchop_point computes", cached_match);
	check_true("pruning caches the flyby burns into the ancestors",
		second_flyby->flyby_burn_cache.valid && first_flyby->flyby_burn_cache.valid == 0 &&
		arrivals[0]->flyby_burn_cache.valid && arrivals[1]->flyby_burn_cache.valid);

	// A changed velocity at the shared flyby invalidates the cached burn for the next pruning pass.
	second_flyby->v_dep = add_vec3(first_flyby->v_body, vec3(3600, -2200, 100));
	arrivals[0]->arrival_dv_cache = (struct ItinArrivalDvCache){0};
	struct PorkchopPoint changed = create_porkchop_point(arrivals[0], dep_periapsis, arr_periapsis);
	arrivals[0]->arrival_dv_cache = (struct ItinArrivalDvCache){0};
	remove_end_nodes_that_do_not_satisfy_dv_requirements(second_flyby, 2, &generous);
	check_near("pruning recomputes a flyby burn whose inputs changed",
		arrivals[0]->arrival_dv_cache.dv_flyby, changed.dv_flyby, 1e-9);
	check_true("the changed flyby burn differs from the cached one", fabs(changed.dv_flyby - expected[0].dv_flyby) > 1);

	struct Dv_Filter tight = generous;
	tight.max_satdv = 1;
	check_true("a tight satellite limit prunes the powered end nodes",
		remove_end_nodes_that_do_not_satisfy_dv_requirements(second_flyby, 2, &tight) == 0 && second_flyby->num_next_nodes == 0);
	free_itinerary(departure);
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

static void check_powered_seed_budget_presets(void) {
	int ascending = 1, found_default = 0, round_trip = 1;
	for(int i = 0; i < POWERED_SEED_BUDGET_PRESET_COUNT; i++) {
		int budget = get_powered_seed_budget_preset(i);
		if(i > 0) ascending &= budget > get_powered_seed_budget_preset(i - 1);
		found_default |= budget == POWERED_SEEDS_PER_DEPARTURE;
		round_trip &= find_powered_seed_budget_preset(budget) == i;
	}
	check_true("seed budget presets ascend", ascending);
	check_true("seed budget presets include the default", found_default);
	check_true("seed budget presets map back to their index", round_trip);
	check_true("a non-preset seed budget is not found", find_powered_seed_budget_preset(777) == -1);
	check_true("an out-of-range preset index falls back to the default",
		get_powered_seed_budget_preset(-1) == POWERED_SEEDS_PER_DEPARTURE &&
		get_powered_seed_budget_preset(POWERED_SEED_BUDGET_PRESET_COUNT) == POWERED_SEEDS_PER_DEPARTURE);
	int previous = get_powered_seed_budget();
	set_powered_seed_budget(1024);
	check_true("the seed budget can be changed", get_powered_seed_budget() == 1024);
	set_powered_seed_budget(0);
	set_powered_seed_budget(-5);
	check_true("a non-positive seed budget is ignored", get_powered_seed_budget() == 1024);
	set_powered_seed_budget(previous);
}

static void check_powered_seed_share(void) {
	begin_powered_seed_search(NULL);
	int total = get_powered_seed_budget();
	check_true("a new departure has its whole budget available", get_powered_seed_available() == total);
	int outer = begin_powered_seed_share(4);
	int first = get_powered_seed_available();
	check_true("a share among several bodies leaves seeds for the others", first < total && first >= total / 2);
	int inner = begin_powered_seed_share(2);
	check_true("nested shares shrink further", get_powered_seed_available() < first);
	end_powered_seed_share(inner);
	check_true("ending a nested share restores the outer limit", get_powered_seed_available() == first);
	end_powered_seed_share(outer);
	check_true("ending a share restores the whole budget", get_powered_seed_available() == total);
	outer = begin_powered_seed_share(1);
	check_true("a single child gets everything", get_powered_seed_available() == total);
	end_powered_seed_share(outer);
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
	check_true("dual-mode itineraries use the per-step delta-v format", get_current_bin_file_type() == 10);

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
	double single_arrival_radius = alt2radius(single_arrival->body, single_arr_periapsis);
	double single_arrival_vinf = mag_vec3(subtract_vec3(single_arrival->v_arr, single_arrival->v_body));
	int previous_moon_assist = set_moon_assist_enabled(1);
	single_arrival->arrival_dv_cache = (struct ItinArrivalDvCache) {
		.dv_arr_cap = 321,
		.dv_arr_circ = 654,
		.dv_arr_cap_plain = dv_capture(single_arrival->body, single_arrival_radius, single_arrival_vinf),
		.dv_arr_cap_assisted = 321,
		.dv_arr_circ_plain = dv_circ(single_arrival->body, single_arrival_radius, single_arrival_vinf),
		.dv_arr_circ_assisted = 654,
		.incoming_excess = subtract_vec3(single_arrival->v_arr, single_arrival->v_body),
		.date = single_arrival->date,
		.arr_periapsis = single_arr_periapsis,
		.moon_assist_enabled = 1,
		.moon_assist_dvs_valid = 1,
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
		set_moon_assist_enabled(0);
		cache_itinerary_arrival_dvs(loaded_arrival, single_arr_periapsis);
		check_true("disabling arrival moon assist refreshes the capture-cost cache",
			!loaded_arrival->arrival_dv_cache.moon_assist_enabled);
		Vector3 incoming_excess = subtract_vec3(loaded_arrival->v_arr, loaded_arrival->v_body);
		check_near("disabled arrival moon assist uses the plain capture cost",
			loaded_arrival->arrival_dv_cache.dv_arr_cap,
			dv_capture(loaded_arrival->body, alt2radius(loaded_arrival->body, single_arr_periapsis),
				mag_vec3(incoming_excess)), 1e-9);
		set_moon_assist_enabled(1);
		cache_itinerary_arrival_dvs(loaded_arrival, single_arr_periapsis);
		check_true("enabling arrival moon assist refreshes the capture-cost cache",
			loaded_arrival->arrival_dv_cache.moon_assist_enabled);
		loaded_arrival->arrival_dv_cache.route_values_valid = 1;
		loaded_arrival->arrival_dv_cache.dep_periapsis = single_dep_periapsis;
		loaded_arrival->arrival_dv_cache.dv_dep = 123456789;
		loaded_arrival->arrival_dv_cache.dv_dsm = 234567890;
		loaded_arrival->arrival_dv_cache.dv_flyby = 345678901;
		struct PorkchopPoint reused_route_values = create_porkchop_point(loaded_arrival,
			single_dep_periapsis, single_arr_periapsis);
		check_near("Porkchop analysis reuses saved departure delta-v",
			reused_route_values.dv_dep, 123456789, 0);
		check_near("Porkchop analysis reuses saved DSM delta-v",
			reused_route_values.dv_dsm, 234567890, 0);
		check_near("Porkchop analysis reuses saved flyby delta-v",
			reused_route_values.dv_flyby, 345678901, 0);
		double changed_dep_periapsis = single_dep_periapsis + 10000;
		struct PorkchopPoint recalculated_route_values = create_porkchop_point(loaded_arrival,
			changed_dep_periapsis, single_arr_periapsis);
		check_near("Porkchop analysis recalculates route values for a changed periapsis",
			loaded_arrival->arrival_dv_cache.dep_periapsis, changed_dep_periapsis, 0);
		check_true("changed periapsis does not reuse stale departure delta-v",
			recalculated_route_values.dv_dep != 123456789);
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
	struct ItinArrivalDvCache saved_arrival_dvs = arrival->arrival_dv_cache;
	char dual_path[] = "unit_test_routes_both.itins";
	store_itineraries_in_bfile_both_modes(both_results, calc_data, system, dual_path);
	struct ItinsLoadFileResults both_loaded = load_itineraries_from_bfile(dual_path);
	check_true("dual-mode itinerary results are loaded",
		both_loaded.header.file_type == 10 && both_loaded.header.calc_data.dv_filter.last_transfer_type == TF_BOTH &&
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
			loaded_arrival->arrival_dv_cache.moon_assist_enabled &&
			loaded_arrival->arrival_dv_cache.moon_assist_dvs_valid);
		check_near("dual-mode files preserve departure delta-v",
			loaded_arrival->arrival_dv_cache.dv_dep, itins_saved_values.dv_dep, 1e-9);
		check_near("dual-mode files preserve DSM delta-v",
			loaded_arrival->arrival_dv_cache.dv_dsm, itins_saved_values.dv_dsm, 1e-9);
		check_near("dual-mode files preserve flyby delta-v",
			loaded_arrival->arrival_dv_cache.dv_flyby, itins_saved_values.dv_flyby, 1e-9);
		check_near("dual-mode files preserve assisted capture delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_cap, itins_saved_values.dv_arr_cap, 1e-9);
		check_near("dual-mode files preserve unassisted capture delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_cap_plain,
			saved_arrival_dvs.dv_arr_cap_plain, 1e-9);
		check_near("dual-mode files preserve assisted capture delta-v separately",
			loaded_arrival->arrival_dv_cache.dv_arr_cap_assisted,
			saved_arrival_dvs.dv_arr_cap_assisted, 1e-9);
		check_near("dual-mode files preserve assisted circularization delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_circ, itins_saved_values.dv_arr_circ, 1e-9);
		check_near("dual-mode files preserve unassisted circularization delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_circ_plain,
			saved_arrival_dvs.dv_arr_circ_plain, 1e-9);
		struct ItinStep *source_step = get_first(route);
		struct ItinStep *loaded_step = get_first(both_loaded.departures[0]);
		while(source_step != NULL && source_step->next != NULL &&
			loaded_step != NULL && loaded_step->next != NULL) {
			struct ItinStep *source_next = source_step->next[0];
			struct ItinStep *loaded_next = loaded_step->next[0];
			check_true("dual-mode file marks cached step delta-v as valid",
				source_next->dv_from_prev_valid && loaded_next->dv_from_prev_valid);
			check_true("dual-mode file preserves each step delta-v",
				loaded_next->dv_from_prev == source_next->dv_from_prev ||
				(isnan(loaded_next->dv_from_prev) && isnan(source_next->dv_from_prev)));
			source_step = source_next;
			loaded_step = loaded_next;
		}
		int previous_moon_assist = set_moon_assist_enabled(1);
		cache_itinerary_arrival_dvs(loaded_arrival, calc_data.dv_filter.arr_periapsis);
		check_near("enabled moon assist reuses the saved assisted capture delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_cap, saved_arrival_dvs.dv_arr_cap_assisted, 0);
		set_moon_assist_enabled(0);
		cache_itinerary_arrival_dvs(loaded_arrival, calc_data.dv_filter.arr_periapsis);
		check_near("disabled moon assist selects the saved plain capture delta-v",
			loaded_arrival->arrival_dv_cache.dv_arr_cap, saved_arrival_dvs.dv_arr_cap_plain, 0);
		set_moon_assist_enabled(1);
		cache_itinerary_arrival_dvs(loaded_arrival, calc_data.dv_filter.arr_periapsis);
		set_moon_assist_enabled(previous_moon_assist);
		check_near("loaded arrival delta-v cache matches route inputs and is reused",
			loaded_arrival->arrival_dv_cache.dv_arr_cap, saved_arrival_dvs.dv_arr_cap_assisted, 0);
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

	Itin_Calc_Data no_assist_calc_data = calc_data;
	no_assist_calc_data.search_options.moon_assist_arrival = 0;
	char no_assist_path[] = "unit_test_routes_no_assist.itins";
	store_itineraries_in_bfile_both_modes(both_results, no_assist_calc_data, system, no_assist_path);
	struct ItinsLoadFileResults no_assist_loaded = load_itineraries_from_bfile(no_assist_path);
	check_true("itineraries calculated without moon assist do not expose a dual-cost cache",
		no_assist_loaded.departures != NULL &&
		!get_last(no_assist_loaded.departures[0])->arrival_dv_cache.moon_assist_dvs_valid);
	if(no_assist_loaded.departures != NULL) {
		free_itinerary(no_assist_loaded.departures[0]);
		free(no_assist_loaded.departures);
	}
	if(no_assist_loaded.capture_departures != NULL) {
		free_itinerary(no_assist_loaded.capture_departures[0]);
		free(no_assist_loaded.capture_departures);
	}
	if(no_assist_loaded.header.system != NULL) free_celestial_system(no_assist_loaded.header.system);
	remove(no_assist_path);

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

static void check_search_options_storage(void) {
	char path[] = "unit_test_options.bin";
	FILE *file = fopen(path, "w+b");
	check_true("the search options file opens", file != NULL);
	if(file == NULL) return;

	ItinSearchOptions defaults = {.known = 1, .allow_dsm = 1, .allow_resonant = 1, .last_transfer_type = TF_BOTH};
	ItinSearchOptions unpowered = defaults;
	unpowered.disable_powered_flybys = 1;
	ItinSearchOptionsBin raw;
	ItinSearchOptions loaded;

	store_itin_search_options(file, &defaults);
	rewind(file);
	check_true("default options are stored with the known bit only",
		fread(&raw, sizeof(raw), 1, file) == 1 && raw.known == ITIN_OPTIONS_BIN_KNOWN);
	rewind(file);
	check_true("default options load with powered flybys on",
		load_itin_search_options(file, &loaded) && loaded.known && !loaded.disable_powered_flybys &&
		loaded.allow_dsm && loaded.allow_resonant && loaded.last_transfer_type == TF_BOTH);

	rewind(file);
	store_itin_search_options(file, &unpowered);
	rewind(file);
	check_true("powered flybys off adds its own bit to the stored flag",
		fread(&raw, sizeof(raw), 1, file) == 1 &&
		raw.known == (ITIN_OPTIONS_BIN_KNOWN | ITIN_OPTIONS_BIN_POWERED_FLYBYS_DISABLED));
	rewind(file);
	check_true("powered flybys off survives a save and load",
		load_itin_search_options(file, &loaded) && loaded.known && loaded.disable_powered_flybys &&
		loaded.allow_dsm && loaded.allow_resonant && loaded.last_transfer_type == TF_BOTH);

	// A record written before the option existed holds exactly 1 in the flag and must read as powered flybys on.
	ItinSearchOptionsBin legacy = {.known = 1, .allow_dsm = 1, .moon_aware_flybys = 1, .last_transfer_type = TF_CAPTURE};
	rewind(file);
	fwrite(&legacy, sizeof(legacy), 1, file);
	rewind(file);
	check_true("a record from before the option loads with powered flybys on",
		load_itin_search_options(file, &loaded) && loaded.known && !loaded.disable_powered_flybys &&
		loaded.allow_dsm && loaded.moon_aware_flybys && loaded.last_transfer_type == TF_CAPTURE);

	rewind(file);
	store_itin_search_options(file, NULL);
	rewind(file);
	check_true("missing options load as unknown with powered flybys on",
		load_itin_search_options(file, &loaded) && !loaded.known && !loaded.disable_powered_flybys);
	fclose(file);
	remove(path);
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

static void check_route_limits(void) {
	Body sun = {.mu = 1.1723328e18};
	Body eve = {.orbit = {.a = 9.8327e9}};
	Body kerbin = {.orbit = {.a = 1.3599840256e10}};
	Body jool = {.orbit = {.a = 6.8773560320e10}};
	Body *planets[] = {&eve, &kerbin, &jool};
	CelestSystem kerbol = {.cb = &sun, .num_bodies = 3, .bodies = planets};
	double period_eve = 2 * M_PI * sqrt(pow(eve.orbit.a, 3) / sun.mu), period_kerbin = 2 * M_PI * sqrt(pow(kerbin.orbit.a, 3) / sun.mu);
	double synodic_days = 1 / (1 / period_eve - 1 / period_kerbin) / 86400;
	double hohmann_days = calc_hohmann_transfer(eve.orbit.a, kerbin.orbit.a, &sun).dur / 86400;
	double eve_to_kerbin = maximum_intermediate_leg_days(eve.orbit.a, &eve, &kerbin, &kerbol);
	check_near("a neighbouring pair is limited by its synodic period", eve_to_kerbin, LEG_HORIZON_SYNODIC_PERIODS * synodic_days, 1e-6);
	check_true("the synodic window exceeds the Hohmann window for neighbours", eve_to_kerbin > LEG_HORIZON_HOHMANN_DURATIONS * hohmann_days);
	check_true("the window keeps the long leveraging legs that good routes use", eve_to_kerbin > 220);
	double kerbin_to_jool = maximum_intermediate_leg_days(kerbin.orbit.a, &kerbin, &jool, &kerbol);
	double hohmann_jool_days = calc_hohmann_transfer(kerbin.orbit.a, jool.orbit.a, &sun).dur / 86400;
	check_near("a distant pair with a short synodic period is limited by the Hohmann time", kerbin_to_jool, LEG_HORIZON_HOHMANN_DURATIONS * hohmann_jool_days, 1e-6);
	check_true("the window is measured from the current radius", maximum_intermediate_leg_days(0.5 * kerbin.orbit.a, &kerbin, &jool, &kerbol) < kerbin_to_jool);
	check_true("resonant legs are not limited", isinf(maximum_intermediate_leg_days(kerbin.orbit.a, &kerbin, &kerbin, &kerbol)));
	Body unknown = {0};
	check_true("an unknown orbit is not limited", isinf(maximum_intermediate_leg_days(kerbin.orbit.a, &kerbin, &unknown, &kerbol)));
	check_true("no system is not limited", isinf(maximum_intermediate_leg_days(kerbin.orbit.a, &kerbin, &jool, NULL)));

	struct ItinStep departure = {.body = &kerbin}, flyby = {.body = &eve, .prev = &departure}, arrival = {.body = &jool, .prev = &flyby};
	check_true("the departure has no flybys", route_flyby_count(&departure) == 0);
	check_true("every step after the departure counts as a flyby", route_flyby_count(&flyby) == 1 && route_flyby_count(&arrival) == 2);
	check_true("no step has no flybys", route_flyby_count(NULL) == 0);

	check_true("the default flyby limit is the preset", get_max_flybys_per_route() == MAX_FLYBYS_PER_ROUTE);
	check_true("setting the limit returns the previous one", set_max_flybys_per_route(3) == MAX_FLYBYS_PER_ROUTE && get_max_flybys_per_route() == 3);
	check_true("limits below one are ignored", set_max_flybys_per_route(0) == 3 && get_max_flybys_per_route() == 3);
	check_true("limits above the preset are capped", set_max_flybys_per_route(MAX_FLYBYS_PER_ROUTE + 1) == 3 &&
		get_max_flybys_per_route() == MAX_FLYBYS_PER_ROUTE);
	set_max_flybys_per_route(MAX_FLYBYS_PER_ROUTE);
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

static void check_thread_pool_preserves_counters(void) {
	prepare_thread_pool_counters();
	incr_thread_counter_by_amount(2, 9);
	incr_thread_counter_by_amount(3, 1);
	check_true("the cancellation flag exposes the requested state",
		get_thread_counter(3) == 1);
	atomic_store(&thread_pool_test_calls, 0);
	struct Thread_Pool pool = use_thread_pool32_preserving_counters(
		count_thread_pool_test_call, NULL);
	join_thread_pool(pool);
	check_true("preserving worker pools run all workers",
		atomic_load(&thread_pool_test_calls) == (int)pool.size);
	check_true("preserving worker pools retain progress and cancellation counters",
		get_thread_counter(2) == 9 && get_thread_counter(3) == 1);
	prepare_thread_pool_counters();
	check_true("preparing counters clears the cancellation flag",
		get_thread_counter(3) == 0);
}

static void check_concurrent_thread_pools_have_independent_handles(void) {
	atomic_store(&thread_pool_test_calls, 0);
	struct Thread_Pool first = use_thread_pool32_preserving_counters_with_limit(
		count_thread_pool_test_call, NULL, 2);
	struct Thread_Pool second = use_thread_pool32_preserving_counters_with_limit(
		count_thread_pool_test_call, NULL, 2);
	check_true("concurrent pools own distinct worker handles",
		first.size > 0 && second.size > 0 && first.threads[0] != second.threads[0]);
	join_thread_pool(first);
	join_thread_pool(second);
	check_true("limited pools stay within their worker limits",
		first.size <= 2 && second.size <= 2);
	check_true("concurrent pools run every worker",
		atomic_load(&thread_pool_test_calls) == (int)(first.size + second.size));
}

static void check_thread_pool_worker_limit(void) {
	set_thread_pool_worker_limit(THREAD_POOL_MAX_WORKERS);
	size_t default_capacity = get_thread_pool32_worker_capacity();
	set_thread_pool_worker_limit(1);
	check_true("the worker cap can select one worker",
		get_thread_pool32_worker_capacity() == 1);
	set_thread_pool_worker_limit(3);
	size_t expected_capacity = default_capacity < 3 ? default_capacity : 3;
	check_true("the worker cap never exceeds available capacity",
		get_thread_pool32_worker_capacity() == expected_capacity);
	set_thread_pool_worker_limit(THREAD_POOL_MAX_WORKERS);
	check_true("the default worker capacity is restored",
		get_thread_pool32_worker_capacity() == default_capacity);
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
	check_end_node_pruning();
	check_resonant_samples();
	check_powered_seed_budget_presets();
	check_powered_seed_share();
	check_conjunction_times();
	check_itinerary_files();
	check_search_options_storage();
	check_search_rejections();
	check_thread_pool_preserves_counters();
	check_concurrent_thread_pools_have_independent_handles();
	check_thread_pool_worker_limit();
	check_minimum_leg();
	check_route_limits();
	check_moons_are_saved();
	check_moon_flybys();
	check_unmodeled_soi_detection();
	int failed = finish_tests("KMAT itinerary unit checks");
	checks = failures = 0;
	return failed;
}
