#include "transfer_planner_internal.h"
#include "gui/settings.h"
#include "gui/info_win_manager.h"
#include "gui/gui_manager.h"
#include "tools/file_io.h"
#include "orbit_calculator/itinerary_tree.h"
#include "orbit_calculator/flyby_policy.h"
#include "tools/starting_orbit.h"
#include "ksp/ksp_session.h"
#include <orbitlib_fileio.h>
#include <glib/gstdio.h>

#include <math.h>
#include <string.h>
#include <stdio.h>

G_MODULE_EXPORT void on_show_itin_overview() {
	if(curr_transfer_tp == NULL) return;
	char text[10000];
	itinerary_detailed_overview_to_string(curr_transfer_tp, get_settings_datetime_type(), tp_dep_periapsis, tp_arr_periapsis, text);
	show_msg_window_owned(text, &tp_details_owner);
}

typedef struct KspNodeExportTask {
	GtkWidget *button;
	struct ItinStep *route;
	int success;
	int correction;
	char message[768];
} KspNodeExportTask;

static gboolean finish_ksp_node_export(gpointer data) {
	KspNodeExportTask *task = data;
	gtk_widget_set_sensitive(task->button, TRUE);
	GtkWidget *parent = gtk_widget_get_toplevel(task->button);
	GtkWidget *dialog = gtk_message_dialog_new(GTK_IS_WINDOW(parent) ? GTK_WINDOW(parent) : NULL,
		GTK_DIALOG_MODAL, task->success ? GTK_MESSAGE_INFO : GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, NULL);
	// A trailing "Note:" paragraph is a warning and is shown in red.
	char *warning = strstr(task->message, "\nNote:");
	char *plain = g_markup_escape_text(task->message, warning != NULL ? warning - task->message : -1);
	char *warning_text = warning != NULL ? g_markup_escape_text(warning + 1, -1) : NULL;
	char *markup = warning_text != NULL ? g_strdup_printf("%s\n<span foreground=\"#d02020\" weight=\"bold\">%s</span>", plain, warning_text) : g_strdup(plain);
	gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog), markup);
	g_free(markup);
	g_free(warning_text);
	g_free(plain);
	GtkWidget *message_area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
	GList *children = gtk_container_get_children(GTK_CONTAINER(message_area));
	for(GList *child = children; child != NULL; child = child->next)
		if(GTK_IS_LABEL(child->data)) gtk_label_set_selectable(GTK_LABEL(child->data), TRUE);
	g_list_free(children);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	free_itinerary(get_first(task->route));
	g_object_unref(task->button);
	g_free(task);
	return G_SOURCE_REMOVE;
}

static gpointer run_ksp_node_export(gpointer data) {
	KspNodeExportTask *task = data;
	task->success = task->correction ? ksp_session_add_correction_node(task->route, tp_arr_periapsis, task->message, sizeof(task->message)) :
		ksp_session_add_next_maneuver_node(task->route, tp_dep_periapsis,
		tp_arr_periapsis, tp_last_transfer_type, task->message, sizeof(task->message));
	g_idle_add(finish_ksp_node_export, task);
	return NULL;
}

static void start_ksp_node_export(GtkWidget *widget, int correction) {
	if(!ksp_session_is_connected()) {
		show_msg_window("Connect to a matching KSP system from Settings first.");
		return;
	}
	if(curr_transfer_tp == NULL) {
		show_msg_window("Load or calculate an itinerary in the Transfer Planner first.");
		return;
	}
	KspNodeExportTask *task = g_new0(KspNodeExportTask, 1);
	task->correction = correction;
	task->route = create_itin_copy(get_first(curr_transfer_tp));
	if(task->route == NULL) {
		g_free(task);
		show_msg_window("Could not copy the current itinerary.");
		return;
	}
	task->button = g_object_ref(widget);
	gtk_widget_set_sensitive(widget, FALSE);
	GThread *thread = g_thread_new("ksp-node-export", run_ksp_node_export, task);
	if(thread == NULL) {
		gtk_widget_set_sensitive(widget, TRUE);
		free_itinerary(task->route);
		g_object_unref(task->button);
		g_free(task);
		show_msg_window("Could not start the KSP node export worker.");
		return;
	}
	g_thread_unref(thread);
}

