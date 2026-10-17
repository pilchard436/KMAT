#include "transfer_planner_internal.h"
#include "gui/settings.h"
#include "gui/info_win_manager.h"
#include "gui/gui_manager.h"
#include "tools/file_io.h"
#include "tools/gmat_interface.h"
#include "orbit_calculator/flyby_policy.h"

#include <math.h>
#include <stdio.h>

G_MODULE_EXPORT void on_show_itin_overview() {
	if(curr_transfer_tp == NULL) return;
	char text[10000];
	int previous = set_flyby_unrestricted_periapsis(1);
	itinerary_detailed_overview_to_string(curr_transfer_tp, get_settings_datetime_type(), tp_dep_periapsis, tp_arr_periapsis, text);
	set_flyby_unrestricted_periapsis(previous);
	show_msg_window_owned(text, &tp_details_owner);
}

G_MODULE_EXPORT void on_save_itinerary(GtkWidget* widget, gpointer data) {
	if(tp_system == NULL) return;
	struct ItinStep *first = get_first(curr_transfer_tp);
	if(first == NULL || !is_valid_itinerary(get_last(curr_transfer_tp))) return;

	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itin", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	store_single_itinerary_in_bfile(first, tp_system, filepath);
}

G_MODULE_EXPORT void on_load_itinerary(GtkWidget* widget, gpointer data) {
	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itin", GTK_FILE_CHOOSER_ACTION_OPEN, "")) return;

	if(curr_transfer_tp != NULL) {
		fix_tfbody = TRUE;
		update_body_dropdown(GTK_COMBO_BOX(cb_tp_tfbody), NULL);
		free_itinerary(get_first(curr_transfer_tp));
	}
	if(!is_available_system(tp_system) && tp_system != NULL) free_celestial_system(tp_system);
	curr_transfer_tp = NULL;
	tp_system = NULL;
	if(gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_system)) == get_num_available_systems()) remove_combobox_last_entry(GTK_COMBO_BOX(cb_tp_system));

	struct ItinLoadFileResults load_results = load_single_itinerary_from_bfile(filepath);
	if(load_results.itin == NULL) return;
	curr_transfer_tp = get_first(load_results.itin);
	tp_system = load_results.system;
	tp_route_unconverged = 0;
	current_date_tp = curr_transfer_tp->date;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate), 0);
	tp_update_bodies();
	// Keep the saved velocities: re-solving from body centers would change a powered-flyby route's delta-v.
	tp_dep_periapsis = get_first(curr_transfer_tp)->body->atmo_alt + 50e3;
	tp_arr_periapsis = get_last(curr_transfer_tp)->body->atmo_alt + 50e3;
	update_itin_body_osvs(get_first(curr_transfer_tp), tp_system);
	update();
	update_camera_to_celestial_system(tp_system_camera, tp_system, deg2rad(90), 0); tp_focus_body = NULL; tp_focus_spacecraft = 0;
	camera_zoom_to_fit_itinerary(tp_system_camera, curr_transfer_tp);
	update_tp_system_view();
	char system_name[50];
	sprintf(system_name, "- %s -", tp_system->name);
	append_combobox_entry(GTK_COMBO_BOX(cb_tp_system), system_name);
	update_central_body_dropdown(GTK_COMBO_BOX(cb_tp_central_body), tp_system);

	struct ItinStep *step2pr = get_first(curr_transfer_tp);
	HyperbolaParams hyp_params = get_hyperbola_params(vec3(0,0,0), step2pr->next[0]->v_dep, step2pr->v_body,
																		   step2pr->body, 200e3, HYP_DEPARTURE);
	printf("\nDeparture Hyperbola %s\n"
		   "Date: %f\n"
		   "OutgoingRadPer: %f km\n"
		   "OutgoingC3Energy: %f km²/s²\n"
		   "OutgoingRHA: %f°\n"
		   "OutgoingDHA: %f°\n"
		   "OutgoingBVAZI: -°\n"
		   "TA: 0.0°\n",
		   step2pr->body->name, step2pr->date, hyp_params.rp/1000, hyp_params.c3_energy/1e6,
		   rad2deg(hyp_params.outgoing.bplane_angle), rad2deg(hyp_params.outgoing.decl));
	
	step2pr = step2pr->next[0];
	while(step2pr->num_next_nodes != 0) {
		if(step2pr->body != NULL) {
			Vector3 v_arr = step2pr->v_arr;
			Vector3 v_dep = step2pr->next[0]->v_dep;
			Vector3 v_body = step2pr->v_body;
			double incl = get_flyby_inclination(v_arr, v_dep, v_body, get_body_equatorial_plane(step2pr->body));

			hyp_params = get_hyperbola_params(step2pr->v_arr, step2pr->next[0]->v_dep, step2pr->v_body, step2pr->body, 0, HYP_FLYBY);
			double dt_in_days = step2pr->date - step2pr->prev->date;

			printf("\nFly-by Hyperbola %s (Travel Time: %.2f days)\n"
				   "Date: %f\n"
				   "RadPer: %f km\n"
				   "Inclination: %f°\n"
				   "C3Energy: %f km²/s²\n"
				   "IncomingRHA: %f°\n"
				   "IncomingDHA: %f°\n"
				   "IncomingBVAZI: %f°\n"
				   "OutgoingRHA: %f°\n"
				   "OutgoingDHA: %f°\n"
				   "OutgoingBVAZI: %f°\n"
				   "TA: 0.0°\n",
				   step2pr->body->name, dt_in_days, step2pr->date, hyp_params.rp / 1000, rad2deg(incl),
				   hyp_params.c3_energy / 1e6,
				   rad2deg(hyp_params.incoming.bplane_angle), rad2deg(hyp_params.incoming.decl),
				   rad2deg(hyp_params.incoming.bvazi),
				   rad2deg(hyp_params.outgoing.bplane_angle), rad2deg(hyp_params.outgoing.decl),
				   rad2deg(hyp_params.outgoing.bvazi));
			step2pr = step2pr->next[0];
		} else {
			double dt_in_days = step2pr->date - step2pr->prev->date;
			double dist_to_sun = mag_vec3(step2pr->r);

			Vector3 orbit_prograde = step2pr->v_arr;
			Vector3 orbit_normal = cross_vec3(step2pr->r, step2pr->v_arr);
			Vector3 orbit_radialin = cross_vec3(orbit_normal, step2pr->v_arr);
			Vector3 dv_vec = subtract_vec3(step2pr->next[0]->v_dep, step2pr->v_arr);

			// dv vector in S/C coordinate system (prograde, radial in, normal) (sign it if projected vector more than 90° from target vector / pointing in opposite direction)
			Vector3 dv_vec_sc = {
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_prograde)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_prograde), orbit_prograde) < M_PI/2 ? 1 : -1),
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_radialin)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_radialin), orbit_radialin) < M_PI/2 ? 1 : -1),
					mag_vec3(proj_vec3_vec3(dv_vec, orbit_normal)) * (angle_vec3_vec3(proj_vec3_vec3(dv_vec, orbit_normal), orbit_normal) < M_PI/2 ? 1 : -1)
			};

			printf("\nDeep Space Maneuver (Travel Time: %.2f days)\n"
				   "Date: %f\n"
				   "Distance to the Sun: %.3f AU\n"
				   "Dv Prograde: %f m/s\n"
				   "Dv Radial: %f m/s\n"
				   "Dv Normal: %f m/s\n"
				   "Total: %f m/s\n",
				   dt_in_days, step2pr->date, dist_to_sun / 1.495978707e11, dv_vec_sc.x, dv_vec_sc.y, dv_vec_sc.z, mag_vec3(dv_vec_sc));
			step2pr = step2pr->next[0];
		}
	}
	
	double dt_in_days = step2pr->date - step2pr->prev->date;
	hyp_params = get_hyperbola_params(step2pr->v_arr, vec3(0,0,0), step2pr->v_body, step2pr->body, 200e3, HYP_ARRIVAL);
	printf("\nArrival Hyperbola %s (Travel Time: %.2f days)\n"
		   "Date: %f\n"
		   "IncomingRadPer: %f km\n"
		   "IncomingC3Energy: %f km²/s²\n"
		   "IncomingRHA: %f°\n"
		   "IncomingDHA: %f°\n"
		   "IncomingBVAZI: -°\n"
		   "TA: 0.0°\n",
		   step2pr->body->name, dt_in_days, step2pr->date, hyp_params.rp/1000, hyp_params.c3_energy/1e6,
		   rad2deg(hyp_params.incoming.bplane_angle), rad2deg(hyp_params.incoming.decl));
}

G_MODULE_EXPORT void on_create_gmat_script() {
	if(curr_transfer_tp != NULL) write_gmat_script(curr_transfer_tp, "transfer.script");
}
