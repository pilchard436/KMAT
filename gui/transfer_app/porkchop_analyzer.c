#include "porkchop_analyzer_internal.h"
#include "gui/css_loader.h"

#include <locale.h>
#include <math.h>
#include <string.h>

int pa_num_deps, pa_num_itins;
struct ItinStep **pa_departures;
enum LastTransferType pa_last_transfer_type;
GObject *da_pa_porkchop, *da_pa_preview;
struct ItinStep *curr_transfer_pa;
double current_date_pa;
int pa_num_groups = 0;
struct PorkchopGroup *pa_groups;
struct PorkchopAnalyzerPoint *pa_porkchop_points;

ItinStepBinHeaderData pa_analysis_params;

CelestSystem *pa_system;

double pa_min_vals[6], pa_max_vals[6];


enum PaYAxisType pa_yaxis_type = PA_YAXIS_DUR;
double pa_dep_periapsis = 50e3;
double pa_arr_periapsis = 50e3;

Screen *pa_porkchop_screen;
Camera *pa_itin_preview_camera;

gboolean *body_show_status_pa;
GObject *tf_pa_min_feedback[6];
GObject *tf_pa_max_feedback[6];
GObject *lb_pa_tfdate;
GObject *lb_pa_tfbody;
GObject *lb_pa_transfer_dv;
GObject *lb_pa_total_dv;
GObject *lb_pa_periapsis;
GObject *st_pa_step_group_selector;
GObject *vp_pa_groups;
GtkWidget *grid_pa_groups;

double calc_step_dv_pa(struct ItinStep *step);
double calc_total_dv_pa(void);
double calc_periapsis_height_pa(void);

void update_pa();
void on_pa_screen_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer *ptr);
void on_pa_screen_resize(GtkWidget *widget, cairo_t *cr, gpointer *ptr);
void on_pa_screen_mouse_move(GtkWidget *widget, GdkEventButton *event, gpointer *ptr);
void on_pa_screen_button_release(GtkWidget *widget, GdkEventButton *event, gpointer *ptr);


void init_porkchop_analyzer(GtkBuilder *builder) {
	pa_analysis_params.num_deps = 0;
	pa_num_deps = 0;
	pa_num_itins = 0;
	pa_departures = NULL;
	pa_porkchop_points = NULL;
	curr_transfer_pa = NULL;
	pa_last_transfer_type = TF_FLYBY;
	da_pa_porkchop = gtk_builder_get_object(builder, "da_pa_porkchop");
	da_pa_preview = gtk_builder_get_object(builder, "da_pa_preview");
	tf_pa_min_feedback[PA_DEP] = gtk_builder_get_object(builder, "tf_pa_min_depdate");
	tf_pa_min_feedback[PA_ARR] = gtk_builder_get_object(builder, "tf_pa_min_arrdate");
	tf_pa_min_feedback[PA_DUR] = gtk_builder_get_object(builder, "tf_pa_min_dur");
	tf_pa_min_feedback[PA_TOTDV] = gtk_builder_get_object(builder, "tf_pa_min_totdv");
	tf_pa_min_feedback[PA_DEPDV] = gtk_builder_get_object(builder, "tf_pa_min_depdv");
	tf_pa_min_feedback[PA_SATDV] = gtk_builder_get_object(builder, "tf_pa_min_satdv");
	tf_pa_max_feedback[PA_DEP] = gtk_builder_get_object(builder, "tf_pa_max_depdate");
	tf_pa_max_feedback[PA_ARR] = gtk_builder_get_object(builder, "tf_pa_max_arrdate");
	tf_pa_max_feedback[PA_DUR] = gtk_builder_get_object(builder, "tf_pa_max_dur");
	tf_pa_max_feedback[PA_TOTDV] = gtk_builder_get_object(builder, "tf_pa_max_totdv");
	tf_pa_max_feedback[PA_DEPDV] = gtk_builder_get_object(builder, "tf_pa_max_depdv");
	tf_pa_max_feedback[PA_SATDV] = gtk_builder_get_object(builder, "tf_pa_max_satdv");
	lb_pa_tfdate = gtk_builder_get_object(builder, "lb_pa_tfdate");
	lb_pa_tfbody = gtk_builder_get_object(builder, "lb_pa_tfbody");
	lb_pa_transfer_dv = gtk_builder_get_object(builder, "lb_pa_transfer_dv");
	lb_pa_total_dv = gtk_builder_get_object(builder, "lb_pa_total_dv");
	lb_pa_periapsis = gtk_builder_get_object(builder, "lb_pa_periapsis");
	st_pa_step_group_selector = gtk_builder_get_object(builder, "st_pa_step_group_selector");
	vp_pa_groups = gtk_builder_get_object(builder, "vp_pa_groups");


	pa_porkchop_screen = new_screen(GTK_WIDGET(da_pa_porkchop), &on_pa_screen_resize, &on_screen_button_press, &on_pa_screen_button_release, &on_pa_screen_mouse_move, NULL);
	set_screen_background_color(pa_porkchop_screen, 0.15, 0.15, 0.15);
	pa_itin_preview_camera = new_camera(GTK_WIDGET(da_pa_preview), &on_pa_screen_resize, &on_enable_camera_rotation, &on_disable_camera_rotation, &on_pa_screen_mouse_move, &on_pa_screen_scroll);

	pa_system = NULL;
}


