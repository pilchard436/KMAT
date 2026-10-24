#include "porkchop_analyzer_internal.h"
#include "gui/css_loader.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int pa_num_deps, pa_num_itins;
struct ItinStep **pa_departures;
struct ItinStep **pa_dual_flyby_departures, **pa_dual_capture_departures;
int pa_dual_flyby_num_deps, pa_dual_capture_num_deps, pa_has_both_modes;
enum LastTransferType pa_last_transfer_type;
int pa_last_transfer_type_locked;
GObject *da_pa_porkchop, *da_pa_preview;
struct ItinStep *curr_transfer_pa;
struct ItinStep *pa_selected_arrival;
double current_date_pa;
int pa_num_groups = 0;
struct PorkchopGroup *pa_groups;
struct PorkchopAnalyzerPoint *pa_porkchop_points;

ItinStepBinHeaderData pa_analysis_params;

CelestSystem *pa_system;

double pa_min_vals[6], pa_max_vals[6];

static GtkWidget *pa_transfer_type_buttons[2];
static GtkWidget *pa_arrival_filter_label, *pa_duration_filter_label;
static int pa_setting_transfer_type;

enum PaYAxisType pa_yaxis_type = PA_YAXIS_DUR;
double pa_dep_periapsis = 50e3;
double pa_arr_periapsis = 50e3;

Screen *pa_porkchop_screen;
Camera *pa_itin_preview_camera;
static struct PorkchopPlotView pa_porkchop_view;
enum PaPorkchopDragMode {PA_PORKCHOP_DRAG_NONE, PA_PORKCHOP_DRAG_ZOOM, PA_PORKCHOP_DRAG_PAN};
static enum PaPorkchopDragMode pa_porkchop_drag_mode;

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
gboolean on_pa_screen_button_press(GtkWidget *widget, GdkEventButton *event, gpointer ptr);
gboolean on_pa_porkchop_key_press(GtkWidget *widget, GdkEventKey *event, gpointer ptr);
gboolean on_pa_porkchop_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer ptr);
void on_pa_screen_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer ptr);
void on_pa_screen_resize(GtkWidget *widget, cairo_t *cr, gpointer ptr);
void on_pa_screen_mouse_move(GtkWidget *widget, GdkEventButton *event, gpointer ptr);
void on_pa_screen_button_release(GtkWidget *widget, GdkEventButton *event, gpointer ptr);

static int get_pa_porkchop_plot_bounds(double *left, double *right, double *bottom) {
	if(pa_porkchop_screen == NULL) return 0;
	*left = pa_yaxis_type == PA_YAXIS_ARRDATE ? get_porkchop_arrdate_yaxis_x() : get_porkchop_dur_yaxis_x();
	*right = pa_porkchop_screen->width - get_porkchop_colorbar_width();
	*bottom = pa_porkchop_screen->height - get_porkchop_xaxis_y();
	return *right > *left && *bottom > 0;
}

static void update_pa_y_axis_filter_fields(void) {
	gboolean show_arrival = pa_yaxis_type == PA_YAXIS_ARRDATE;
	gtk_widget_set_visible(pa_arrival_filter_label, show_arrival);
	gtk_widget_set_visible(GTK_WIDGET(tf_pa_min_feedback[PA_ARR]), show_arrival);
	gtk_widget_set_visible(GTK_WIDGET(tf_pa_max_feedback[PA_ARR]), show_arrival);
	gtk_widget_set_visible(pa_duration_filter_label, !show_arrival);
	gtk_widget_set_visible(GTK_WIDGET(tf_pa_min_feedback[PA_DUR]), !show_arrival);
	gtk_widget_set_visible(GTK_WIDGET(tf_pa_max_feedback[PA_DUR]), !show_arrival);
}

static int pa_porkchop_point_inside_plot(double x, double y, double left, double right, double bottom) {
	return x >= left && x <= right && y >= 0 && y <= bottom;
}

