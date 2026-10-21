#include "itinerary_calculator_internal.h"
#include "gui/css_loader.h"
#include "tools/file_io.h"
#include "tools/itinerary_date_limits.h"
#include "tools/itinerary_reference_defaults.h"
#include "tools/thread_pool.h"
#include <math.h>


static GtkWidget *lb_ic_ksp_note;
GObject *tf_ic_window;
GObject *cb_ic_system;
GObject *cb_ic_central_body;
GObject *lb_ic_central_body;
GObject *cb_ic_depbody;
GObject *cb_ic_arrbody;
GObject *tf_ic_mindepdate;
GObject *tf_ic_maxdepdate;
GObject *tf_ic_maxarrdate;
GtkWidget *box_ic_maxarrdate;
GObject *tf_ic_maxdur;
GObject *rb_ic_maxarrdate;
GObject *rb_ic_maxdur;
GObject *cb_ic_transfertype;
GtkWidget *cb_ic_allow_dsm;
GtkWidget *cb_ic_allow_resonant;
GtkWidget *cb_ic_moon_aware_flybys;
GtkWidget *cb_ic_moon_assist_arrival;
GObject *tf_ic_depdv;
GObject *tf_ic_satdv;
GtkWidget *lb_ic_date_warning;
GtkWidget *bt_ic_calc;
GObject *vp_ic_fbbodies;
GtkWidget *grid_ic_fbbodies;
GtkWidget *ic_latest_dep_minus_month_button;
GtkWidget *ic_latest_dep_plus_month_button;
GtkWidget *ic_latest_arr_minus_month_button;
GtkWidget *ic_latest_arr_plus_month_button;
gboolean ic_changing_date_type;
gboolean ic_syncing_time_limits;
gboolean ic_setting_ksp_start_date;
gboolean ic_ksp_start_active;
double ic_ksp_start_jd;
Body *ic_ksp_start_body;

CelestSystem *ic_system;
static gboolean ic_initialized;

double ic_dep_periapsis = 50e3;
double ic_arr_periapsis = 50e3;

static void update_datetime_type_for_system(CelestSystem *system) {
	if(system == NULL) return;
	CelestSystem *top_level_system = get_top_level_system(system);
	if(top_level_system == NULL || top_level_system->name == NULL) return;
	enum DateType date_type = itinerary_date_type_for_system_name(top_level_system->name);
	set_settings_datetime_type(date_type);
}

void ic_initialize_date_defaults(void) {
	if(ic_system == NULL) return;
	update_datetime_type_for_system(ic_system);
	enum DateType date_type = get_settings_datetime_type();
	ItineraryStartupDateDefaults defaults = calculate_itinerary_startup_date_defaults(date_type);
	char date_text[32];
	date_to_string(defaults.departure_date, date_text, 0);
	gtk_entry_set_text(GTK_ENTRY(tf_ic_mindepdate), date_text);
	update_ic_reference_defaults();
	ic_syncing_time_limits = TRUE;
	date_to_string(defaults.latest_arrival_date, date_text, 0);
	gtk_entry_set_text(GTK_ENTRY(tf_ic_maxarrdate), date_text);
	g_snprintf(date_text, sizeof(date_text), "%d", defaults.max_duration_user_days);
	gtk_entry_set_text(GTK_ENTRY(tf_ic_maxdur), date_text);
	ic_syncing_time_limits = FALSE;
	gtk_entry_set_text(GTK_ENTRY(tf_ic_depdv), "");
	gtk_entry_set_text(GTK_ENTRY(tf_ic_satdv), "");
}