// ITINERARY PREVIEW AND PORKCHOP CALLBACKS -----------------------------------------------
void update_pa_porkchop_diagram() {
	clear_screen(pa_porkchop_screen);

	if(pa_porkchop_points != NULL) draw_porkchop(pa_porkchop_screen->static_layer.cr, pa_porkchop_screen->width, pa_porkchop_screen->height, pa_porkchop_points, pa_num_itins, pa_last_transfer_type, pa_yaxis_type);
	draw_screen(pa_porkchop_screen);
}

void update_pa_itinerary_preview() {
	clear_camera_screen(pa_itin_preview_camera);

	if(pa_system == NULL || body_show_status_pa == NULL) return;
	char date_text[32];
	date_to_string(convert_JD_date(current_date_pa, get_settings_datetime_type()), date_text, 0);
	gtk_label_set_text(GTK_LABEL(lb_pa_tfdate), date_text);
	if(curr_transfer_pa != NULL) {
		gtk_label_set_text(GTK_LABEL(lb_pa_tfbody),
			curr_transfer_pa->body != NULL ? curr_transfer_pa->body->name : "Deep Space Maneuver");
		char value_text[48];
		g_snprintf(value_text, sizeof(value_text), "%.0f m/s", calc_step_dv_pa(curr_transfer_pa));
		gtk_label_set_text(GTK_LABEL(lb_pa_transfer_dv), value_text);
		g_snprintf(value_text, sizeof(value_text), "%.0f m/s", calc_total_dv_pa());
		gtk_label_set_text(GTK_LABEL(lb_pa_total_dv), value_text);
		double periapsis_height = calc_periapsis_height_pa();
		if(periapsis_height < -1e10)
			gtk_label_set_text(GTK_LABEL(lb_pa_periapsis), "- km");
		else {
			g_snprintf(value_text, sizeof(value_text), "%.2f km", periapsis_height);
			gtk_label_set_text(GTK_LABEL(lb_pa_periapsis), value_text);
		}
	}

	register_body_occluders(pa_system, current_date_pa);
	// Sun
	draw_body(pa_itin_preview_camera, pa_system, pa_system->cb, current_date_pa);
	// Planets
	for(int i = 0; i < pa_system->num_bodies; i++) {
		if(!body_show_status_pa[i]) continue;
		draw_body(pa_itin_preview_camera, pa_system, pa_system->bodies[i], current_date_pa);
		struct Orbit orbit = pa_system->bodies[i]->orbit;
		if(pa_system->prop_method == EPHEMS) {
			struct OSV body_osv = osv_from_ephem(pa_system->bodies[i]->ephem, pa_system->bodies[i]->num_ephems, current_date_pa, pa_system->cb);
			orbit = constr_orbit_from_osv(body_osv.r, body_osv.v, pa_system->cb);
		}
		draw_orbit(pa_itin_preview_camera, orbit);
	}
	// Transfers
	if(curr_transfer_pa != NULL) draw_itinerary(pa_itin_preview_camera, pa_system, curr_transfer_pa, current_date_pa);

	draw_camera_image(pa_itin_preview_camera);
}

void on_pa_screen_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer *ptr) {
	if((Camera*)ptr == pa_itin_preview_camera) {
		on_camera_zoom(widget, event, pa_itin_preview_camera);
		update_pa_itinerary_preview();
	}
}

void on_pa_screen_resize(GtkWidget *widget, cairo_t *cr, gpointer *ptr) {
	if((Screen*)ptr == pa_porkchop_screen) {
		resize_screen(pa_porkchop_screen);
		update_pa_porkchop_diagram();
	}
	if((Camera*)ptr == pa_itin_preview_camera) {
		resize_camera_screen(pa_itin_preview_camera);
		update_pa_itinerary_preview();
	}
}