static int pa_porkchop_value_at_pixel(double x, double y, double left, double right, double bottom,
		double *x_value, double *y_value) {
	if(!pa_porkchop_view.initialized || !porkchop_plot_range_is_valid(&pa_porkchop_view.range) ||
		right <= left || bottom <= 0) return 0;
	double x_fraction = (x - left) / (right - left);
	double y_fraction = (bottom - y) / bottom;
	return porkchop_plot_value_at_fraction(&pa_porkchop_view.range, x_fraction, y_fraction, x_value, y_value);
}

static void draw_pa_porkchop_selection_marker(void) {
	if(pa_selected_arrival == NULL || pa_porkchop_points == NULL || !pa_porkchop_view.initialized) return;
	double left, right, bottom;
	if(!get_pa_porkchop_plot_bounds(&left, &right, &bottom)) return;
	double x_span = pa_porkchop_view.range.max_x - pa_porkchop_view.range.min_x;
	double y_span = pa_porkchop_view.range.max_y - pa_porkchop_view.range.min_y;
	for(int i = 0; i < pa_num_itins; i++) {
		struct PorkchopAnalyzerPoint *point = &pa_porkchop_points[i];
		if(point->data.arrival != pa_selected_arrival || !point->inside_filter ||
			point->group == NULL || !point->group->show_group) continue;
		double y_value = pa_yaxis_type == PA_YAXIS_ARRDATE ?
				point->data.dep_date + point->data.dur : point->data.dur;
		if(point->data.dep_date < pa_porkchop_view.range.min_x ||
			point->data.dep_date > pa_porkchop_view.range.max_x ||
			y_value < pa_porkchop_view.range.min_y || y_value > pa_porkchop_view.range.max_y) return;
		double x = left + (point->data.dep_date - pa_porkchop_view.range.min_x) / x_span * (right - left);
		double y = bottom - (y_value - pa_porkchop_view.range.min_y) / y_span * bottom;
		cairo_t *cr = pa_porkchop_screen->dynamic_layer.cr;
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, 2);
		cairo_arc(cr, x, y, 6, 0, 2 * M_PI);
		cairo_stroke(cr);
		return;
	}
}

static void draw_pa_porkchop_hover(double mouse_x, double mouse_y, double left, double right, double bottom) {
	clear_dynamic_screen_layer(pa_porkchop_screen);
	draw_pa_porkchop_selection_marker();
	double x_value, y_value;
	if(!pa_porkchop_value_at_pixel(mouse_x, mouse_y, left, right, bottom, &x_value, &y_value)) {
		set_stable_tooltip(GTK_WIDGET(da_pa_porkchop),
				"Click a node to select it; use arrow keys to navigate. Left-drag to zoom, right-drag to pan, scroll to zoom.");
		draw_screen(pa_porkchop_screen);
		return;
	}

	cairo_t *cr = pa_porkchop_screen->dynamic_layer.cr;
	cairo_set_source_rgba(cr, 1.0, 0.55, 0.1, 0.9);
	cairo_set_line_width(cr, 1);
	cairo_move_to(cr, mouse_x, 0);
	cairo_line_to(cr, mouse_x, bottom);
	cairo_move_to(cr, left, mouse_y);
	cairo_line_to(cr, right, mouse_y);
	cairo_stroke(cr);

	int nearest_point_index = find_nearest_porkchop_point(pa_porkchop_points, pa_num_itins,
			pa_yaxis_type, &pa_porkchop_view.range, left, right, bottom, mouse_x, mouse_y, 12);
	int nearest_point_found = nearest_point_index >= 0;
	struct PorkchopPoint nearest_point = {0};
	if(nearest_point_found) nearest_point = pa_porkchop_points[nearest_point_index].data;

	char tooltip_text[256];
	if(nearest_point_found) {
		char departure_text[32], arrival_text[32];
		date_to_string(convert_JD_date(nearest_point.dep_date, get_settings_datetime_type()),
				departure_text, 0);
		date_to_string(convert_JD_date(nearest_point.dep_date + nearest_point.dur,
				get_settings_datetime_type()), arrival_text, 0);
		double duration = get_settings_datetime_type() == DATE_KERBAL ?
				nearest_point.dur * 4 : nearest_point.dur;
		double total_dv = porkchop_point_total_dv(nearest_point, pa_last_transfer_type);
		char dv_text[64];
		if(isfinite(total_dv)) g_snprintf(dv_text, sizeof(dv_text), "Delta-v: %.0f m/s", total_dv);
		else g_strlcpy(dv_text, "Delta-v: unavailable", sizeof(dv_text));
		g_snprintf(tooltip_text, sizeof(tooltip_text),
				"Departure: %s\nArrival: %s\nDuration: %.2f days\n%s",
				departure_text, arrival_text, duration, dv_text);
	} else {
		char departure_text[32], value_text[64];
		date_to_string(convert_JD_date(x_value, get_settings_datetime_type()), departure_text, 0);
		if(pa_yaxis_type == PA_YAXIS_ARRDATE) {
			char arrival_text[32];
			date_to_string(convert_JD_date(y_value, get_settings_datetime_type()), arrival_text, 0);
			g_snprintf(value_text, sizeof(value_text), "Arrival: %s", arrival_text);
		} else {
			double duration = get_settings_datetime_type() == DATE_KERBAL ? y_value * 4 : y_value;
			g_snprintf(value_text, sizeof(value_text), "Duration: %.2f days", duration);
		}
		g_snprintf(tooltip_text, sizeof(tooltip_text),
				"Departure: %s\n%s\nDelta-v: no nearby point", departure_text, value_text);
	}
	set_stable_tooltip_at(GTK_WIDGET(da_pa_porkchop), tooltip_text, mouse_x, mouse_y);
	draw_screen(pa_porkchop_screen);
}