void init_itinerary_calculator(GtkBuilder *builder) {
	tf_ic_window = gtk_builder_get_object(builder, "window");
	lb_ic_ksp_note = GTK_WIDGET(gtk_builder_get_object(builder, "lb_ic_ksp_note"));
	cb_ic_system = gtk_builder_get_object(builder, "cb_ic_system");
	cb_ic_central_body = gtk_builder_get_object(builder, "cb_ic_central_body");
	lb_ic_central_body = gtk_builder_get_object(builder, "lb_ic_central_body");
	cb_ic_depbody = gtk_builder_get_object(builder, "cb_ic_depbody");
	cb_ic_arrbody = gtk_builder_get_object(builder, "cb_ic_arrbody");
	tf_ic_mindepdate = gtk_builder_get_object(builder, "tf_ic_mindepdate");
	tf_ic_maxdepdate = gtk_builder_get_object(builder, "tf_ic_maxdepdate");
	tf_ic_maxarrdate = gtk_builder_get_object(builder, "tf_ic_maxarrdate");
	box_ic_maxarrdate = GTK_WIDGET(gtk_builder_get_object(builder, "box_ic_maxarrdate"));
	lb_ic_date_warning = GTK_WIDGET(gtk_builder_get_object(builder, "lb_ic_date_warning"));
	bt_ic_calc = GTK_WIDGET(gtk_builder_get_object(builder, "bt_ic_calc"));
	gtk_widget_hide(lb_ic_date_warning);
	tf_ic_maxdur = gtk_builder_get_object(builder, "tf_ic_maxdur");
	rb_ic_maxarrdate = gtk_builder_get_object(builder, "rb_ic_maxarrdate");
	rb_ic_maxdur = gtk_builder_get_object(builder, "rb_ic_maxdur");
	g_signal_connect(rb_ic_maxarrdate, "toggled", G_CALLBACK(on_ic_time_limit_toggled), NULL);
	g_signal_connect(rb_ic_maxdur, "toggled", G_CALLBACK(on_ic_time_limit_toggled), NULL);
	on_ic_time_limit_toggled(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate), NULL);
	connect_ic_date_entry_validation(GTK_ENTRY(tf_ic_mindepdate));
	connect_ic_date_entry_validation(GTK_ENTRY(tf_ic_maxdepdate));
	connect_ic_date_entry_validation(GTK_ENTRY(tf_ic_maxarrdate));
	g_signal_connect(tf_ic_mindepdate, "changed", G_CALLBACK(on_ic_earliest_dep_changed), NULL);
	g_signal_connect(tf_ic_maxarrdate, "changed", G_CALLBACK(on_ic_latest_arrival_changed), NULL);
	g_signal_connect(tf_ic_maxdur, "changed", G_CALLBACK(on_ic_max_duration_changed), NULL);
	connect_date_adjust_buttons(builder, "ic_maxdep", GTK_ENTRY(tf_ic_maxdepdate));
	connect_date_adjust_buttons(builder, "ic_maxarr", GTK_ENTRY(tf_ic_maxarrdate));
	update_month_adjust_button_labels(get_settings_datetime_type());
	cb_ic_transfertype = gtk_builder_get_object(builder, "cb_ic_transfertype");
	cb_ic_allow_dsm = GTK_WIDGET(gtk_builder_get_object(builder, "cb_ic_allow_dsm"));
	if(cb_ic_allow_dsm != NULL)
		set_stable_tooltip(cb_ic_allow_dsm, "Tries one deep-space maneuver per leg on the best routes. Its delta-v counts toward the satellite delta-v limit, so raise that limit above 0 for maneuvers to appear.");
	cb_ic_allow_resonant = GTK_WIDGET(gtk_builder_get_object(builder, "cb_ic_allow_resonant"));
	if(cb_ic_allow_resonant != NULL)
		set_stable_tooltip(cb_ic_allow_resonant, "Also searches flybys that return to the same body after one or more revolutions (for example Earth-Earth). Makes the search several times slower.");
	cb_ic_moon_aware_flybys = GTK_WIDGET(gtk_builder_get_object(builder, "cb_ic_moon_aware_flybys"));
	if(cb_ic_moon_aware_flybys != NULL)
		set_stable_tooltip(cb_ic_moon_aware_flybys, "Rejects or re-plans flybys that hit or pass too close to a moon. Makes the search slower.");
	cb_ic_moon_assist_arrival = GTK_WIDGET(gtk_builder_get_object(builder, "cb_ic_moon_assist_arrival"));
	if(cb_ic_moon_assist_arrival != NULL)
		set_stable_tooltip(cb_ic_moon_assist_arrival, "Uses a moon flyby to reduce the capture or circularization burn when one is available.");
	tf_ic_depdv = gtk_builder_get_object(builder, "tf_ic_depdv");
	tf_ic_satdv = gtk_builder_get_object(builder, "tf_ic_satdv");
	vp_ic_fbbodies = gtk_builder_get_object(builder, "vp_ic_fbbodies");

	ic_system = NULL;

	create_combobox_dropdown_text_renderer(cb_ic_system, GTK_ALIGN_CENTER);
	create_combobox_dropdown_text_renderer(cb_ic_central_body, GTK_ALIGN_CENTER);
	create_combobox_dropdown_text_renderer(cb_ic_depbody, GTK_ALIGN_CENTER);
	create_combobox_dropdown_text_renderer(cb_ic_arrbody, GTK_ALIGN_CENTER);
	update_system_dropdown(GTK_COMBO_BOX(cb_ic_system));
	set_ic_default_system(get_settings_default_system());
	if(get_num_available_systems() > 0) {
		ic_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))];
		update_central_body_dropdown(GTK_COMBO_BOX(cb_ic_central_body), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_depbody), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_arrbody), ic_system);
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
	}
	g_signal_connect(cb_ic_depbody, "changed", G_CALLBACK(on_ic_reference_input_changed), NULL);
	g_signal_connect(cb_ic_arrbody, "changed", G_CALLBACK(on_ic_reference_input_changed), NULL);
	g_signal_connect(cb_ic_transfertype, "changed", G_CALLBACK(on_ic_reference_input_changed), NULL);
	update_ic_reference_defaults();
	ic_initialized = TRUE;
}

