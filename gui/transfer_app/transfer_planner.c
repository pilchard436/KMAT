#include "transfer_planner_internal.h"
#include "gui/css_loader.h"
#include "orbit_calculator/flyby_policy.h"

#include <string.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <math.h>


struct ItinStep *curr_transfer_tp;

CelestSystem *tp_system;

Camera *tp_system_camera;

GObject *da_tp;
GObject *cb_tp_system;
GObject *cb_tp_tfbody;
GObject *lb_tp_date;
GObject *tb_tp_tfdate;
GObject *bt_tp_10y6hp;
GObject *bt_tp_10y6hm;
GObject *bt_tp_1y1hp;
GObject *bt_tp_1y1hm;
GObject *bt_tp_1m30d10minp;
GObject *bt_tp_1m30d10minm;
GObject *bt_tp_1d1minp;
GObject *bt_tp_1d1minm;
GObject *lb_tp_transfer_dv;
GObject *lb_tp_total_dv;
GObject *lb_tp_dv_warning;
GObject *lb_tp_param_labels;
GObject *lb_tp_param_values;
GObject *vp_tp_bodies;
GtkWidget *grid_tp_bodies;

// Set while the system dropdown has an extra entry for the system of a loaded itinerary.
int tp_loaded_system_entry;

GtkWidget *tb_tp_animate, *cb_tp_anim_speed;

gboolean *body_show_status_tp, fix_tfbody;
double current_date_tp;

enum LastTransferType tp_last_transfer_type;
int tp_last_transfer_type_locked;
int tp_setting_last_transfer_type;
GtkWidget *tp_transfer_type_buttons[2];
ItinSearchOptions tp_search_options;

double tp_dep_periapsis = 50e3;
double tp_arr_periapsis = 50e3;


void init_transfer_planner(GtkBuilder *builder) {
	struct Datetime date = {1950, 1, 1, 0, 0, 0};
	current_date_tp = convert_date_JD(date);

	remove_all_transfers();
	tp_last_transfer_type = TF_FLYBY;

	cb_tp_system = gtk_builder_get_object(builder, "cb_tp_system");
	cb_tp_tfbody = gtk_builder_get_object(builder, "cb_tp_tfbody");
	tb_tp_tfdate = gtk_builder_get_object(builder, "tb_tp_tfdate");
	bt_tp_10y6hp = gtk_builder_get_object(builder, "bt_tp_10y6hp");
	bt_tp_10y6hm = gtk_builder_get_object(builder, "bt_tp_10y6hm");
	bt_tp_1y1hp = gtk_builder_get_object(builder, "bt_tp_1y1hp");
	bt_tp_1y1hm = gtk_builder_get_object(builder, "bt_tp_1y1hm");
	bt_tp_1m30d10minp = gtk_builder_get_object(builder, "bt_tp_1m30d10minp");
	bt_tp_1m30d10minm = gtk_builder_get_object(builder, "bt_tp_1m30d10minm");
	bt_tp_1d1minp = gtk_builder_get_object(builder, "bt_tp_1d1minp");
	bt_tp_1d1minm = gtk_builder_get_object(builder, "bt_tp_1d1minm");
	set_stable_tooltip(GTK_WIDGET(gtk_builder_get_object(builder, "bt_tp_send_ksp_correction")),
		"Adds a small node on the current trajectory that restores the planned encounter periapsis");
	lb_tp_transfer_dv = gtk_builder_get_object(builder, "lb_tp_transfer_dv");
	lb_tp_total_dv = gtk_builder_get_object(builder, "lb_tp_total_dv");
	lb_tp_dv_warning = gtk_builder_get_object(builder, "lb_tp_dv_warning");
	lb_tp_param_labels = gtk_builder_get_object(builder, "lb_tp_param_labels");
	lb_tp_param_values = gtk_builder_get_object(builder, "lb_tp_param_values");
	lb_tp_date = gtk_builder_get_object(builder, "lb_tp_date");
	da_tp = gtk_builder_get_object(builder, "da_tp");
	vp_tp_bodies = gtk_builder_get_object(builder, "vp_tp_bodies");
	tp_transfer_type_buttons[TF_FLYBY] = GTK_WIDGET(gtk_builder_get_object(builder, "rb_tp_fb"));
	tp_transfer_type_buttons[TF_CAPTURE] = GTK_WIDGET(gtk_builder_get_object(builder, "rb_tp_cap"));
	tb_tp_animate = GTK_WIDGET(gtk_builder_get_object(builder, "tb_tp_animate"));
	cb_tp_anim_speed = GTK_WIDGET(gtk_builder_get_object(builder, "cb_tp_anim_speed"));
	tp_set_last_transfer_type(TF_FLYBY, 0);

	tp_system_camera = new_camera(GTK_WIDGET(da_tp), &on_tp_screen_resize, &on_enable_camera_rotation, &on_disable_camera_rotation, &on_tp_screen_mouse_move, &on_tp_screen_scroll);
	g_signal_connect(da_tp, "button-press-event", G_CALLBACK(on_tp_screen_button_press), NULL);
	g_signal_connect(da_tp, "destroy", G_CALLBACK(on_tp_view_destroyed), NULL);


	tp_system = NULL;
	curr_transfer_tp = NULL;

	fix_tfbody = TRUE;

	create_combobox_dropdown_text_renderer(cb_tp_system, GTK_ALIGN_CENTER);
	create_combobox_dropdown_text_renderer(cb_tp_tfbody, GTK_ALIGN_CENTER);
	if(get_num_available_systems() > 0) {
		update_system_dropdown(GTK_COMBO_BOX(cb_tp_system));
		tp_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_system))];
		tp_update_bodies();

		update_camera_to_celestial_system(tp_system_camera, tp_system, deg2rad(90), 0);
	}

	update_date_label();
	update_transfer_panel();
}