void reset_pa_porkchop_view() {
	pa_porkchop_view.initialized = 0;
	pa_porkchop_drag_mode = PA_PORKCHOP_DRAG_NONE;
}

void pa_set_last_transfer_type(enum LastTransferType type, int locked) {
	if(type == TF_CIRC) type = TF_CAPTURE;
	if(type < TF_FLYBY || type > TF_CAPTURE) return;
	pa_last_transfer_type = type;
	pa_last_transfer_type_locked = !!locked;
	pa_setting_transfer_type = 1;
	for(int i = 0; i < 2; i++) {
		if(pa_transfer_type_buttons[i] == NULL) continue;
		if(i == type)
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pa_transfer_type_buttons[i]), TRUE);
		gtk_widget_set_sensitive(pa_transfer_type_buttons[i], !locked || i == type);
	}
	pa_setting_transfer_type = 0;
}


void init_porkchop_analyzer(GtkBuilder *builder) {
	pa_analysis_params.num_deps = 0;
	pa_num_deps = 0;
	pa_num_itins = 0;
	pa_departures = NULL;
	pa_dual_flyby_departures = NULL;
	pa_dual_capture_departures = NULL;
	pa_dual_flyby_num_deps = 0;
	pa_dual_capture_num_deps = 0;
	pa_has_both_modes = 0;
	pa_porkchop_points = NULL;
	curr_transfer_pa = NULL;
	pa_selected_arrival = NULL;
	reset_pa_porkchop_view();
	pa_transfer_type_buttons[TF_FLYBY] = GTK_WIDGET(gtk_builder_get_object(builder, "rb_pa_fb"));
	pa_transfer_type_buttons[TF_CAPTURE] = GTK_WIDGET(gtk_builder_get_object(builder, "rb_pa_cap"));
	pa_set_last_transfer_type(TF_FLYBY, 0);
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
	pa_arrival_filter_label = GTK_WIDGET(gtk_builder_get_object(builder, "lb_pa_arrival_filter"));
	pa_duration_filter_label = GTK_WIDGET(gtk_builder_get_object(builder, "lb_pa_duration_filter"));
	update_pa_y_axis_filter_fields();
	connect_pa_filter_entry_validation();
	for(int i = 0; i < 6; i++) {
		g_signal_connect(tf_pa_min_feedback[i], "changed", G_CALLBACK(on_pa_filter_entry_changed), NULL);
		g_signal_connect(tf_pa_max_feedback[i], "changed", G_CALLBACK(on_pa_filter_entry_changed), NULL);
	}
	lb_pa_tfdate = gtk_builder_get_object(builder, "lb_pa_tfdate");
	lb_pa_tfbody = gtk_builder_get_object(builder, "lb_pa_tfbody");
	lb_pa_transfer_dv = gtk_builder_get_object(builder, "lb_pa_transfer_dv");
	lb_pa_total_dv = gtk_builder_get_object(builder, "lb_pa_total_dv");
	lb_pa_periapsis = gtk_builder_get_object(builder, "lb_pa_periapsis");
	st_pa_step_group_selector = gtk_builder_get_object(builder, "st_pa_step_group_selector");
	vp_pa_groups = gtk_builder_get_object(builder, "vp_pa_groups");


	set_stable_tooltip(GTK_WIDGET(da_pa_porkchop),
			"Click a node to select it; use arrow keys to navigate. Left-drag to zoom, right-drag to pan, scroll to zoom.");
	gtk_widget_add_events(GTK_WIDGET(da_pa_porkchop), GDK_KEY_PRESS_MASK);
	pa_porkchop_screen = new_screen(GTK_WIDGET(da_pa_porkchop), &on_pa_screen_resize, NULL,
			&on_pa_screen_button_release, &on_pa_screen_mouse_move, NULL);
	g_signal_connect(da_pa_porkchop, "button-press-event", G_CALLBACK(on_pa_screen_button_press), pa_porkchop_screen);
	g_signal_connect(da_pa_porkchop, "key-press-event", G_CALLBACK(on_pa_porkchop_key_press), pa_porkchop_screen);
	g_signal_connect(da_pa_porkchop, "scroll-event", G_CALLBACK(on_pa_porkchop_scroll), pa_porkchop_screen);
	set_screen_background_color(pa_porkchop_screen, 0.15, 0.15, 0.15);
	pa_itin_preview_camera = new_camera(GTK_WIDGET(da_pa_preview), &on_pa_screen_resize, &on_enable_camera_rotation, &on_disable_camera_rotation, &on_pa_screen_mouse_move, &on_pa_screen_scroll);

	pa_system = NULL;
}