void set_ic_default_system(char *system_name) {
	if(cb_ic_system == NULL || system_name == NULL) return;
	CelestSystem *default_system = get_system_by_name(system_name);
	for(int i = 0; i < get_num_available_systems(); i++) {
		if(get_available_systems()[i] == default_system) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(cb_ic_system), i);
			break;
		}
	}
}

int ic_select_ksp_start(CelestSystem *system, Body *departure_body, double departure_jd) {
	if(system == NULL || departure_body == NULL || !isfinite(departure_jd)) return 0;
	CelestSystem *top_level = get_top_level_system(system);
	int system_index = -1, central_body_index = -1;
	for(int i = 0; i < get_num_available_systems(); i++)
		if(get_available_systems()[i] == top_level) { system_index = i; break; }
	if(system_index < 0 || departure_body->orbit.cb == NULL) return 0;
	for(int i = 0; i <= get_number_of_subsystems(top_level); i++) {
		CelestSystem *subsystem = get_subsystem_from_system_and_id(top_level, i);
		if(subsystem != NULL && subsystem->cb == departure_body->orbit.cb) {
			central_body_index = i;
			ic_system = subsystem;
			break;
		}
	}
	if(central_body_index < 0) return 0;
	gtk_combo_box_set_active(GTK_COMBO_BOX(cb_ic_system), system_index);
	gtk_combo_box_set_active(GTK_COMBO_BOX(cb_ic_central_body), central_body_index);
	int departure_index = get_body_system_id(departure_body, ic_system);
	if(departure_index < 0) return 0;
	gtk_combo_box_set_active(GTK_COMBO_BOX(cb_ic_depbody), departure_index);
	char date_text[32];
	date_to_string(convert_JD_date(departure_jd, get_settings_datetime_type()), date_text, 0);
	ic_setting_ksp_start_date = TRUE;
	gtk_entry_set_text(GTK_ENTRY(tf_ic_mindepdate), date_text);
	ic_setting_ksp_start_date = FALSE;
	ic_ksp_start_jd = departure_jd;
	ic_ksp_start_body = departure_body;
	ic_ksp_start_active = TRUE;
	return 1;
}

void ic_set_ksp_note(const char *text) {
	if(lb_ic_ksp_note == NULL) return;
	gtk_label_set_text(GTK_LABEL(lb_ic_ksp_note), text != NULL ? text : "");
	gtk_widget_set_visible(lb_ic_ksp_note, text != NULL && text[0] != 0);
}

void ic_clear_ksp_start(void) {
	ic_ksp_start_active = FALSE;
	ic_ksp_start_body = NULL;
}

void ic_change_date_type(enum DateType old_date_type, enum DateType new_date_type) {
	ic_changing_date_type = TRUE;
	change_text_field_date_type(tf_ic_mindepdate, old_date_type, new_date_type);
	change_text_field_date_type(tf_ic_maxdepdate, old_date_type, new_date_type);
	change_text_field_date_type(tf_ic_maxarrdate, old_date_type, new_date_type);
	ic_changing_date_type = FALSE;
	update_month_adjust_button_labels(new_date_type);
	validate_ic_date_range_widgets();
}


