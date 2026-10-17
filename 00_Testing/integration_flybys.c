#include "integration_common.h"

int run_dsm_comparison(CelestSystem *system, Body *departure_body, Body *flyby_body,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {flyby_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 2
	};
	calc_data.allow_dsm = 0;
	Itin_Calc_Results without_dsm = search_for_itineraries(calc_data);
	calc_data.allow_dsm = 1;
	Itin_Calc_Results with_dsm = search_for_itineraries(calc_data);

	int valid = 0;
	double best_without = DBL_MAX, best_with = DBL_MAX;
	int maneuver_routes = 0, bad_routes = 0;
	Itin_Calc_Results *runs[] = {&without_dsm, &with_dsm};
	for(int run = 0; run < 2; run++) {
		for(int i = 0; i < runs[run]->num_deps; i++) {
			int count = get_number_of_itineraries(runs[run]->departures[i]);
			struct ItinStep **leaves = malloc((size_t)count * sizeof(*leaves));
			if(leaves == NULL) goto done;
			int stored = 0;
			store_itineraries_in_array(runs[run]->departures[i], leaves, &stored);
			for(int j = 0; j < stored; j++) {
				double dsm_dv;
				double total = stored_total_dv(leaves[j], &calc_data, &dsm_dv);
				if(!isfinite(total)) { bad_routes++; continue; }
				int body_nodes = 0;
				for(struct ItinStep *step = leaves[j]; step != NULL; step = step->prev)
					body_nodes += step->body != NULL;
				// Direct routes have no flyby, so a DSM cannot change them.
				if(body_nodes < 3) continue;
				if(run == 0 && total < best_without) best_without = total;
				if(run == 1 && total < best_with) best_with = total;
				if(run == 1 && dsm_dv > 0) {
					maneuver_routes++;
					double plain = plain_cost_without_maneuvers(leaves[j], &calc_data);
					// A kept maneuver must beat the same route without it by more than the noise floor.
					if(!(plain < DBL_MAX) || total > plain - 1.0 || dsm_dv < 1.0) bad_routes++;
				}
			}
			free(leaves);
		}
	}
	printf("DSM comparison (Earth-Venus-Jupiter flyby routes, one departure): best total dV %.3f m/s without DSM, %.3f m/s with DSM (saving %.3f); %d routes carry a maneuver; optimizer improved %d of %d routes, best saving %.3f m/s\n",
		best_without, best_with, best_without - best_with, maneuver_routes,
		with_dsm.powered_dsm_routes_improved, with_dsm.powered_dsm_routes_optimized,
		with_dsm.powered_best_dsm_saving);
	valid = best_without < DBL_MAX && best_with < DBL_MAX && bad_routes == 0 &&
		best_with <= best_without + 1.0 && best_without - best_with >= 50.0 &&
		maneuver_routes > 0 && with_dsm.powered_dsm_routes_improved > 0;
	if(!valid)
		fprintf(stderr, "DSM comparison failed: %d inconsistent routes, %d with maneuvers, saving %.3f m/s\n",
			bad_routes, maneuver_routes, best_without - best_with);

	if(valid) {
		// Saving and reloading must keep maneuver nodes (stored with body id -1) and their delta-v.
		char file_path[] = "dsm_roundtrip.itins";
		struct ItinStep **saved = with_dsm.departures;
		int saved_routes = 0, saved_maneuvers = 0;
		double saved_best = DBL_MAX;
		summarize_itineraries(saved, with_dsm.num_deps, &calc_data, &saved_routes, &saved_maneuvers, &saved_best);
		store_itineraries_in_bfile(saved, with_dsm.num_nodes, with_dsm.num_deps, with_dsm.num_itins,
			calc_data, system, file_path, get_current_bin_file_type());
		struct ItinsLoadFileResults loaded = load_itineraries_from_bfile(file_path);
		int loaded_routes = 0, loaded_maneuvers = 0;
		double loaded_best = DBL_MAX;
		if(loaded.departures != NULL)
			summarize_itineraries(loaded.departures, loaded.header.num_deps, &loaded.header.calc_data,
				&loaded_routes, &loaded_maneuvers, &loaded_best);
		if(loaded.departures == NULL || saved_routes != loaded_routes ||
			saved_maneuvers != loaded_maneuvers || saved_maneuvers == 0 ||
			fabs(saved_best - loaded_best) > 0.01) {
			fprintf(stderr, "DSM file round trip failed: %d/%d routes, %d/%d maneuver nodes, best %.3f/%.3f m/s\n",
				loaded_routes, saved_routes, loaded_maneuvers, saved_maneuvers, loaded_best, saved_best);
			valid = 0;
		} else {
			if(!check_itinerary_operations(loaded.departures, loaded.header.num_deps, loaded.header.system) ||
				!check_itinerary_operations(with_dsm.departures, with_dsm.num_deps, system))
				valid = 0;
			printf("DSM file round trip passed: %d routes, %d maneuver nodes, best %.3f m/s\n",
				loaded_routes, loaded_maneuvers, loaded_best);
		}
		for(int i = 0; loaded.departures != NULL && i < loaded.header.num_deps; i++)
			free_itinerary(loaded.departures[i]);
		free(loaded.departures);
		if(loaded.header.system != NULL) free_celestial_system(loaded.header.system);
		remove(file_path);
	}

done:
	for(int run = 0; run < 2; run++) {
		for(int i = 0; i < runs[run]->num_deps; i++) free_itinerary(runs[run]->departures[i]);
		free(runs[run]->departures);
	}
	return valid;
}