// ITINERARY PREVIEW AND PORKCHOP CALLBACKS -----------------------------------------------
void update_pa_porkchop_diagram() {
	clear_screen(pa_porkchop_screen);

	if(pa_porkchop_points != NULL) draw_porkchop(pa_porkchop_screen->static_layer.cr, pa_porkchop_screen->width,
			pa_porkchop_screen->height, pa_porkchop_points, pa_num_itins, pa_last_transfer_type, pa_yaxis_type,
			&pa_porkchop_view);
	draw_pa_porkchop_selection_marker();
	draw_screen(pa_porkchop_screen);
}

static void select_pa_porkchop_point(int point_index) {
	if(point_index < 0 || point_index >= pa_num_itins || pa_porkchop_points == NULL) return;
	pa_selected_arrival = pa_porkchop_points[point_index].data.arrival;
	update_pa_selected_itinerary();
	clear_dynamic_screen_layer(pa_porkchop_screen);
	draw_pa_porkchop_selection_marker();
	draw_screen(pa_porkchop_screen);
	update_pa_itinerary_preview();
}

gboolean on_pa_porkchop_key_press(GtkWidget *widget, GdkEventKey *event, gpointer ptr) {
	if((Screen*)ptr != pa_porkchop_screen || pa_porkchop_points == NULL ||
		!pa_porkchop_view.initialized) return FALSE;
	enum PorkchopDirection direction;
	switch(event->keyval) {
		case GDK_KEY_Left: direction = PORKCHOP_LEFT; break;
		case GDK_KEY_Right: direction = PORKCHOP_RIGHT; break;
		case GDK_KEY_Up: direction = PORKCHOP_UP; break;
		case GDK_KEY_Down: direction = PORKCHOP_DOWN; break;
		default: return FALSE;
	}
	double left, right, bottom;
	if(!get_pa_porkchop_plot_bounds(&left, &right, &bottom)) return TRUE;
	int point_index = find_directional_porkchop_neighbor(pa_porkchop_points, pa_num_itins,
			pa_selected_arrival, direction, pa_yaxis_type, &pa_porkchop_view.range,
			right - left, bottom);
	select_pa_porkchop_point(point_index);
	return TRUE;
}