GtkWidget * ic_update_seq_body_grid(GObject *viewport, GtkWidget *grid) {
	if (grid != NULL && GTK_WIDGET(viewport) == gtk_widget_get_parent(grid)) {
		gtk_container_remove(GTK_CONTAINER(viewport), grid);
	}

	grid = gtk_grid_new();
	int departure_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody));
	int arrival_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody));
	int first_route_index = MIN(departure_index, arrival_index);
	int last_route_index = MAX(departure_index, arrival_index);

	// Create labels and buttons and add them to the grid
	for (int body_idx = 0; body_idx < ic_system->num_bodies; body_idx++) {
		int row = body_idx;
		GtkWidget *widget;
		// Create a show body check button
		widget = gtk_check_button_new_with_label(ic_system->bodies[body_idx]->name);
		gboolean is_endpoint = body_idx == departure_index || body_idx == arrival_index;
		const char *body_name = ic_system->bodies[body_idx]->name;
		gboolean is_default_excluded = g_ascii_strcasecmp(body_name, "Vesta") == 0 ||
			g_ascii_strcasecmp(body_name, "Ceres") == 0;
		gboolean is_between_endpoints = departure_index >= 0 && arrival_index >= 0 &&
			body_idx >= first_route_index && body_idx <= last_route_index && !is_default_excluded;
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), is_between_endpoints);
		gtk_widget_set_sensitive(widget, !is_endpoint);
		gtk_widget_set_halign(widget, GTK_ALIGN_START);

		// Set the label in the grid at the specified row and column
		gtk_grid_attach(GTK_GRID(grid), widget, 0, row, 1, 1);
	}
	gtk_container_add (GTK_CONTAINER (viewport), grid);
	gtk_widget_show_all(GTK_WIDGET(viewport));

	return grid;
}

int get_num_selected_bodies_from_grid(GtkWidget *grid) {
	int num_bodies = 0;
	for(int i = 0; i < ic_system->num_bodies; i++) {
		if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gtk_grid_get_child_at(GTK_GRID(grid), 0, i))))
			num_bodies++;
	}
	return num_bodies;
}

Body ** get_bodies_from_grid(GtkWidget *grid, int num_bodies) {
	Body **bodies = malloc(num_bodies * sizeof(Body*));
	int idx = 0;
	for(int i = 0; i < ic_system->num_bodies; i++) {
		if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gtk_grid_get_child_at(GTK_GRID(grid), 0, i)))) {
			bodies[idx] = ic_system->bodies[i]; idx++;
		}
	}

	return bodies;
}


G_MODULE_EXPORT void on_ic_system_change() {
	ic_close_date_picker();
	if(get_num_available_systems() > 0) {
		ic_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))];
		if(ic_initialized) update_datetime_type_for_system(ic_system);
		update_central_body_dropdown(GTK_COMBO_BOX(cb_ic_central_body), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_depbody), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_arrbody), ic_system);
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
		update_ic_reference_defaults();
	}
}

G_MODULE_EXPORT void on_ic_central_body_change() {
	if(get_num_available_systems() > 0) {
		if(get_number_of_subsystems(get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))]) == 0) {
			gtk_widget_set_sensitive(GTK_WIDGET(cb_ic_central_body), 0);
			return;
		}
		gtk_widget_set_sensitive(GTK_WIDGET(cb_ic_central_body), 1);
		CelestSystem *ic_og_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))];
		ic_system = get_subsystem_from_system_and_id(ic_og_system, gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_central_body)));
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_depbody), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_arrbody), ic_system);
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
		update_ic_reference_defaults();
	}
}

G_MODULE_EXPORT void on_get_ic_ref_values() {
	if(ic_system == NULL || !update_ic_reference_defaults()) return;
	int departure_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody));
	int arrival_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody));
	Body *dep_body = ic_system->bodies[departure_index];
	Body *arr_body = ic_system->bodies[arrival_index];
	ItineraryHohmannReference reference;
	if(!calculate_itinerary_hohmann_reference(dep_body, arr_body, ic_system->cb,
		ic_dep_periapsis, ic_arr_periapsis, &reference)) return;
	double duration = reference.duration_seconds /
		(get_settings_datetime_type() == DATE_KERBAL ? 6 * 60 * 60 : 24 * 60 * 60);

	char msg[256];
	sprintf(msg, "Hohmann Transfer from %s to %s \n(from %.3E km to %.3E km circular orbit):\n\n"
				 "Departure dv: %.0f m/s\n"
				 "Arrival Capture dv: %.0f m/s\n"
				 "Arrival Circularization: %.0f m/s\n"
				 "Duration: %.0f days",
			dep_body->name, arr_body->name, dep_body->orbit.a/1e3, arr_body->orbit.a/1e3,
			reference.departure_dv, reference.arrival_capture_dv, reference.arrival_circularization_dv, duration);
	show_msg_window(msg);
}

void reset_ic() {

}