// Model-level DSM checks on routes with fixed dates, independent of the search.
int check_dsm_model(CelestSystem *system, Body *departure_body, Body *flyby_body,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {flyby_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 2
	};
	Body *bodies[] = {departure_body, flyby_body, arrival_body};
	// Flyby and arrival offsets in days; the first pair is a route where a DSM clearly helps.
	double pairs[][2] = {{66, 1414}, {218, 1414}, {238, 1414}, {120, 900}, {200, 1100}, {150, 1300}};
	int valid = 1;
	double first_saving = 0, first_maneuver = 0;
	PoweredLegDsm first_found[2] = {{0}};
	for(size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
		double times[] = {departure_jd, departure_jd + pairs[p][0], departure_jd + pairs[p][1]};
		double plain = evaluate_powered_flyby_route_objective(&calc_data, bodies, times, 3);
		PoweredLegDsm zero[2] = {{.enabled = 1, .eta = 0.5}, {.enabled = 1, .eta = 0.3}};
		double with_zero = evaluate_powered_flyby_route_objective_with_dsm(&calc_data, bodies, times, 3, zero);
		PoweredLegDsm found[2];
		double best = optimize_powered_flyby_route_dsm(&calc_data, bodies, times, 3, found);
		double recheck = evaluate_powered_flyby_route_objective_with_dsm(&calc_data, bodies, times, 3, found);
		if(!(plain < DBL_MAX) || !(with_zero < DBL_MAX) || !(best < DBL_MAX)) {
			fprintf(stderr, "DSM model: route +%.0f/+%.0f was not evaluable\n", pairs[p][0], pairs[p][1]);
			valid = 0;
			continue;
		}
		// A zero maneuver is the plain route; the gap is the evaluator's convergence noise.
		if(fabs(with_zero - plain) > 10.0) {
			fprintf(stderr, "DSM model: zero-delta-v DSM changed the cost by %.3f m/s on route +%.0f/+%.0f\n",
				with_zero - plain, pairs[p][0], pairs[p][1]);
			valid = 0;
		}
		if(best > plain + 1e-6 || fabs(recheck - best) > 0.01) {
			fprintf(stderr, "DSM model: optimizer returned %.3f (recheck %.3f) against plain %.3f on route +%.0f/+%.0f\n",
				best, recheck, plain, pairs[p][0], pairs[p][1]);
			valid = 0;
		}
		if(p == 0) {
			first_saving = plain - best;
			memcpy(first_found, found, sizeof(first_found));
			for(int leg = 0; leg < 2; leg++)
				if(found[leg].enabled) first_maneuver = fmax(first_maneuver, mag_vec3(found[leg].delta_v));
		}
	}
	if(first_saving < 100.0 || first_maneuver < POWERED_TEST_DV_TOLERANCE) {
		fprintf(stderr, "DSM model: expected a saving above 100 m/s on the first route, got %.3f m/s with a %.1f m/s maneuver\n",
			first_saving, first_maneuver);
		valid = 0;
	}

	// A DSM counts toward the satellite delta-v limit, so a tight limit must reject it.
	double times[] = {departure_jd, departure_jd + pairs[0][0], departure_jd + pairs[0][1]};
	calc_data.dv_filter.max_satdv = first_maneuver / 4;
	double limited = evaluate_powered_flyby_route_objective_with_dsm(&calc_data, bodies, times, 3, first_found);
	calc_data.dv_filter.max_satdv = 1e9;
	double unlimited = evaluate_powered_flyby_route_objective_with_dsm(&calc_data, bodies, times, 3, first_found);
	if(limited < DBL_MAX || !(unlimited < DBL_MAX)) {
		fprintf(stderr, "DSM model: satellite delta-v limit did not govern the maneuver (limited %g, unlimited %g)\n",
			limited, unlimited);
		valid = 0;
	}
	if(valid)
		printf("DSM model checks passed: a DSM saves %.1f m/s on the first route (maneuver %.1f m/s), zero maneuver matches the plain route, satellite limit enforced\n",
			first_saving, first_maneuver);
	return valid;
}

