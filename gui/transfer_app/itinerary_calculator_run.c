#include "itinerary_calculator_internal.h"
#include "tools/file_io.h"
#include "tools/thread_pool.h"
#include "tools/itinerary_date_limits.h"
#include "tools/itinerary_dv_limit.h"
#include "gui/css_loader.h"
#include "orbit_calculator/flyby_policy.h"
#include <math.h>

struct Itin_Calc_Data ic_calc_data;
struct Itin_Calc_Results ic_results;

void save_itineraries_ic() {
	if((ic_results.departures == NULL || ic_results.num_deps == 0) &&
		(ic_results.capture_departures == NULL || ic_results.capture_num_deps == 0)) return;
	char filepath[255];
	if(!get_path_from_file_chooser(filepath,  ".itins", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	store_itineraries_in_bfile_both_modes(ic_results, ic_calc_data, ic_system, filepath);
}

static gboolean end_ic_calc_thread(gpointer user_data) {
	(void)user_data;
	starting_orbit_editor_finish_search();
	end_sc_ic_progress_window();
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_window), 1);

	if(ic_results.capture_calculation_failed)
		show_msg_window("Capture results could not be generated. The Fly-By results are available, but were not saved as a dual-mode file.");
	else
		save_itineraries_ic();
	for(int i = 0; i < ic_results.num_deps; i++) free_itinerary(ic_results.departures[i]);
	free(ic_results.departures);
	for(int i = 0; i < ic_results.capture_num_deps; i++) free_itinerary(ic_results.capture_departures[i]);
	free(ic_results.capture_departures);
	free(ic_calc_data.seq_info.to_target.flyby_bodies);
	if(ic_results.num_itins == 0 && ic_results.capture_num_itins == 0) {
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

	ic_calc_data.dv_filter.max_depdv = parse_itinerary_dv_limit(gtk_entry_get_text(GTK_ENTRY(tf_ic_depdv)));
	ic_calc_data.dv_filter.max_satdv = parse_itinerary_dv_limit(gtk_entry_get_text(GTK_ENTRY(tf_ic_satdv)));
	// Departure burn is capped by its own limit and every later burn by the satellite limit.
	ic_calc_data.dv_filter.max_totdv = ic_calc_data.dv_filter.max_depdv + ic_calc_data.dv_filter.max_satdv;
	ic_calc_data.dv_filter.last_transfer_type = TF_BOTH;
	ic_calc_data.disable_powered_flybys = cb_ic_allow_powered_flybys != NULL &&
		!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb_ic_allow_powered_flybys));
	int max_flybys_selection = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_max_flybys));
	set_max_flybys_per_route(max_flybys_selection >= 0 ? max_flybys_selection + 1 : MAX_FLYBYS_PER_ROUTE);
	ic_calc_data.allow_dsm = !ic_calc_data.disable_powered_flybys && cb_ic_allow_dsm != NULL &&
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb_ic_allow_dsm));
	ic_calc_data.moon_aware_flybys = cb_ic_moon_aware_flybys != NULL &&
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb_ic_moon_aware_flybys));
	ic_calc_data.moon_assist_arrival = cb_ic_moon_assist_arrival != NULL &&
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb_ic_moon_assist_arrival));
	set_moon_aware_flybys_enabled(ic_calc_data.moon_aware_flybys);
	set_moon_assist_enabled(ic_calc_data.moon_assist_arrival);

	ic_calc_data.dv_filter.dep_periapsis = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody))]->atmo_alt + ic_dep_periapsis;
	ic_calc_data.dv_filter.arr_periapsis = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody))]->atmo_alt + ic_arr_periapsis;
	
	ic_calc_data.calc_acc.lambert = 1;
	ic_calc_data.calc_acc.fb_finder = 1;

	ic_calc_data.num_deps_per_date = 500;
	ic_calc_data.step_dep_date =
		get_settings_datetime_type() == DATE_KERBAL ? 0.25 : 1;
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
	StartingOrbit custom_orbit = {0};
	Body *custom_orbit_body = NULL;
	int has_custom_orbit = starting_orbit_get_active(&custom_orbit, &custom_orbit_body) &&
		custom_orbit_body == seq_info.dep_body;
	ic_calc_data.search_options = (ItinSearchOptions) {
		.known = 1,
		.disable_powered_flybys = ic_calc_data.disable_powered_flybys,
		.allow_dsm = ic_calc_data.allow_dsm,
		.moon_aware_flybys = ic_calc_data.moon_aware_flybys,
		.moon_assist_arrival = ic_calc_data.moon_assist_arrival,
		.allow_resonant = seq_info.allow_resonant,
		.last_transfer_type = TF_BOTH,
		.use_custom_starting_orbit = has_custom_orbit,
		.starting_orbit_body_id = has_custom_orbit ? get_body_system_id(custom_orbit_body, ic_system) : -1,
		.starting_orbit = custom_orbit
	};

}

static gpointer ic_calc_thread(gpointer user_data) {
	(void)user_data;
	ic_results = search_for_itineraries_both_modes(ic_calc_data);
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
	if(!validate_ic_date_range_widgets()) return;
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
