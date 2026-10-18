#include "itinerary_calculator_internal.h"
#include "tools/file_io.h"
#include "tools/thread_pool.h"
#include "tools/itinerary_date_limits.h"
#include "gui/css_loader.h"
#include <math.h>

struct Itin_Calc_Data ic_calc_data;
struct Itin_Calc_Results ic_results;

void save_itineraries_ic(struct ItinStep **departures, int num_deps, int num_nodes, int num_itins) {
	if(departures == NULL || num_deps == 0) return;
	char filepath[255];
	if(!get_path_from_file_chooser(filepath,  ".itins", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	store_itineraries_in_bfile(departures, num_nodes, num_deps, num_itins, ic_calc_data, ic_system, filepath, get_current_bin_file_type());
}

static gboolean end_ic_calc_thread(gpointer user_data) {
	(void)user_data;
	starting_orbit_editor_finish_search();
	end_sc_ic_progress_window();
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_window), 1);

	save_itineraries_ic(ic_results.departures, ic_results.num_deps, ic_results.num_nodes, ic_results.num_itins);
	for(int i = 0; i < ic_results.num_deps; i++) free_itinerary(ic_results.departures[i]);
	free(ic_results.departures);
	free(ic_calc_data.seq_info.to_target.flyby_bodies);
	if(ic_results.num_deps == 0) {
		char reason[200], message[256];
		describe_search_rejections(&ic_results.rejections, reason, sizeof(reason));
		g_snprintf(message, sizeof(message), "No itineraries found!%s%s", reason[0] ? "\n" : "", reason);
		show_msg_window(message);
	}
	return G_SOURCE_REMOVE;
}

static void prepare_ic_calc_data_from_widgets() {
	char *string;

	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	ic_calc_data.jd_min_dep = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdepdate));
	ic_calc_data.jd_max_dep = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	int departure_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody));
	if(ic_ksp_start_active && departure_index >= 0 && departure_index < ic_system->num_bodies &&
		ic_system->bodies[departure_index] == ic_ksp_start_body) {
		// The vessel cannot depart before its snapshot, but the latest departure date is the user's choice.
		ic_calc_data.jd_min_dep = ic_ksp_start_jd;
		if(!(ic_calc_data.jd_max_dep >= ic_calc_data.jd_min_dep)) ic_calc_data.jd_max_dep = ic_calc_data.jd_min_dep;
	}
	int use_max_arrival_date = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate));
	double max_arrival_jd = 0;
	double user_max_duration = 0;
	if(use_max_arrival_date) {
		string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxarrdate));
		max_arrival_jd = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	} else {
		string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdur));
		user_max_duration = strtod(string, NULL);
	}
	ItineraryDateLimits time_limits = calculate_itinerary_date_limits(
		use_max_arrival_date,
		ic_calc_data.jd_min_dep,
		ic_calc_data.jd_max_dep,
		max_arrival_jd,
		user_max_duration,
		get_settings_datetime_type());
	ic_calc_data.jd_max_arr = time_limits.max_arrival_jd;
	ic_calc_data.max_duration = time_limits.max_duration_days;

	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_depdv));
	ic_calc_data.dv_filter.max_depdv = strtod(string, NULL);
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_satdv));
	ic_calc_data.dv_filter.max_satdv = strtod(string, NULL);
	// Departure burn is capped by its own limit and every later burn by the satellite limit.
	ic_calc_data.dv_filter.max_totdv = ic_calc_data.dv_filter.max_depdv + ic_calc_data.dv_filter.max_satdv;
	string = (char*) gtk_combo_box_get_active_id(GTK_COMBO_BOX(cb_ic_transfertype));
	ic_calc_data.dv_filter.last_transfer_type = (int) strtol(string, NULL, 10);
	ic_calc_data.allow_dsm = cb_ic_allow_dsm != NULL &&
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb_ic_allow_dsm));

	ic_calc_data.dv_filter.dep_periapsis = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody))]->atmo_alt + ic_dep_periapsis;
	ic_calc_data.dv_filter.arr_periapsis = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody))]->atmo_alt + ic_arr_periapsis;
	
	ic_calc_data.calc_acc.lambert = 1;
	ic_calc_data.calc_acc.fb_finder = 1;

	ic_calc_data.num_deps_per_date = 500;
	ic_calc_data.step_dep_date = 1;
	ic_calc_data.max_num_waiting_orbits = 0;

	// Make sure arrival body is a fly-by body
	Body *arr_body = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody))];
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gtk_grid_get_child_at(GTK_GRID(grid_ic_fbbodies), 0, get_body_system_id(arr_body, ic_system))), 1);

	struct ItinSequenceInfoToTarget seq_info = {
			.system = ic_system,
			.dep_body = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody))],
			.arr_body = arr_body,
			.num_flyby_bodies = get_num_selected_bodies_from_grid(grid_ic_fbbodies),
			.allow_resonant = cb_ic_allow_resonant != NULL &&
				gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb_ic_allow_resonant)),
	};
	seq_info.flyby_bodies = get_bodies_from_grid(grid_ic_fbbodies, seq_info.num_flyby_bodies);
	ic_calc_data.seq_info.to_target = seq_info;

}

static gpointer ic_calc_thread(gpointer user_data) {
	(void)user_data;
	ic_results = search_for_itineraries(ic_calc_data);
	g_idle_add(end_ic_calc_thread, NULL);
	return NULL;
}

Body *ic_get_departure_body(void) {
	if(ic_system == NULL) return NULL;
	int index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody));
	return index >= 0 && index < ic_system->num_bodies ? ic_system->bodies[index] : NULL;
}

G_MODULE_EXPORT void on_calc_ic() {
	if(ic_system == NULL) return;
	char starting_error[256];
	if(!starting_orbit_editor_prepare_search(ic_get_departure_body(), starting_error, sizeof(starting_error))) {
		show_msg_window(starting_error);
		return;
	}
	prepare_ic_calc_data_from_widgets();
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_window), 0);
	prepare_thread_pool_counters();
	init_sc_ic_progress_window();
	GThread *calc_thread = g_thread_new("calc_thread", (GThreadFunc) ic_calc_thread, NULL);
	if(calc_thread != NULL) g_thread_unref(calc_thread);
}
