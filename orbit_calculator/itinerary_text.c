#include "itinerary_text.h"
#include "flyby_policy.h"
#include "tools/starting_orbit.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void store_step_in_file(struct ItinStep *step, FILE *file, int layer, int variation) {
	fprintf(file, "#%d#%d\n", layer, variation);
	fprintf(file, "Date: %f\n", step->date);
	fprintf(file, "r: %lf, %lf, %lf\n", step->r.x, step->r.y, step->r.z);
	fprintf(file, "v_dep: %f, %f, %f\n", step->v_dep.x, step->v_dep.y, step->v_dep.z);
	fprintf(file, "v_arr: %f, %f, %f\n", step->v_arr.x, step->v_arr.y, step->v_arr.z);
	fprintf(file, "v_body: %f, %f, %f\n", step->v_body.x, step->v_body.y, step->v_body.z);
	fprintf(file, "Next Steps: %d\n", step->num_next_nodes);

	for(int i = 0; i < step->num_next_nodes; i++) {
		store_step_in_file(step->next[i], file, layer + 1, i);
	}
}

void store_itineraries_in_file(struct ItinStep **departures, int num_nodes, int num_deps) {
	char filename[50];  // 14 for date + 4 for .csv + 1 for string terminator
	sprintf(filename, "./Itineraries/test.transfer");
	int num_steps = get_num_of_itin_layers(departures[0]);
	printf("Filesize: ~%.3f MB\n", (double)num_nodes*240/1e6);

	FILE *file;
	file = fopen(filename,"w");

	fprintf(file, "Number of stored nodes: %d\n", num_nodes);
	fprintf(file, "Number of Departures: %d, Number of Steps: %d\n", num_deps, num_steps);

	struct ItinStep *ptr = departures[0];

	fprintf(file, "Bodies: ");
	while(ptr != NULL) {
		if(ptr->body != NULL) fprintf(file, "%d", ptr->body->id);
		else fprintf(file, "-");
		if(ptr->next != NULL) {
			fprintf(file, ",");
			ptr = ptr->next[0];
		} else {
			fprintf(file, "#\n");
			break;
		}
	}

	for(int i = 0; i < num_deps; i++) {
		store_step_in_file(departures[i], file, 0, i);
	}

	fclose(file);
}

// Heliocentric radial (outward), tangential (prograde) and normal components of a maneuver node's burn.
static Vector3 dsm_burn_rtn(const struct ItinStep *step) {
	if(step->next == NULL || step->prev == NULL) return vec3(0, 0, 0);
	Vector3 burn = subtract_vec3(step->next[0]->v_dep, step->v_arr);
	Vector3 radial = norm_vec3(step->r);
	Vector3 normal = norm_vec3(cross_vec3(step->r, step->v_arr));
	Vector3 tangential = cross_vec3(normal, radial);
	return vec3(dot_vec3(burn, radial), dot_vec3(burn, tangential), dot_vec3(burn, normal));
}

