#include "integration_common.h"

// A configured starting orbit must govern the departure delta-v that the search stores and filters on.
int check_starting_orbit_search(CelestSystem *system, Body *departure_body, Body *flyby_body,
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

	// A tilted circular orbit can never beat the free-plane burn at the same radius, and must lose on
	// routes whose asymptote is off its line of nodes.
	double radius = alt2radius(departure_body, calc_data.dv_filter.dep_periapsis);
	StartingOrbit tilted = {.semi_major_axis = radius, .inclination = 1.4, .raan = 0.5};
	int valid = starting_orbit_is_valid(&tilted, departure_body);
	starting_orbit_set_active(&tilted, departure_body);
	Itin_Calc_Results run = search_for_itineraries(calc_data);

	double worst_mismatch = 0, lowest_margin = DBL_MAX, largest_penalty = 0;
	int checked = 0, penalized = 0;
	for(int i = 0; i < run.num_deps && valid; i++) {
		int count = get_number_of_itineraries(run.departures[i]);
		struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
		if(leaves == NULL) { valid = 0; break; }
		int stored = 0;
		store_itineraries_in_array(run.departures[i], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			struct PorkchopPoint point = create_porkchop_point(leaves[j],
				calc_data.dv_filter.dep_periapsis, calc_data.dv_filter.arr_periapsis);
			if(!isfinite(point.dv_dep)) continue;
			// Independent recomputation from the first leg's departure velocity.
			struct ItinStep *first = leaves[j];
			while(first->prev != NULL && first->prev->prev != NULL) first = first->prev;
			Vector3 v_inf = subtract_vec3(first->v_dep, first->prev->v_body);
			double direct;
			if(!starting_orbit_best_departure_burn(&tilted, departure_body, v_inf, &direct, NULL)) continue;
			double free_plane = dv_circ(departure_body, radius, mag_vec3(v_inf));
			worst_mismatch = fmax(worst_mismatch, fabs(direct - point.dv_dep));
			lowest_margin = fmin(lowest_margin, point.dv_dep - free_plane);
			largest_penalty = fmax(largest_penalty, point.dv_dep - free_plane);
			penalized += point.dv_dep > free_plane + 10;
			checked++;
		}
		free(leaves);
	}
	starting_orbit_set_active(NULL, NULL);
	for(int i = 0; i < run.num_deps; i++) free_itinerary(run.departures[i]);
	free(run.departures);
	if(!valid || checked == 0 || worst_mismatch > 1e-6 || lowest_margin < -1e-3 || penalized == 0) {
		fprintf(stderr, "Starting orbit search check failed: %d routes checked, mismatch %.9f m/s, lowest margin over free plane %.6f m/s, %d routes penalized\n",
			checked, worst_mismatch, lowest_margin, penalized);
		return 0;
	}
	printf("Starting orbit search check passed: stored departure dV matches the orbit on %d routes (worst mismatch %.9f m/s); never below the free-plane burn, and above it by up to %.1f m/s on %d routes\n",
		checked, worst_mismatch, largest_penalty, penalized);
	return 1;
}