G_MODULE_EXPORT void on_tp_send_ksp_node(GtkWidget *widget, gpointer data) {
	(void)data;
	start_ksp_node_export(widget, 0);
}

G_MODULE_EXPORT void on_tp_send_ksp_correction(GtkWidget *widget, gpointer data) {
	(void)data;
	start_ksp_node_export(widget, 1);
}

G_MODULE_EXPORT void on_save_itinerary(GtkWidget* widget, gpointer data) {
	if(tp_system == NULL) return;
	struct ItinStep *first = get_first(curr_transfer_tp);
	if(first == NULL || !is_valid_itinerary(get_last(curr_transfer_tp))) return;

	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itin", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	ItinSearchOptions options = tp_search_options;
	if(options.known) options.last_transfer_type = tp_last_transfer_type;
	store_single_itinerary_in_bfile_with_options(first, tp_system, filepath, options,
		tp_dep_periapsis, tp_arr_periapsis);
}

static void apply_tp_search_options(ItinSearchOptions options, CelestSystem *system) {
	tp_search_options = options;
	set_moon_assist_enabled(options.known && options.moon_assist_arrival);
	set_moon_aware_flybys_enabled(options.known && options.moon_aware_flybys);
	if(options.known && options.use_custom_starting_orbit && system != NULL &&
		options.starting_orbit_body_id >= 0 && options.starting_orbit_body_id < system->num_bodies)
		starting_orbit_set_active(&options.starting_orbit, system->bodies[options.starting_orbit_body_id]);
	else
		starting_orbit_set_active(NULL, NULL);
}

static int same_value(double a, double b) {
	return fabs(a - b) <= 1e-9 * fmax(fabs(a), fabs(b));
}

// A loaded itinerary whose system matches an available one (same bodies and moons) is shown in that one.
static int find_identical_available_system(CelestSystem *loaded) {
	for(int i = 0; loaded != NULL && i < get_num_available_systems(); i++) {
		CelestSystem *available = get_available_systems()[i];
		if(strcmp(available->name, loaded->name) != 0 || available->num_bodies != loaded->num_bodies ||
			strcmp(available->cb->name, loaded->cb->name) != 0 || !same_value(available->cb->mu, loaded->cb->mu)) continue;
		int same = 1;
		for(int j = 0; same && j < loaded->num_bodies; j++) {
			Body *a = available->bodies[j], *b = loaded->bodies[j];
			int moons_a = a->system != NULL ? a->system->num_bodies : 0, moons_b = b->system != NULL ? b->system->num_bodies : 0;
			same = strcmp(a->name, b->name) == 0 && same_value(a->mu, b->mu) && same_value(a->orbit.a, b->orbit.a) && moons_a == moons_b;
			for(int k = 0; same && k < moons_a; k++)
				same = strcmp(a->system->bodies[k]->name, b->system->bodies[k]->name) == 0;
		}
		if(same) return i;
	}
	return -1;
}