G_MODULE_EXPORT void on_pa_select_best_visible_node(GtkWidget* widget, gpointer data) {
	if(pa_porkchop_points == NULL || !pa_porkchop_view.initialized) return;
	int best_index = find_best_porkchop_point_in_view(pa_porkchop_points, pa_num_itins,
			pa_last_transfer_type, pa_yaxis_type, &pa_porkchop_view.range);
	select_pa_porkchop_point(best_index);
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
		draw_body_moons(pa_itin_preview_camera, pa_system, pa_system->bodies[i], current_date_pa, NULL);
	}
	// Transfers
	if(curr_transfer_pa != NULL) draw_itinerary(pa_itin_preview_camera, pa_system, curr_transfer_pa, current_date_pa);

	draw_camera_image(pa_itin_preview_camera);
}

void on_pa_screen_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer ptr) {
	if((Camera*)ptr == pa_itin_preview_camera) {
		on_camera_zoom(widget, event, pa_itin_preview_camera);
		update_pa_itinerary_preview();
	}
}

gboolean on_pa_porkchop_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer ptr) {
	if((Screen*)ptr != pa_porkchop_screen) return FALSE;
	double left, right, bottom;
	if(!get_pa_porkchop_plot_bounds(&left, &right, &bottom) ||
		!pa_porkchop_view.initialized || !pa_porkchop_point_inside_plot(event->x, event->y, left, right, bottom))
		return FALSE;

	double scroll_steps;
	switch(event->direction) {
		case GDK_SCROLL_UP: scroll_steps = -1; break;
		case GDK_SCROLL_DOWN: scroll_steps = 1; break;
		case GDK_SCROLL_SMOOTH: scroll_steps = event->delta_y; break;
		default: return FALSE;
	}
	double factor = pow(1.2, scroll_steps);
	double x_span = pa_porkchop_view.range.max_x - pa_porkchop_view.range.min_x;
	double y_span = pa_porkchop_view.range.max_y - pa_porkchop_view.range.min_y;
	double min_x_span = (pa_porkchop_view.data_range.max_x - pa_porkchop_view.data_range.min_x) * 1e-6;
	double min_y_span = (pa_porkchop_view.data_range.max_y - pa_porkchop_view.data_range.min_y) * 1e-6;
	double max_x_span = (pa_porkchop_view.data_range.max_x - pa_porkchop_view.data_range.min_x) * 1e4;
	double max_y_span = (pa_porkchop_view.data_range.max_y - pa_porkchop_view.data_range.min_y) * 1e4;
	if(!isfinite(factor) || factor <= 0 || !isfinite(x_span) || !isfinite(y_span)) return TRUE;
	factor = fmax(factor, fmax(min_x_span / x_span, min_y_span / y_span));
	factor = fmin(factor, fmin(max_x_span / x_span, max_y_span / y_span));
	porkchop_plot_zoom(&pa_porkchop_view.range, &pa_porkchop_view.data_range, factor,
			(event->x - left) / (right - left),
			(bottom - event->y) / bottom);
	update_pa_porkchop_diagram();
	return TRUE;
}

void on_pa_screen_resize(GtkWidget *widget, cairo_t *cr, gpointer ptr) {
	if((Screen*)ptr == pa_porkchop_screen) {
		resize_screen(pa_porkchop_screen);
		update_pa_porkchop_diagram();
	}
	if((Camera*)ptr == pa_itin_preview_camera) {
		resize_camera_screen(pa_itin_preview_camera);
		update_pa_itinerary_preview();
	}
}

