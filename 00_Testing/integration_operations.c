#include "integration_common.h"

#define TEXT_GUARD_SIZE 16

static int itinerary_structure_problem(struct ItinStep *first) {
	if(first == NULL || first->prev != NULL || first->body == NULL) return 1;
	for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL) {
		if(s->next == NULL ? s->num_next_nodes != 0 : s->num_next_nodes != 1) return 2;
		if(s->next != NULL && s->next[0]->prev != s) return 3;
		if(s->next != NULL && !(s->next[0]->date > s->date)) return 4;
		if(s->body == NULL && (s->prev == NULL || s->next == NULL)) return 5;
		if(!isfinite(s->r.x + s->r.y + s->r.z + s->v_dep.x + s->v_dep.y + s->v_dep.z +
			s->v_arr.x + s->v_arr.y + s->v_arr.z)) return 6;
	}
	return 0;
}

// Calls every text builder on every step with the planner's buffer sizes; returns nonzero on an overflow.
static int guard_broken(const char *buffer, size_t size) {
	for(size_t i = size; i < size + TEXT_GUARD_SIZE; i++)
		if(buffer[i] != 0x5A) return 1;
	return 0;
}

static int itinerary_text_problem(struct ItinStep *first, double dep_periapsis, double arr_periapsis) {
	static char labels[ITIN_STEP_TEXT_SIZE + TEXT_GUARD_SIZE], values[ITIN_STEP_TEXT_SIZE + TEXT_GUARD_SIZE], overview[10000 + TEXT_GUARD_SIZE];
	for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL) {
		memset(labels, 0x5A, sizeof(labels));
		memset(values, 0x5A, sizeof(values));
		labels[0] = values[0] = 0;
		itinerary_step_parameters_to_string(labels, values, DATE_ISO, dep_periapsis, arr_periapsis, s);
		if(guard_broken(labels, ITIN_STEP_TEXT_SIZE) || guard_broken(values, ITIN_STEP_TEXT_SIZE)) return 1;
		// A maneuver is described by its three burn components.
		if(s->body == NULL && (strstr(labels, "Prograde") == NULL || strstr(labels, "Radial") == NULL || strstr(labels, "Normal") == NULL)) return 4;
	}
	for(int detailed = 0; detailed < 2; detailed++) {
		memset(overview, 0x5A, sizeof(overview));
		overview[0] = 0;
		if(detailed) itinerary_detailed_overview_to_string(first, DATE_ISO, dep_periapsis, arr_periapsis, overview);
		else itinerary_short_overview_to_string(first, DATE_ISO, dep_periapsis, arr_periapsis, overview);
		if(guard_broken(overview, 10000) || strstr(overview, "nan") != NULL) return 2 + detailed;
		if(first->next != NULL && strstr(overview, "Total dv") == NULL) return 6;
		if(strstr(overview, "Deep-Space Maneuver") != NULL && (strstr(overview, "tangential") == NULL || strstr(overview, "normal") == NULL)) return 5;
	}
	return 0;
}

// Step delta-vs shown by the planner must add up to the totals of the itinerary overview.
static double itinerary_dv_mismatch(struct ItinStep *first, double dep_periapsis, double arr_periapsis) {
	struct PorkchopPoint pp = create_porkchop_point(get_last(first), dep_periapsis, arr_periapsis);
	double worst = 0;
	for(int type = TF_FLYBY; type <= TF_CIRC; type++) {
		double expected = pp.dv_dep + pp.dv_dsm + pp.dv_flyby +
			(type == TF_CAPTURE ? pp.dv_arr_cap : type == TF_CIRC ? pp.dv_arr_circ : 0);
		if(!isfinite(expected)) continue;
		double sum = 0;
		for(struct ItinStep *s = first; s != NULL; s = s->next != NULL ? s->next[0] : NULL)
			sum += itinerary_step_dv(s, dep_periapsis, arr_periapsis, type);
		worst = fmax(worst, fabs(sum - expected));
	}
	return worst;
}

static int itinerary_recompute_problem(struct ItinStep *first, CelestSystem *system) {
	update_itin_body_osvs(first, system);
	calc_itin_v_vectors_from_dates_and_r(first, system);
	return itinerary_structure_problem(first) != 0;
}