void tp_set_last_transfer_type(enum LastTransferType type, int locked) {
	if(type == TF_CIRC) type = TF_CAPTURE;
	if(type < TF_FLYBY || type > TF_CAPTURE) return;
	tp_last_transfer_type = type;
	tp_last_transfer_type_locked = !!locked;
	tp_setting_last_transfer_type = 1;
	for(int i = 0; i < 2; i++) {
		if(tp_transfer_type_buttons[i] == NULL) continue;
		if(i == type)
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tp_transfer_type_buttons[i]), TRUE);
		gtk_widget_set_sensitive(tp_transfer_type_buttons[i], !locked || i == type);
	}
	tp_setting_last_transfer_type = 0;
}


// -------------------------------------------------------------------------------------


G_MODULE_EXPORT void on_tp_system_change() {
	if(get_num_available_systems() == 0 ||
			gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_system)) == get_num_available_systems() ||
			tp_system == get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_system))] ||
			gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_system)) == -1) return;
	
	if(!is_available_system(get_top_level_system(tp_system)) && tp_system != NULL) {
		free_celestial_system(get_top_level_system(tp_system));
		tp_system = NULL;
		if(tp_loaded_system_entry) remove_combobox_last_entry(GTK_COMBO_BOX(cb_tp_system));
		tp_loaded_system_entry = 0;
	}

	tp_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_system))];
	update_camera_to_celestial_system(tp_system_camera, tp_system, deg2rad(90), 0); tp_focus_body = NULL; tp_focus_spacecraft = 0;
	remove_all_transfers();
	tp_update_bodies();
	update();
}

void tp_select_system(int index) {
	if(cb_tp_system != NULL && index >= 0 && index < get_num_available_systems())
		gtk_combo_box_set_active(GTK_COMBO_BOX(cb_tp_system), index);
}


// Set when the flyby iteration did not settle and the shown delta-v is only the last iterate.
int tp_route_unconverged;
// Identifies the itinerary details text in the shared message window.
char tp_details_owner;

double calc_step_dv(struct ItinStep *step) {
	return itinerary_step_dv(step, tp_dep_periapsis, tp_arr_periapsis, tp_last_transfer_type);
}