gboolean on_pa_screen_button_press(GtkWidget *widget, GdkEventButton *event, gpointer ptr) {
	if((Screen*)ptr != pa_porkchop_screen ||
		(event->button != GDK_BUTTON_PRIMARY && event->button != GDK_BUTTON_SECONDARY)) return FALSE;
	double left, right, bottom;
	if(!get_pa_porkchop_plot_bounds(&left, &right, &bottom) ||
		!pa_porkchop_point_inside_plot(event->x, event->y, left, right, bottom)) return FALSE;

	gtk_widget_grab_focus(widget);
	on_screen_button_press(widget, event, pa_porkchop_screen);
	if(event->button == GDK_BUTTON_SECONDARY)
		pa_porkchop_drag_mode = PA_PORKCHOP_DRAG_PAN;
	else
		pa_porkchop_drag_mode = PA_PORKCHOP_DRAG_ZOOM;
	return TRUE;
}

void on_pa_screen_mouse_move(GtkWidget *widget, GdkEventButton *event, gpointer ptr) {
	if((Screen*)ptr == pa_porkchop_screen) {
		double left, right, bottom;
		if(!get_pa_porkchop_plot_bounds(&left, &right, &bottom)) return;
		if(pa_porkchop_screen->dragging) {
			if(pa_porkchop_drag_mode == PA_PORKCHOP_DRAG_PAN) {
				double dx_fraction = (event->x - pa_porkchop_screen->last_mouse_pos.x) / (right - left);
				double dy_fraction = (event->y - pa_porkchop_screen->last_mouse_pos.y) / bottom;
				porkchop_plot_pan(&pa_porkchop_view.range, &pa_porkchop_view.data_range,
						dx_fraction, dy_fraction);
				pa_porkchop_screen->last_mouse_pos = vec2(event->x, event->y);
				update_pa_porkchop_diagram();
			} else if(pa_porkchop_drag_mode == PA_PORKCHOP_DRAG_ZOOM) {
				double x0 = pa_porkchop_screen->mouse_pos_on_press.x;
				double y0 = pa_porkchop_screen->mouse_pos_on_press.y;
				double x1 = fmax(left, fmin(right, event->x));
				double y1 = fmax(0, fmin(bottom, event->y));
				x0 = fmax(left, fmin(right, x0));
				y0 = fmax(0, fmin(bottom, y0));
				clear_dynamic_screen_layer(pa_porkchop_screen);
				draw_pa_porkchop_selection_marker();
				cairo_t *cr = pa_porkchop_screen->dynamic_layer.cr;
				cairo_rectangle(cr, fmin(x0, x1), fmin(y0, y1), fabs(x1 - x0), fabs(y1 - y0));
				cairo_set_source_rgba(cr, 1, 0, 0, 1);
				cairo_stroke(cr);
				draw_screen(pa_porkchop_screen);
			}
		} else if(pa_porkchop_view.initialized &&
				pa_porkchop_point_inside_plot(event->x, event->y, left, right, bottom)) {
			draw_pa_porkchop_hover(event->x, event->y, left, right, bottom);
		} else {
			clear_dynamic_screen_layer(pa_porkchop_screen);
			draw_pa_porkchop_selection_marker();
			draw_screen(pa_porkchop_screen);
		}
	}
	if((Camera*)ptr == pa_itin_preview_camera && pa_itin_preview_camera->rotation_sensitive) {
		on_camera_rotate(pa_itin_preview_camera, event);
		resize_camera_screen(pa_itin_preview_camera);
		update_pa_itinerary_preview();
	}
}