// Flybys with nearly equal in and out speeds must stay feasible whichever speed is larger.
// Routes that return to the same body after revolutions must be stored consistently and re-solve to the stored delta-v.
int check_resonant_flybys(CelestSystem *system, Body *departure_body, Body *flyby_body,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {departure_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 2, .allow_resonant = 1
	};
	calc_data.allow_dsm = 1;
	Itin_Calc_Results results = search_for_itineraries(calc_data);

	int resonant_routes = 0, bad_routes = 0, resolve_failures = 0, reported = 0, scored_routes = 0, unscorable_routes = 0, maneuver_routes = 0;
	double best = DBL_MAX, worst_recompute = 0, worst_resolve = 0;
	struct ItinStep *best_leaf = NULL;
	// Only the cheapest routes are corrected by the optimizer, so the second pass checks those.
	for(int pass = 0; pass < 2; pass++)
	for(int i = 0; i < results.num_deps; i++) {
		int count = get_number_of_itineraries(results.departures[i]);
		struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
		if(leaves == NULL) { bad_routes++; continue; }
		int stored = 0;
		store_itineraries_in_array(results.departures[i], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			int resonant = route_is_resonant(leaves[j]);
			if(!resonant) continue;
			double total = stored_total_dv(leaves[j], &calc_data, NULL);
			if(pass == 0) {
				resonant_routes++;
				if(isfinite(total) && total < best) { best = total; best_leaf = leaves[j]; }
				continue;
			}
			if(!isfinite(total)) { bad_routes++; continue; }
			if(total > best * 1.1 + 100) continue;
			double recomputed = plain_cost_without_maneuvers(leaves[j], &calc_data);
			int has_maneuver = 0;
			for(struct ItinStep *step = leaves[j]; step != NULL; step = step->prev) has_maneuver |= step->body == NULL;
			maneuver_routes += has_maneuver;
			// Routes the optimizer could not correct stay as raw search nodes and cannot be scored from dates.
			if(!(recomputed < DBL_MAX)) { unscorable_routes++; continue; }
			scored_routes++;
			double recompute_gap = has_maneuver ? 0 : fabs(recomputed - total);
			worst_recompute = fmax(worst_recompute, recompute_gap);
			if(recompute_gap > POWERED_TEST_DV_TOLERANCE) {
				bad_routes++;
				if(reported++ < 5) fprintf(stderr, "Resonant route stored %.3f but recomputes to %.3f m/s\n", total, recomputed);
			}
			// The planner re-solves from dates alone, so an unchanged route must reproduce its stored cost.
			struct ItinStep *copy = get_first(create_itin_copy_from_arrival(leaves[j]));
			double dep = copy->body->atmo_alt + 50e3, arr = get_last(copy)->body->atmo_alt + 50e3;
			update_itin_body_osvs(copy, system);
			int converged = 0;
			int resolved = resolve_itinerary_with_powered_flybys_approximate(system, copy, dep, arr, TF_FLYBY, &converged);
			double after = 0;
			for(struct ItinStep *s = copy; s != NULL; s = s->next != NULL ? s->next[0] : NULL) after += itinerary_step_dv(s, dep, arr, TF_FLYBY);
			double resolve_gap = resolved && converged ? fabs(after - total) : DBL_MAX;
			if(resolve_gap < DBL_MAX) worst_resolve = fmax(worst_resolve, resolve_gap);
			if(!(after <= total + 0.5 + 1e-4 * total) || !resolved || !converged) {
				resolve_failures++;
				if(reported++ < 5) fprintf(stderr, "Resonant route stored %.3f re-solves to %.3f m/s (resolved %d, converged %d)\n", total, after, resolved, converged);
			}
			free_itinerary(copy);
		}
		free(leaves);
	}
	// A maneuver inside the revolution-spanning leg must be optimizable and never make the route worse.
	int dsm_probe_valid = 1, probe_maneuvers = 0;
	double probe_plain = DBL_MAX, probe_best = DBL_MAX;
	if(best_leaf != NULL) {
		Body *probe_bodies[16];
		double probe_times[16];
		size_t probe_steps = 0;
		for(struct ItinStep *step = best_leaf; step != NULL && probe_steps < 16; step = step->prev)
			if(step->body != NULL) { probe_bodies[probe_steps] = step->body; probe_times[probe_steps++] = step->date; }
		for(size_t a = 0, b = probe_steps - 1; a < b; a++, b--) {
			Body *tb = probe_bodies[a]; probe_bodies[a] = probe_bodies[b]; probe_bodies[b] = tb;
			double tt = probe_times[a]; probe_times[a] = probe_times[b]; probe_times[b] = tt;
		}
		PoweredLegDsm probe_dsm[16];
		probe_plain = evaluate_powered_flyby_route_objective(&calc_data, probe_bodies, probe_times, probe_steps);
		probe_best = optimize_powered_flyby_route_dsm(&calc_data, probe_bodies, probe_times, probe_steps, probe_dsm);
		for(size_t leg = 0; leg + 1 < probe_steps; leg++) probe_maneuvers += probe_dsm[leg].enabled;
		dsm_probe_valid = probe_plain < DBL_MAX && probe_best <= probe_plain + POWERED_TEST_DV_TOLERANCE;
		printf("Resonant DSM probe: %.3f m/s without, %.3f m/s optimized, %d maneuver(s)\n", probe_plain, probe_best, probe_maneuvers);
	}
	int valid = dsm_probe_valid && scored_routes > 0 && bad_routes == 0 && resolve_failures == 0;
	if(valid)
		printf("Resonant flyby check passed: %d resonant routes (%d scored, %d unscorable, %d with a maneuver), best %.3f m/s, worst recompute gap %.4f, worst re-solve gap %.4f m/s\n",
			resonant_routes, scored_routes, unscorable_routes, maneuver_routes, best, worst_recompute, worst_resolve);
	else
		fprintf(stderr, "Resonant flyby check failed: %d resonant routes, %d scored, %d inconsistent, %d did not re-solve\n",
			resonant_routes, scored_routes, bad_routes, resolve_failures);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return valid;
}

int check_near_equal_speed_flybys(Body *body) {
	static const double speed_offsets[] = {-0.9, -0.5, 0.5, 0.9};
	int valid = 1;
	double minimum_radius = body->radius + body->atmo_alt + 1e4;
	Vector3 incoming = vec3(6000, 0, 0);
	for(int i = 0; i < 4; i++) {
		double speed = 6000 + speed_offsets[i];
		Vector3 outgoing = vec3(speed * cos(M_PI / 3), speed * sin(M_PI / 3), 0);
		PoweredFlybyResult result;
		int feasible = calculate_powered_flyby(incoming, outgoing, vec3(0, 0, 0), body, minimum_radius, &result);
		if(!feasible || !isfinite(result.burn_delta_v) || result.burn_delta_v > 5.0) {
			fprintf(stderr, "Near-equal-speed flyby with a %+.1f m/s speed difference: feasible %d, burn %.3f m/s\n", speed_offsets[i], feasible, feasible ? result.burn_delta_v : -1.0);
			valid = 0;
		}
	}
	if(valid) printf("Near-equal-speed flyby check passed on %s\n", body->name);
	return valid;
}