double calc_total_dv() {
	if(curr_transfer_tp == NULL || !is_valid_itinerary(get_last(curr_transfer_tp))) return 0;
	double dv = 0;
	struct ItinStep *step = get_last(curr_transfer_tp);
	while(step != NULL) {
		dv += calc_step_dv(step);
		step = step->prev;
	}
	return dv;
}

double calc_periapsis_height_tp() {
	if(curr_transfer_tp->body == NULL) return -1e20;
	if(curr_transfer_tp->next == NULL || curr_transfer_tp->prev == NULL) return -1e20;
	PoweredFlybyResult flyby;
	if(calculate_flyby_with_moons(curr_transfer_tp->v_arr, curr_transfer_tp->next[0]->v_dep,
		curr_transfer_tp->v_body, curr_transfer_tp->body, flyby_minimum_periapsis(curr_transfer_tp->body),
		curr_transfer_tp->date, &flyby))
		return (flyby.periapsis_radius - curr_transfer_tp->body->radius) * 1e-3;
	double rp = get_flyby_periapsis(curr_transfer_tp->v_arr, curr_transfer_tp->next[0]->v_dep, curr_transfer_tp->v_body, curr_transfer_tp->body);
	return (rp-curr_transfer_tp->body->radius)*1e-3;
}

// Direct routes have no flyby, so plain Lambert arcs are exact; routes with flybys keep their stored velocities
// if even the approximate powered-flyby re-solve fails, because patched-conic arcs would misstate the flyby burns.
int tp_resolve_route(void) {
	struct ItinStep *first = get_first(curr_transfer_tp);
	update_itin_body_osvs(first, tp_system);
	int encounters = 0;
	for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) encounters += node->body != NULL;
	if(encounters < 3) {
		calc_itin_v_vectors_from_dates_and_r(first, tp_system);
		tp_route_unconverged = 0;
		return 1;
	}
	int converged = 1;
	int resolved = resolve_itinerary_with_powered_flybys_approximate(tp_system, first, tp_dep_periapsis, tp_arr_periapsis, tp_last_transfer_type, &converged);
	if(resolved) tp_route_unconverged = !converged;
	return resolved;
}

void update_itinerary() {
	if(curr_transfer_tp != NULL) {
		tp_dep_periapsis = get_first(curr_transfer_tp)->body->atmo_alt + 50e3;
		tp_arr_periapsis = get_last(curr_transfer_tp)->body->atmo_alt + 50e3;
		tp_resolve_route();
	}
	update();
}

void sort_transfer_dates(struct ItinStep *tf) {
	if(tf == NULL) return;
	while(tf->next != NULL) {
		if(tf->next[0]->date <= tf->date) tf->next[0]->date = tf->date+1;
		tf = tf->next[0];
	}
	update_itinerary();
}

void remove_all_transfers() {
	if(curr_transfer_tp != NULL) {
		free_itinerary(get_first(curr_transfer_tp));
		curr_transfer_tp = NULL;
	}
	tp_search_options = (ItinSearchOptions) {.starting_orbit_body_id = -1, .last_transfer_type = -1};
	set_moon_assist_enabled(0);
	set_moon_aware_flybys_enabled(0);
	starting_orbit_set_active(NULL, NULL);
	tp_set_last_transfer_type(TF_FLYBY, 0);
}

void update() {
	update_date_label();
	update_transfer_panel();
	update_tp_system_view();
	// Keep an open itinerary details window in step with the route.
	if(curr_transfer_tp != NULL && msg_window_shows_owner(&tp_details_owner)) {
		char text[10000];
		itinerary_detailed_overview_to_string(curr_transfer_tp, get_settings_datetime_type(), tp_dep_periapsis, tp_arr_periapsis, text);
		update_msg_window_if_owned(text, &tp_details_owner);
	}
}


void end_transfer_planner() {
	tp_stop_animation();
	remove_all_transfers();
	if(body_show_status_tp != NULL) free(body_show_status_tp);
	destroy_camera(tp_system_camera);
}