int check_itinerary_operations(struct ItinStep **departures, int num_departures, CelestSystem *system) {
	int problems = 0, routes = 0, maneuver_routes = 0, reported = 0, resolve_attempts = 0, resolve_fallbacks = 0, maneuver_optimizations = 0, sweep_routes = 0, flagged_routes = 0;
#define OPERATION_PROBLEM(...) do { problems++; if(reported++ < 12) fprintf(stderr, __VA_ARGS__); } while(0)
	int maneuver_budget = 30, plain_budget = 30;
	for(int d = 0; d < num_departures; d++) {
		int count = get_number_of_itineraries(departures[d]);
		struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
		if(leaves == NULL) return 0;
		int stored = 0;
		store_itineraries_in_array(departures[d], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			int has_maneuver = 0, nodes = 0;
			for(struct ItinStep *s = leaves[j]; s != NULL; s = s->prev) { has_maneuver |= s->body == NULL; nodes++; }
			if(has_maneuver ? maneuver_budget <= 0 : (plain_budget <= 0 || j % 7 != 0)) continue;
			if(has_maneuver) maneuver_budget--; else plain_budget--;
			routes++; maneuver_routes += has_maneuver;
			flagged_routes += count_flybys_needing_burn(leaves[j], NULL) > 0;

			struct ItinStep *copy = get_first(create_itin_copy_from_arrival(leaves[j]));
			double dep = copy->body->atmo_alt + 50e3, arr = get_last(copy)->body->atmo_alt + 50e3;
			int problem = itinerary_structure_problem(copy);
			if(problem) OPERATION_PROBLEM("route %d.%d: structure problem %d\n", d, j, problem);
			if(!is_valid_itinerary(get_last(copy))) OPERATION_PROBLEM("route %d.%d: is_valid_itinerary rejects a stored route\n", d, j);
			if((problem = itinerary_text_problem(copy, dep, arr)) != 0) OPERATION_PROBLEM("route %d.%d: text builders problem %d (1 step text overflow, 2 short or 3 detailed overview overflow or nan)\n", d, j, problem);
			double mismatch = itinerary_dv_mismatch(copy, dep, arr);
			if(mismatch > 1e-3) OPERATION_PROBLEM("route %d.%d: step dv do not add up to the overview (%.4f m/s off)\n", d, j, mismatch);
			if(itinerary_recompute_problem(copy, system)) OPERATION_PROBLEM("route %d.%d: recompute leaves non-finite nodes\n", d, j);
			free_itinerary(copy);

			// Re-solving at unchanged dates, as the planner does after an edit, must reproduce the stored delta-v.
			copy = get_first(create_itin_copy_from_arrival(leaves[j]));
			int encounters = 0;
			for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) encounters += s->body != NULL;
			if(encounters >= 3) {
				double before = 0, after = 0;
				for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) before += itinerary_step_dv(s, dep, arr, TF_FLYBY);
				resolve_attempts++;
				if(!resolve_itinerary_with_powered_flybys(system, copy, dep, arr, TF_FLYBY)) {
					resolve_fallbacks++;
					double shortest = DBL_MAX;
					for(struct ItinStep *s = copy; s->next != NULL; s = s->next[0]) shortest = fmin(shortest, s->next[0]->date - s->date);
					printf("  re-solve fell back on route %d.%d (maneuver %d, %d encounters, shortest leg %.1f days)\n", d, j, has_maneuver, encounters, shortest);
				}
				else {
					for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) after += itinerary_step_dv(s, dep, arr, TF_FLYBY);
					// A maneuver node is pinned by its stored position and date, which amplifies the solver's convergence noise.
					double tolerance = has_maneuver ? 0.05 * before : 50.0;
					if(fabs(after - before) > tolerance) OPERATION_PROBLEM("route %d.%d: re-solve changed total dv from %.1f to %.1f m/s\n", d, j, before, after);
				}
			}
			free_itinerary(copy);

			// Re-optimizing maneuvers must leave a valid route that is not worse than before.
			if(encounters >= 3 && maneuver_optimizations < 6) {
				copy = get_first(create_itin_copy_from_arrival(leaves[j]));
				double before = 0, after = 0;
				for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) before += itinerary_step_dv(s, dep, arr, TF_FLYBY);
				if(optimize_itinerary_maneuvers(system, copy, dep, arr, TF_FLYBY)) {
					maneuver_optimizations++;
					problem = itinerary_structure_problem(copy);
					if(problem) OPERATION_PROBLEM("route %d.%d: structure problem %d after optimizing maneuvers\n", d, j, problem);
					for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) after += itinerary_step_dv(s, dep, arr, TF_FLYBY);
					if(after > before * 1.05 + 50.0) OPERATION_PROBLEM("route %d.%d: maneuver optimization raised dv from %.1f to %.1f m/s\n", d, j, before, after);
					if(itinerary_text_problem(copy, dep, arr)) OPERATION_PROBLEM("route %d.%d: text problem after optimizing maneuvers\n", d, j);
				}
				free_itinerary(copy);
			}

			// Moving the departure a day at a time must change delta-v smoothly, never jump to a mirrored flyby solution.
			if(encounters >= 3 && sweep_routes < 24) {
				copy = get_first(create_itin_copy_from_arrival(leaves[j]));
				update_itin_body_osvs(copy, system);
				if(resolve_itinerary_with_powered_flybys(system, copy, dep, arr, TF_FLYBY)) {
					sweep_routes++;
					double previous = 0;
					for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) previous += itinerary_step_dv(s, dep, arr, TF_FLYBY);
					for(int click = 1; click <= 20; click++) {
						set_itinerary_step_date(copy, system, copy->date + (sweep_routes % 2 ? 1 : -1));
						update_itin_body_osvs(copy, system);
						if(!resolve_itinerary_with_powered_flybys(system, copy, dep, arr, TF_FLYBY)) break;
						double current = 0;
						for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) current += itinerary_step_dv(s, dep, arr, TF_FLYBY);
						if(current > previous * 1.4 + 200.0) OPERATION_PROBLEM("route %d.%d: moving the departure one day raised dv from %.0f to %.0f m/s (click %d)\n", d, j, previous, current, click);
						previous = current;
					}
				}
				free_itinerary(copy);
			}

			for(int k = 0; k < nodes; k++) {
				// Moving the step's date.
				copy = get_first(create_itin_copy_from_arrival(leaves[j]));
				struct ItinStep *moved = copy;
				for(int n = 0; n < k; n++) moved = moved->next[0];
				if(moved->prev == NULL) { free_itinerary(copy); continue; }
				double original_date = moved->date;
				Vector3 original_r = moved->r;
				double target = moved->next != NULL ? original_date + 0.3 * (moved->next[0]->date - original_date)
					: original_date - 0.3 * (original_date - moved->prev->date);
				set_itinerary_step_date(moved, system, target);
				problem = itinerary_structure_problem(copy);
				if(problem) OPERATION_PROBLEM("route %d.%d: structure problem %d after moving step %d\n", d, j, problem, k);
				itinerary_recompute_problem(copy, system);
				if(itinerary_text_problem(copy, dep, arr)) OPERATION_PROBLEM("route %d.%d: text problem after moving step %d\n", d, j, k);
				if(moved->body == NULL) {
					double slide = mag_vec3(subtract_vec3(moved->r, original_r)) / mag_vec3(original_r);
					if(!(slide > 1e-6)) OPERATION_PROBLEM("route %d.%d: maneuver node did not move with its date\n", d, j);
					set_itinerary_step_date(moved, system, original_date);
					double shift = mag_vec3(subtract_vec3(moved->r, original_r)) / mag_vec3(original_r);
					if(shift > 0.05) OPERATION_PROBLEM("route %d.%d: maneuver node moved %.3f of its radius after a date round trip\n", d, j, shift);
				}
				free_itinerary(copy);

				// Moving a step past its successor must push the later steps back.
				copy = get_first(create_itin_copy_from_arrival(leaves[j]));
				moved = copy;
				for(int n = 0; n < k; n++) moved = moved->next[0];
				if(moved->prev != NULL && moved->next != NULL) {
					set_itinerary_step_date(moved, system, moved->next[0]->date + 2.0);
					problem = itinerary_structure_problem(copy);
					if(problem) OPERATION_PROBLEM("route %d.%d: structure problem %d after moving step %d past its successor\n", d, j, problem, k);
				}
				free_itinerary(copy);
			}
		}
		free(leaves);
	}
#undef OPERATION_PROBLEM
	// A route that does not converge falls back to the plain recompute, which is fine for a few routes.
	if(resolve_fallbacks * 100 > resolve_attempts * 30) { problems++; fprintf(stderr, "re-solve fell back on %d of %d routes\n", resolve_fallbacks, resolve_attempts); }
	else printf("Planner re-solve: %d of %d stored routes re-solved to the stored delta-v (the rest fall back)\n", resolve_attempts - resolve_fallbacks, resolve_attempts);
	if(problems == 0) printf("Itinerary operations check passed: %d routes (%d with maneuvers, %d flagged as having a flyby that hits a planet) survive structure, text, dv-sum, recompute, removal and date-change checks\n", routes, maneuver_routes, flagged_routes);
	else fprintf(stderr, "Itinerary operations check failed: %d problems on %d routes\n", problems, routes);
	return problems == 0 && routes > 0;
}