void itinerary_short_overview_to_string(struct ItinStep *step, enum DateType date_type, double dep_periapsis, double arr_periapsis, char *string) {
	sprintf(string, "");
	if(step == NULL) return;
	step = get_first(step);
	if(step->next == NULL) return;
	char date_string[32];
	while(step != NULL) {
		if(step->prev != NULL) sprintf(string, "%s - ", string);
		sprintf(string, "%s%s", string, step->body != NULL ? step->body->name : "DSM");
		if(step->next != NULL) step = step->next[0];
		else break;
	}
	
	date_to_string(convert_JD_date(get_first(step)->date, date_type), date_string, 1);
	sprintf(string, "%s\nDeparture: %s\n", string, date_string);
	date_to_string(convert_JD_date(get_last(step)->date, date_type), date_string, 1);
	double dt = get_last(step)->date-get_first(step)->date;
	if(date_type == DATE_KERBAL) dt *= 4;
	sprintf(string, "%sArrival: %s\nDuration: %.2f days\n", string, date_string, dt);
	
	struct PorkchopPoint pp = create_porkchop_point(get_last(step), dep_periapsis, arr_periapsis);
	
	sprintf(string, "%s\nDeparture dv: %.2f m/s (Periapsis: %.2fkm)\n", string, pp.dv_dep, dep_periapsis/1e3);
	sprintf(string, "%sArrival dv: %.2f m/s  (Capture; Periapsis: %.2fkm)\n", string, pp.dv_arr_cap, arr_periapsis/1e3);
	sprintf(string, "%sArrival dv: %.2f m/s  (Circularization; Periapsis: %.2fkm)\n", string, pp.dv_arr_circ, arr_periapsis/1e3);
	sprintf(string, "%sPowered flyby burns: %.2f m/s\n", string, pp.dv_flyby);
	sprintf(string, "%sDSM burns: %.2f m/s\n", string, pp.dv_dsm);
	sprintf(string, "%sTotal dv: %.2f m/s  (Fly-By)\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby);
	sprintf(string, "%sTotal dv: %.2f m/s  (Capture)\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_cap);
	sprintf(string, "%sTotal dv: %.2f m/s  (Circularization)\n\n--\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_circ);
	
	step = get_first(step);
	int step_id = 0;
	
	while(step != NULL) {
		date_to_string(convert_JD_date(step->date, date_type), date_string, 1);
		if(step->body == NULL) {
			double maneuver_dv = step->next != NULL ? mag_vec3(subtract_vec3(step->next[0]->v_dep, step->v_arr)) : 0;
			Vector3 rtn = dsm_burn_rtn(step);
			sprintf(string, "%sDeep-Space Maneuver: | %s | Burn: %.2f m/s (radial %.2f, tangential %.2f, normal %.2f)\n", string, date_string, maneuver_dv, rtn.x, rtn.y, rtn.z);
			step = step->next != NULL ? step->next[0] : NULL;
			continue;
		}
		if(step_id == 0) sprintf(string, "%sDeparture", string);
		else if(step->next == NULL) sprintf(string, "%sArrival", string);
		else sprintf(string, "%sFly-By %d", string, step_id);
		sprintf(string, "%s (%s): | %s", string, step->body->name, date_string);
		if(step_id == 0) {
//			sprintf(string, "%sPeriapsis: %.2fkm\n", string, dep_periapsis/1e3);
			sprintf(string, "%s\n", string);
		} else if(step->next == NULL) {
//			sprintf(string, "%sPeriapsis: %.2fkm\n", string, arr_periapsis/1e3);
			sprintf(string, "%s\n", string);
		} else {
			Vector3 v_arr = step->v_arr;
			Vector3 v_dep = step->next[0]->v_dep;
			Vector3 v_body = step->v_body;
			PoweredFlybyResult flyby;
			double min_periapsis = flyby_minimum_periapsis(step->body);
			int valid_flyby = calculate_flyby_with_moons(v_arr, v_dep, v_body, step->body, min_periapsis, step->date, &flyby);
			double periapsis = valid_flyby ? flyby.periapsis_radius : get_flyby_periapsis(v_arr, v_dep, v_body, step->body);
			double burn = valid_flyby ? flyby.burn_delta_v : 0;
			sprintf(string, "%s | Periapsis: %.2fkm | Burn: %.2f m/s\n", string,
				(periapsis-step->body->radius)/1e3, burn);
		}
		if(step->next != NULL) step = step->next[0];
		else step = NULL;
		step_id++;
	}
}

void itinerary_detailed_overview_to_string(struct ItinStep *step, enum DateType date_type, double dep_periapsis, double arr_periapsis, char *string) {
	sprintf(string, "");
	if(step == NULL) return;
	step = get_first(step);
	if(step->next == NULL) return;
	char date_string[32];
	while(step != NULL) {
		if(step->prev != NULL) sprintf(string, "%s - ", string);
		sprintf(string, "%s%s", string, step->body != NULL ? step->body->name : "DSM");
		if(step->next != NULL) step = step->next[0];
		else break;
	}
	
	date_to_string(convert_JD_date(get_first(step)->date, date_type), date_string, 1);
	sprintf(string, "%s\nDeparture: %s\n", string, date_string);
	date_to_string(convert_JD_date(get_last(step)->date, date_type), date_string, 1);
	double dt = get_last(step)->date-get_first(step)->date;
	if(date_type == DATE_KERBAL) dt *= 4;
	sprintf(string, "%sArrival: %s\nDuration: %.2f days\n", string, date_string, dt);
	
	struct PorkchopPoint pp = create_porkchop_point(get_last(step), dep_periapsis, arr_periapsis);
	
	sprintf(string, "%s\nDeparture dv: %.2f m/s\n", string, pp.dv_dep);
	struct ItinStep *departure = get_first(step);
	DepartureBurn3D departure_burn;
	Vector3 departure_vinf = subtract_vec3(departure->next[0]->v_dep, departure->v_body);
	if(calculate_free_plane_departure_burn(departure_vinf, departure->body,
		alt2radius(departure->body, dep_periapsis), &departure_burn)) {
		Vector3 radial = departure_burn.periapsis_direction;
		Vector3 tangential = departure_burn.tangential_direction;
		Vector3 normal = norm_vec3(cross_vec3(radial, tangential));
		double burn_radial = dot_vec3(departure_burn.burn_delta_v, radial);
		double burn_tangential = dot_vec3(departure_burn.burn_delta_v, tangential);
		double burn_normal = dot_vec3(departure_burn.burn_delta_v, normal);
		sprintf(string, "%sInitial burn: %.2f m/s (radial %.2f, tangential %.2f, normal %.2f) "
			"[starting-orbit RTN]\n", string, departure_burn.burn_magnitude,
			burn_radial, burn_tangential, burn_normal);
	}
	sprintf(string, "%sArrival dv: %.2f m/s  (Capture)\n", string, pp.dv_arr_cap);
	sprintf(string, "%sArrival dv: %.2f m/s  (Circularization)\n", string, pp.dv_arr_circ);
	sprintf(string, "%sPowered flyby burns: %.2f m/s\n", string, pp.dv_flyby);
	sprintf(string, "%sDSM burns: %.2f m/s\n", string, pp.dv_dsm);
	sprintf(string, "%sTotal dv: %.2f m/s  (Fly-By)\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby);
	sprintf(string, "%sTotal dv: %.2f m/s  (Capture)\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_cap);
	sprintf(string, "%sTotal dv: %.2f m/s  (Circularization)\n\n--\n", string, pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_circ);
	
	step = get_first(step);
	int step_id = 0;
	
	while(step != NULL) {
		date_to_string(convert_JD_date(step->date, date_type), date_string, 1);
		if(step->body == NULL) {
			double maneuver_dv = step->next != NULL ? mag_vec3(subtract_vec3(step->next[0]->v_dep, step->v_arr)) : 0;
			Vector3 rtn = dsm_burn_rtn(step);
			sprintf(string, "%s\nDeep-Space Maneuver:\nDate: %s  (T+%.2f days)\nBurn: %.2f m/s (radial %.2f, tangential %.2f, normal %.2f)\n", string,
				date_string, step->date-get_first(step)->date, maneuver_dv, rtn.x, rtn.y, rtn.z);
			step = step->next != NULL ? step->next[0] : NULL;
			continue;
		}
		if(step_id == 0) sprintf(string, "%s\nDeparture", string);
		else if(step->next == NULL) sprintf(string, "%s\nArrival", string);
		else sprintf(string, "%s\nFly-By %d", string, step_id);
		sprintf(string, "%s (%s):\nDate: %s  (T+%.2f days)\n", string, step->body->name, date_string, step->date-get_first(step)->date);
		if(step_id == 0) {
			sprintf(string, "%sPeriapsis: %.2fkm\n", string, dep_periapsis/1e3);
		} else if(step->next == NULL) {
			sprintf(string, "%sPeriapsis: %.2fkm\n", string, arr_periapsis/1e3);
		} else {
			Vector3 v_arr = step->v_arr;
			Vector3 v_dep = step->next[0]->v_dep;
			Vector3 v_body = step->v_body;
			double incl = get_flyby_inclination(v_arr, v_dep, v_body, get_body_equatorial_plane(step->body));
			PoweredFlybyResult flyby;
			double min_periapsis = flyby_minimum_periapsis(step->body);
			int valid_flyby = calculate_flyby_with_moons(v_arr, v_dep, v_body, step->body, min_periapsis, step->date, &flyby);
			double periapsis = valid_flyby ? flyby.periapsis_radius : get_flyby_periapsis(v_arr, v_dep, v_body, step->body);
			double burn = valid_flyby ? flyby.burn_delta_v : 0;
			Vector3 burn_normal = valid_flyby ?
				cross_vec3(flyby.periapsis_direction, flyby.incoming_tangent_direction) : vec3(0, 0, 0);
			double burn_radial = valid_flyby ? dot_vec3(flyby.burn_delta_v_vector, flyby.periapsis_direction) : 0;
			double burn_tangential = valid_flyby ? dot_vec3(flyby.burn_delta_v_vector, flyby.incoming_tangent_direction) : 0;
			double burn_normal_component = valid_flyby ? dot_vec3(flyby.burn_delta_v_vector, burn_normal) : 0;
			sprintf(string, "%sPeriapsis: %.2fkm\nInclination: %.2f°\n"
				"Powered burn: %.2f m/s (radial %.2f, tangential %.2f, normal %.2f)\n",
				string, (periapsis-step->body->radius)/1e3, rad2deg(incl), burn,
				burn_radial, burn_tangential, burn_normal_component);
		}
		if(step->next != NULL) step = step->next[0];
		else step = NULL;
		step_id++;
	}
}

void itinerary_step_parameters_to_string(char *s_labels, char *s_values, enum DateType date_type, double dep_periapsis, double arr_periapsis, struct ItinStep *step) {
	if(step == NULL || (step->prev == NULL && step->next == NULL)) {sprintf(s_labels,""); sprintf(s_values,""); return;}
	if(step->body == NULL) {
		double dt_in_days = step->date - get_first(step)->date;
		if(date_type == DATE_KERBAL) dt_in_days *= 4;
		Vector3 dv_vec = step->next != NULL ? subtract_vec3(step->next[0]->v_dep, step->v_arr) : vec3(0, 0, 0);
		// Components in the spacecraft frame: prograde, radial in, orbit normal.
		Vector3 prograde = norm_vec3(step->v_arr);
		Vector3 normal = norm_vec3(cross_vec3(step->r, step->v_arr));
		Vector3 radial_in = norm_vec3(cross_vec3(normal, prograde));
		sprintf(s_labels, "Deep-Space Maneuver\nT+:\nDistance to the Sun:\nDv Prograde:\nDv Radial:\nDv Normal:\nTotal:");
		sprintf(s_values, "\n%.2f days\n%.3f AU\n%.3f m/s\n%.3f m/s\n%.3f m/s\n%.3f m/s",
			dt_in_days, mag_vec3(step->r) / 1.495978707e11, dot_vec3(dv_vec, prograde),
			dot_vec3(dv_vec, radial_in), dot_vec3(dv_vec, normal), mag_vec3(dv_vec));
		return;
	}
	HyperbolaParams hyp_params;

	// is departure step
	if(step->prev == NULL) {
		hyp_params = get_hyperbola_params(vec3(0,0,0), step->next[0]->v_dep, step->v_body, step->body, dep_periapsis, HYP_DEPARTURE);
		sprintf(s_labels, "Departure\n"
						  "T+:\n"
						  "RadPer:\n"
						  "AltPer:\n"
						  "Min Incl:\n\n"
						  "C3Energy:\n"
						  "RHA (out):\n"
						  "DHA (out):");

		sprintf(s_values, "\n0 days\n"
						  "%.0f km\n"
						  "%.0f km\n"
						  "%.2f°\n\n"
						  "%.2f km²/s²\n"
						  "%.2f°\n"
						  "%.2f°",
				hyp_params.rp / 1000, (hyp_params.rp-step->body->radius) / 1000, rad2deg(angle_plane3_vec3(get_body_equatorial_plane(step->body), subtract_vec3(step->next[0]->v_dep, step->v_body))), hyp_params.c3_energy / 1e6,
				rad2deg(hyp_params.outgoing.bplane_angle), rad2deg(hyp_params.outgoing.decl));

	} else if(step->num_next_nodes == 0) {
		double dt_in_days = step->date - get_first(step)->date;
		if(date_type == DATE_KERBAL) dt_in_days *= 4;
		hyp_params = get_hyperbola_params(step->v_arr, vec3(0,0,0), step->v_body, step->body, arr_periapsis, HYP_ARRIVAL);
		sprintf(s_labels, "Arrival\n"
						  "T+:\n"
						  "RadPer:\n"
						  "AltPer:\n"
						  "Min Incl:\n"
						  "Max Incl:\n\n"
						  "C3Energy:\n"
						  "RHA (in):\n"
						  "DHA (in):");

		sprintf(s_values, "\n%.2f days\n"
						  "%.0f km\n"
						  "%.0f km\n"
						  "%.2f°\n"
						  "%.2f°\n\n"
						  "%.2f km²/s²\n"
						  "%.2f°\n"
						  "%.2f°",
				dt_in_days, hyp_params.rp / 1000, (hyp_params.rp-step->body->radius) / 1000, rad2deg(angle_plane3_vec3(get_body_equatorial_plane(step->body), subtract_vec3(step->v_arr, step->v_body))), 180.0 - fabs(rad2deg(angle_plane3_vec3(get_body_equatorial_plane(step->body), subtract_vec3(step->v_arr, step->v_body)))), hyp_params.c3_energy / 1e6,
				rad2deg(hyp_params.incoming.bplane_angle), rad2deg(hyp_params.incoming.decl));

	} else {
		if(step->body != NULL) {
			Vector3 v_arr = step->v_arr;
			Vector3 v_dep = step->next[0]->v_dep;
			Vector3 v_body = step->v_body;
			double incl = get_flyby_inclination(v_arr, v_dep, v_body, get_body_equatorial_plane(step->body));

			hyp_params = get_hyperbola_params(step->v_arr, step->next[0]->v_dep,
											  step->v_body, step->body, 0, HYP_FLYBY);
			// The unpowered periapsis can lie below the surface; the route actually flies the powered one.
			PoweredFlybyResult powered_flyby;
			int powered_valid = calculate_flyby_with_moons(v_arr, v_dep, v_body, step->body,
				flyby_minimum_periapsis(step->body), step->date, &powered_flyby);
			double periapsis_radius = powered_valid ? powered_flyby.periapsis_radius : hyp_params.rp;
			double flyby_burn = powered_valid ? powered_flyby.burn_delta_v : 0;
			double dt_in_days = step->date - get_first(step)->date;
			if(date_type == DATE_KERBAL) dt_in_days *= 4;

			sprintf(s_labels, "Hyperbola\n"
							  "T+:\n"
							  "RadPer:\n"
							  "AltPer:\n"
							  "Burn:\n"
							  "Unpowered AltPer:\n"
							  "Inclination:\n\n"
							  "C3Energy:\n"
							  "RHA (in):\n"
							  "DHA (in):\n"
							  "BVAZI (in):\n"
							  "RHA (out):\n"
							  "DHA (out):\n"
							  "BVAZI (out):");

			sprintf(s_values, "\n%.2f days\n"
							  "%.0f km\n"
							  "%.0f km\n"
							  "%.1f m/s\n"
							  "%.0f km%s\n"
							  "%.2f°\n\n"
							  "%.2f km²/s²\n"
							  "%.2f°\n"
							  "%.2f°\n"
							  "%.2f°\n"
							  "%.2f°\n"
							  "%.2f°\n"
							  "%.2f°",
					dt_in_days, periapsis_radius / 1000, (periapsis_radius-step->body->radius) / 1000, flyby_burn, (hyp_params.rp - step->body->radius) / 1000, flyby_periapsis_shortfall(step) > 0 ? " (!)" : "", rad2deg(incl),
					hyp_params.c3_energy / 1e6,
					rad2deg(hyp_params.incoming.bplane_angle), rad2deg(hyp_params.incoming.decl),
					rad2deg(hyp_params.incoming.bvazi),
					rad2deg(hyp_params.outgoing.bplane_angle), rad2deg(hyp_params.outgoing.decl),
					rad2deg(hyp_params.outgoing.bvazi));
		} else {
			double dt_in_days = step->date - get_first(step)->date;
			if(date_type == DATE_KERBAL) dt_in_days *= 4;
			double dist_to_sun = mag_vec3(step->r);

			Vector3 orbit_prograde = step->v_arr;
			Vector3 orbit_normal = cross_vec3(step->r, step->v_arr);
			Vector3 orbit_radialin = cross_vec3(orbit_normal, step->v_arr);
			Vector3 dv_vec = subtract_vec3(step->next[0]->v_dep, step->v_arr);

			// dv vector in S/C coordinate system (prograde, radial in, normal) (sign it if projected vector more than 90° from target vector / pointing in opposite direction)
			Vector3 dv_vec_sc = {
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_prograde)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_prograde), orbit_prograde) < M_PI/2 ? 1 : -1),
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_radialin)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_radialin), orbit_radialin) < M_PI/2 ? 1 : -1),
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_normal)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_normal), orbit_normal) < M_PI/2 ? 1 : -1)
			};

			sprintf(s_labels, "DSM\n"
							  "T+:\n"
							  "Distance to the Sun:\n"
							  "Dv Prograde:\n"
							  "Dv Radial:\n"
							  "Dv Normal:\n"
							  "Total:");

			sprintf(s_values, "\n%.2f days\n"
							  "%.3f AU\n"
							  "%.3f m/s\n"
							  "%.3f m/s\n"
							  "%.3f m/s\n"
							  "%.3f m/s",
					dt_in_days, dist_to_sun / 1.495978707e11, dv_vec_sc.x, dv_vec_sc.y, dv_vec_sc.z, mag_vec3(dv_vec_sc));
		}
	}
}