static void tp_show_loaded_itinerary(struct ItinLoadFileResults load_results) {
	if(curr_transfer_tp != NULL) {
		fix_tfbody = TRUE;
		update_body_dropdown(GTK_COMBO_BOX(cb_tp_tfbody), NULL);
		free_itinerary(get_first(curr_transfer_tp));
	}
	if(!is_available_system(tp_system) && tp_system != NULL) free_celestial_system(tp_system);
	curr_transfer_tp = NULL;
	tp_system = NULL;
	apply_tp_search_options((ItinSearchOptions){.starting_orbit_body_id = -1, .last_transfer_type = -1}, NULL);
	tp_set_last_transfer_type(TF_FLYBY, 0);
	// The focused body belonged to the system that was just freed.
	tp_focus_body = NULL; tp_focus_spacecraft = 0;
	if(tp_loaded_system_entry) remove_combobox_last_entry(GTK_COMBO_BOX(cb_tp_system));
	tp_loaded_system_entry = 0;

	enum LastTransferType loaded_type = load_results.search_options.last_transfer_type == TF_CIRC ?
		TF_CAPTURE : (enum LastTransferType)load_results.search_options.last_transfer_type;
	tp_set_last_transfer_type(load_results.search_options.known &&
		loaded_type >= TF_FLYBY && loaded_type <= TF_CAPTURE ? loaded_type : TF_FLYBY, 0);
	int matching_system = find_identical_available_system(load_results.system);
	if(matching_system >= 0) {
		CelestSystem *available = get_available_systems()[matching_system];
		for(struct ItinStep *step = get_first(load_results.itin); step != NULL; step = step->num_next_nodes > 0 ? step->next[0] : NULL)
			if(step->body != NULL) step->body = available->bodies[get_body_system_id(step->body, load_results.system)];
		free_celestial_system(load_results.system);
		load_results.system = available;
	}
	curr_transfer_tp = get_first(load_results.itin);
	tp_system = load_results.system;
	apply_tp_search_options(load_results.search_options, tp_system);
	tp_route_unconverged = 0;
	current_date_tp = curr_transfer_tp->date;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate), 0);
	tp_update_bodies();
	// Keep the saved velocities: re-solving from body centers would change a powered-flyby route's delta-v.
	tp_dep_periapsis = load_results.arrival_dv_cache_stored ? load_results.departure_periapsis :
		get_first(curr_transfer_tp)->body->atmo_alt + 50e3;
	tp_arr_periapsis = load_results.arrival_dv_cache_stored ? load_results.arrival_periapsis :
		get_last(curr_transfer_tp)->body->atmo_alt + 50e3;
	update_itin_body_osvs(get_first(curr_transfer_tp), tp_system);
	update();
	update_camera_to_celestial_system(tp_system_camera, tp_system, deg2rad(90), 0); tp_focus_body = NULL; tp_focus_spacecraft = 0;
	camera_zoom_to_fit_itinerary(tp_system_camera, curr_transfer_tp);
	update_tp_system_view();
	if(matching_system >= 0) gtk_combo_box_set_active(GTK_COMBO_BOX(cb_tp_system), matching_system);
	else {
		char system_name[50];
		sprintf(system_name, "- %s -", tp_system->name);
		append_combobox_entry(GTK_COMBO_BOX(cb_tp_system), system_name);
		tp_loaded_system_entry = 1;
	}

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

G_MODULE_EXPORT void on_load_itinerary(GtkWidget* widget, gpointer data) {
	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itin", GTK_FILE_CHOOSER_ACTION_OPEN, "")) return;
	struct ItinLoadFileResults load_results = load_single_itinerary_from_bfile(filepath);
	if(load_results.itin == NULL) return;
	tp_show_loaded_itinerary(load_results);
}

void tp_open_itinerary(struct ItinStep *itin, CelestSystem *system, ItinSearchOptions search_options,
		double dep_periapsis, double arr_periapsis) {
	// A round trip through a temporary file gives the planner its own copy of the route and of the system,
	// with the same stored values that saving and loading it would give.
	gchar *temp_path = NULL;
	gint fd = g_file_open_tmp("kmat-route-XXXXXX.itin", &temp_path, NULL);
	if(fd < 0) return;
	g_close(fd, NULL);
	char filepath[1024];
	g_strlcpy(filepath, temp_path, sizeof(filepath));
	g_free(temp_path);
	store_single_itinerary_in_bfile_with_options(itin, system, filepath, search_options, dep_periapsis, arr_periapsis);
	struct ItinLoadFileResults load_results = load_single_itinerary_from_bfile(filepath);
	g_remove(filepath);
	if(load_results.itin == NULL) return;
	tp_show_loaded_itinerary(load_results);
	show_page_with_widget(GTK_WIDGET(da_tp));
}