void on_pa_screen_mouse_move(GtkWidget *widget, GdkEventButton *event, gpointer *ptr) {
	if((Screen*)ptr == pa_porkchop_screen) {
		if(pa_system == NULL) return;
		int min_x = pa_yaxis_type == PA_YAXIS_ARRDATE ? get_porkchop_arrdate_yaxis_x() : get_porkchop_dur_yaxis_x();
		int min_y = get_porkchop_xaxis_y();
		if(pa_porkchop_screen->dragging) {
			double x = pa_porkchop_screen->mouse_pos_on_press.x;
			double y = pa_porkchop_screen->mouse_pos_on_press.y;
			double width = event->x-pa_porkchop_screen->mouse_pos_on_press.x;
			double height = event->y-pa_porkchop_screen->mouse_pos_on_press.y;
			if(x < min_x || y > pa_porkchop_screen->height-min_y) return;
			if(x+width < min_x) width = min_x-x;
			if(y+height < 0) height = -y;
			if(x+width > pa_porkchop_screen->width) width = pa_porkchop_screen->width-x;
			if(y+height > pa_porkchop_screen->height-min_y) height = (pa_porkchop_screen->height-min_y)-y;
			clear_dynamic_screen_layer(pa_porkchop_screen);
			cairo_rectangle(pa_porkchop_screen->dynamic_layer.cr, x, y, width, height);
			cairo_set_source_rgba(pa_porkchop_screen->dynamic_layer.cr, 1, 0, 0, 1);
			cairo_stroke(pa_porkchop_screen->dynamic_layer.cr);
			draw_screen(pa_porkchop_screen);
		}
	}
	if((Camera*)ptr == pa_itin_preview_camera && pa_itin_preview_camera->rotation_sensitive) {
		on_camera_rotate(pa_itin_preview_camera, event);
		resize_camera_screen(pa_itin_preview_camera);
		update_pa_itinerary_preview();
	}
}

void on_pa_screen_button_release(GtkWidget *widget, GdkEventButton *event, gpointer *ptr) {
	if((Screen*)ptr == pa_porkchop_screen) {
		pa_porkchop_screen->dragging = FALSE;

		clear_dynamic_screen_layer(pa_porkchop_screen);

		if(pa_system == NULL) return;

		double x0 = pa_porkchop_screen->mouse_pos_on_press.x;
		double y0 = pa_porkchop_screen->mouse_pos_on_press.y;
		double x1 = event->x;
		double y1 = event->y;

		if(x0 < 45 || y0 > pa_porkchop_screen->height-40) return;
		get_min_max_dep_arr_dur_range_from_mouse_rect(&x0, &x1, &y0, &y1, pa_min_vals[PA_DEP], pa_max_vals[PA_DEP],
													  pa_min_vals[pa_yaxis_type == PA_YAXIS_DUR ? PA_DUR : PA_ARR],
													  pa_max_vals[pa_yaxis_type == PA_YAXIS_DUR ? PA_DUR : PA_ARR],
													  pa_porkchop_screen->width, pa_porkchop_screen->height,
													  pa_yaxis_type);

		char string[20];
		date_to_string(convert_JD_date(x0, get_settings_datetime_type()), string, 0);
		gtk_entry_set_text(GTK_ENTRY(tf_pa_min_feedback[PA_DEP]), string);
		date_to_string(convert_JD_date(x1, get_settings_datetime_type()), string, 0);
		gtk_entry_set_text(GTK_ENTRY(tf_pa_max_feedback[PA_DEP]), string);
		
		if(pa_yaxis_type == PA_YAXIS_ARRDATE) {
			date_to_string(convert_JD_date(y0, get_settings_datetime_type()), string, 0);
			gtk_entry_set_text(GTK_ENTRY(tf_pa_min_feedback[PA_ARR]), string);
			date_to_string(convert_JD_date(y1, get_settings_datetime_type()), string, 0);
			gtk_entry_set_text(GTK_ENTRY(tf_pa_max_feedback[PA_ARR]), string);
		} else {
			sprintf(string, "%.0f", get_settings_datetime_type() == DATE_KERBAL ? y0*4 : y0);
			gtk_entry_set_text(GTK_ENTRY(tf_pa_min_feedback[PA_DUR]), string);
			sprintf(string, "%.0f", get_settings_datetime_type() == DATE_KERBAL ? y1*4 : y1);
			gtk_entry_set_text(GTK_ENTRY(tf_pa_max_feedback[PA_DUR]), string);
		}

		on_apply_filter(NULL, NULL);

		draw_screen(pa_porkchop_screen);
	}
}

// -------------------------------------------------------------------------------------