void on_pa_screen_button_release(GtkWidget *widget, GdkEventButton *event, gpointer ptr) {
	if((Screen*)ptr == pa_porkchop_screen) {
		enum PaPorkchopDragMode drag_mode = pa_porkchop_drag_mode;
		pa_porkchop_screen->dragging = FALSE;
		pa_porkchop_drag_mode = PA_PORKCHOP_DRAG_NONE;

		clear_dynamic_screen_layer(pa_porkchop_screen);
		draw_pa_porkchop_selection_marker();

		if(drag_mode == PA_PORKCHOP_DRAG_ZOOM) {
			double left, right, bottom;
			if(!get_pa_porkchop_plot_bounds(&left, &right, &bottom)) return;
			double x0 = fmax(left, fmin(right, pa_porkchop_screen->mouse_pos_on_press.x));
			double x1 = fmax(left, fmin(right, event->x));
			double y0 = fmax(0, fmin(bottom, pa_porkchop_screen->mouse_pos_on_press.y));
			double y1 = fmax(0, fmin(bottom, event->y));
			if(fabs(x1 - x0) < 2 || fabs(y1 - y0) < 2) {
				int point_index = find_nearest_porkchop_point(pa_porkchop_points, pa_num_itins,
						pa_yaxis_type, &pa_porkchop_view.range, left, right, bottom,
						event->x, event->y, 10);
				if(point_index >= 0) {
					select_pa_porkchop_point(point_index);
					return;
				}
				draw_screen(pa_porkchop_screen);
				return;
			}
			double min_x_fraction = (fmin(x0, x1) - left) / (right - left);
			double max_x_fraction = (fmax(x0, x1) - left) / (right - left);
			double min_y_fraction = (bottom - fmax(y0, y1)) / bottom;
			double max_y_fraction = (bottom - fmin(y0, y1)) / bottom;
			porkchop_plot_zoom_to_box(&pa_porkchop_view.range, min_x_fraction, max_x_fraction,
					min_y_fraction, max_y_fraction);
			update_pa_porkchop_diagram();
			return;
		}
		if(drag_mode != PA_PORKCHOP_DRAG_ZOOM) {
			draw_screen(pa_porkchop_screen);
			return;
		}
	}
}

// -------------------------------------------------------------------------------------


void pa_change_date_type(enum DateType old_date_type, enum DateType new_date_type) {
	begin_pa_filter_entry_update();
	change_text_field_date_type(tf_pa_min_feedback[PA_DEP], old_date_type, new_date_type);
	change_text_field_date_type(tf_pa_max_feedback[PA_DEP], old_date_type, new_date_type);
	change_text_field_date_type(tf_pa_min_feedback[PA_ARR], old_date_type, new_date_type);
	change_text_field_date_type(tf_pa_max_feedback[PA_ARR], old_date_type, new_date_type);
	end_pa_filter_entry_update();
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
	enum PaYAxisType next_axis = pa_yaxis_type != PA_YAXIS_DUR ?
			PA_YAXIS_DUR : PA_YAXIS_ARRDATE;
	if(pa_porkchop_view.initialized &&
		!porkchop_plot_switch_y_axis(&pa_porkchop_view.range, pa_yaxis_type, next_axis))
		reset_pa_porkchop_view();
	pa_yaxis_type = next_axis;
	update_pa_y_axis_filter_fields();
	if(pa_porkchop_points != NULL) apply_filter();
	update_pa();
}


G_MODULE_EXPORT void on_last_transfer_type_changed_pa(GtkWidget* widget, gpointer data) {
	if(pa_setting_transfer_type || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) return;
	const char *name = gtk_widget_get_name(widget);
	enum LastTransferType selected_type;
	if		(strcmp(name, "fb") == 0) selected_type = TF_FLYBY;
	else if	(strcmp(name, "capture") == 0) selected_type = TF_CAPTURE;
	else return;
	if(pa_last_transfer_type_locked && selected_type != pa_last_transfer_type) {
		pa_set_last_transfer_type(pa_last_transfer_type, 1);
		return;
	}
	if(pa_has_both_modes && selected_type != pa_last_transfer_type) {
		pa_last_transfer_type = selected_type;
		clear_porkchop_analyzer_analysis();
		if(selected_type == TF_CAPTURE) {
			pa_departures = pa_dual_capture_departures;
			pa_num_deps = pa_dual_capture_num_deps;
		} else {
			pa_departures = pa_dual_flyby_departures;
			pa_num_deps = pa_dual_flyby_num_deps;
		}
		analyze_departure_itins();
		reset_pa_porkchop_view();
		update_pa_porkchop_diagram();
		update_pa_itinerary_preview();
		return;
	}

	pa_last_transfer_type = selected_type;
	if(pa_porkchop_points == NULL) return;
	reset_pa_porkchop_view();
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
}


G_MODULE_EXPORT void on_pa_update(GtkWidget* widget, gpointer data) {
	if(pa_porkchop_points == NULL) return;
	reset_pa_porkchop_view();
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