void pa_change_date_type(enum DateType old_date_type, enum DateType new_date_type) {
	change_text_field_date_type(tf_pa_min_feedback[PA_DEP], old_date_type, new_date_type);
	change_text_field_date_type(tf_pa_max_feedback[PA_DEP], old_date_type, new_date_type);
	change_text_field_date_type(tf_pa_min_feedback[PA_ARR], old_date_type, new_date_type);
	change_text_field_date_type(tf_pa_max_feedback[PA_ARR], old_date_type, new_date_type);
	if(curr_transfer_pa != NULL) change_label_date_type(lb_pa_tfdate, old_date_type, new_date_type);
	else if(new_date_type == DATE_ISO) gtk_label_set_text(GTK_LABEL(lb_pa_tfdate), "0000-00-00");
	else gtk_label_set_text(GTK_LABEL(lb_pa_tfdate), "0000-000");
	update_pa();
}

double calc_step_dv_pa(struct ItinStep *step) {
	if(step == NULL) return 0;
	struct ItinStep *departure = get_first(step), *arrival = get_last(step);
	return itinerary_step_dv(step, departure->body->atmo_alt + pa_dep_periapsis,
		arrival->body->atmo_alt + pa_arr_periapsis, pa_last_transfer_type);
}

double calc_total_dv_pa() {
	if(curr_transfer_pa == NULL) return 0;
	double dv = 0;
	struct ItinStep *step = get_last(curr_transfer_pa);
	while(step != NULL) {
		dv += calc_step_dv_pa(step);
		step = step->prev;
	}
	return dv;
}

double calc_periapsis_height_pa() {
	if(curr_transfer_pa->body == NULL) return -1e20;
	if(curr_transfer_pa->next == NULL || curr_transfer_pa->prev == NULL) return -1e20;
	double rp = get_flyby_periapsis(curr_transfer_pa->v_arr, curr_transfer_pa->next[0]->v_dep, curr_transfer_pa->v_body, curr_transfer_pa->body);
	return (rp-curr_transfer_pa->body->radius)*1e-3;
}


void pa_update_body_show_status() {
	if(pa_system == NULL || body_show_status_pa == NULL) return;
	mark_itinerary_bodies_visible(body_show_status_pa, pa_system->num_bodies, get_last(curr_transfer_pa), pa_system);
}


G_MODULE_EXPORT void on_pa_switch_y_axis_type(GtkWidget* widget, gpointer data) {
	pa_yaxis_type = pa_yaxis_type != PA_YAXIS_DUR ? PA_YAXIS_DUR : PA_YAXIS_ARRDATE;
	update_pa();
}


G_MODULE_EXPORT void on_last_transfer_type_changed_pa(GtkWidget* widget, gpointer data) {
	if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) return;
	const char *name = gtk_widget_get_name(widget);
	if		(strcmp(name, "fb") == 0) pa_last_transfer_type = TF_FLYBY;
	else if	(strcmp(name, "capture") == 0) pa_last_transfer_type = TF_CAPTURE;
	else if	(strcmp(name, "circ") == 0) pa_last_transfer_type = TF_CIRC;

	if(pa_porkchop_points == NULL) return;

	sort_porkchop(pa_porkchop_points, pa_num_itins, pa_last_transfer_type);
	update_best_itin();
	update_pa_porkchop_diagram();
	update_pa_itinerary_preview();
	reset_min_max_feedback(1);
}

void update_pa() {
	update_best_itin();
	update_pa_porkchop_diagram();
	update_pa_itinerary_preview();
	update_group_overview();
	reset_min_max_feedback(1);
}


G_MODULE_EXPORT void on_pa_update(GtkWidget* widget, gpointer data) {
	if(pa_porkchop_points == NULL) return;
	reset_min_max_feedback(0);
	apply_filter();
	update_pa();
}


G_MODULE_EXPORT void on_prev_transfer_pa(GtkWidget* widget, gpointer data) {
	if(curr_transfer_pa == NULL) return;
	if(curr_transfer_pa->prev != NULL) curr_transfer_pa = curr_transfer_pa->prev;
	current_date_pa = curr_transfer_pa->date;
	update_pa_itinerary_preview();
}

G_MODULE_EXPORT void on_next_transfer_pa(GtkWidget* widget, gpointer data) {
	if(curr_transfer_pa == NULL) return;
	if(curr_transfer_pa->next != NULL) curr_transfer_pa = curr_transfer_pa->next[0];
	current_date_pa = curr_transfer_pa->date;
	update_pa_itinerary_preview();
}

G_MODULE_EXPORT void on_switch_steps_groups(GtkWidget* widget, gpointer data) {
	if(strcmp(gtk_stack_get_visible_child_name(GTK_STACK(st_pa_step_group_selector)), "page0") == 0)
		gtk_stack_set_visible_child_name(GTK_STACK(st_pa_step_group_selector), "page1");
	else
		gtk_stack_set_visible_child_name(GTK_STACK(st_pa_step_group_selector), "page0");
}

void end_porkchop_analyzer() {
	free_all_porkchop_analyzer_itins();
	destroy_camera(pa_itin_preview_camera);
	destroy_screen(pa_porkchop_screen);
}