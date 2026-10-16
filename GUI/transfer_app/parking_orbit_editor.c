#include "parking_orbit_editor.h"
#include "gui/gui_manager.h"
#include "gui/info_win_manager.h"
#include "gui/gui_tools/camera.h"
#include "tools/parking_orbit.h"
#include "tools/celestial_systems.h"
#include <math.h>
#include <stdlib.h>

enum { PO_PERI, PO_APO, PO_INC, PO_RAAN, PO_ARGP, PO_TA, PO_NUM };

extern double ic_dep_periapsis;
Body *ic_get_departure_body(void);

static GtkWidget *po_window, *po_entries[PO_NUM], *po_toggle, *po_status, *po_area;
static Camera *po_camera;
static Body *po_body;
static ParkingOrbit po_orbit;
static int po_orbit_ok;
static int po_loading;

static const double DEG = M_PI / 180.0;

static void po_write_entries(void) {
	double values[PO_NUM] = {(parking_orbit_periapsis_radius(&po_orbit) - po_body->radius) / 1e3,
		(parking_orbit_apoapsis_radius(&po_orbit) - po_body->radius) / 1e3, po_orbit.inclination / DEG,
		po_orbit.raan / DEG, po_orbit.arg_periapsis / DEG, po_orbit.true_anomaly / DEG};
	po_loading = 1;
	for(int i = 0; i < PO_NUM; i++) {
		char text[32];
		g_snprintf(text, sizeof(text), "%.6g", values[i]);
		gtk_entry_set_text(GTK_ENTRY(po_entries[i]), text);
	}
	po_loading = 0;
}

static int po_read_entries(ParkingOrbit *out) {
	double v[PO_NUM];
	for(int i = 0; i < PO_NUM; i++) {
		const char *text = gtk_entry_get_text(GTK_ENTRY(po_entries[i]));
		char *end;
		v[i] = strtod(text, &end);
		if(end == text || *end != '\0' || !isfinite(v[i])) return 0;
	}
	double periapsis_radius = po_body->radius + v[PO_PERI] * 1e3, apoapsis_radius = po_body->radius + v[PO_APO] * 1e3;
	// An apoapsis below the periapsis gives a negative eccentricity, which the validity check rejects.
	*out = (ParkingOrbit){.semi_major_axis = (periapsis_radius + apoapsis_radius) / 2,
		.eccentricity = (apoapsis_radius - periapsis_radius) / (apoapsis_radius + periapsis_radius),
		.inclination = v[PO_INC] * DEG,
		.raan = v[PO_RAAN] * DEG, .arg_periapsis = v[PO_ARGP] * DEG, .true_anomaly = v[PO_TA] * DEG};
	return 1;
}

static void po_set_default(Body *body) {
	po_orbit = parking_orbit_circular(body->radius + body->atmo_alt + ic_dep_periapsis);
	po_write_entries();
}

static void po_line(cairo_t *cr, Vector3 a, Vector3 b) {
	Vector2 p = p3d_to_p2d(po_camera, a), q = p3d_to_p2d(po_camera, b);
	cairo_move_to(cr, p.x, p.y);
	cairo_line_to(cr, q.x, q.y);
	cairo_stroke(cr);
}

static void po_draw_circle(cairo_t *cr, Vector3 axis_u, Vector3 axis_v, double radius) {
	Vector2 last = {0, 0};
	for(int i = 0; i <= 120; i++) {
		double angle = 2 * M_PI * i / 120;
		Vector3 point = add_vec3(scale_vec3(axis_u, radius * cos(angle)), scale_vec3(axis_v, radius * sin(angle)));
		Vector2 p = p3d_to_p2d(po_camera, point);
		if(i > 0) { cairo_move_to(cr, last.x, last.y); cairo_line_to(cr, p.x, p.y); cairo_stroke(cr); }
		last = p;
	}
}

static void po_redraw(void) {
	if(po_camera == NULL || po_body == NULL) return;
	clear_camera_screen(po_camera);
	cairo_t *cr = get_camera_screen_cairo(po_camera);
	cairo_set_line_width(cr, 1.2);

	// Body drawn as three great circles
	cairo_set_source_rgb(cr, 0.45, 0.45, 0.5);
	po_draw_circle(cr, vec3(1, 0, 0), vec3(0, 1, 0), po_body->radius);
	po_draw_circle(cr, vec3(1, 0, 0), vec3(0, 0, 1), po_body->radius);
	po_draw_circle(cr, vec3(0, 1, 0), vec3(0, 0, 1), po_body->radius);

	// Reference frame axes: x red, y green, z blue (the celestial system's inertial frame)
	double axis_len = po_body->radius * 1.6;
	if(po_orbit_ok) axis_len = fmax(axis_len, parking_orbit_apoapsis_radius(&po_orbit) * 1.1);
	cairo_set_source_rgb(cr, 0.8, 0.3, 0.3); po_line(cr, vec3(0, 0, 0), vec3(axis_len, 0, 0));
	cairo_set_source_rgb(cr, 0.3, 0.8, 0.3); po_line(cr, vec3(0, 0, 0), vec3(0, axis_len, 0));
	cairo_set_source_rgb(cr, 0.3, 0.5, 0.9); po_line(cr, vec3(0, 0, 0), vec3(0, 0, axis_len));

	if(po_orbit_ok) {
		cairo_set_source_rgb(cr, 1.0, 0.85, 0.3);
		Vector2 last = {0, 0};
		for(int i = 0; i <= 360; i++) {
			OSV state;
			if(!parking_orbit_state_at(&po_orbit, po_body, 2 * M_PI * i / 360, &state)) continue;
			Vector2 p = p3d_to_p2d(po_camera, state.r);
			if(i > 0) { cairo_move_to(cr, last.x, last.y); cairo_line_to(cr, p.x, p.y); cairo_stroke(cr); }
			last = p;
		}
		OSV periapsis, start;
		if(parking_orbit_state_at(&po_orbit, po_body, 0, &periapsis)) {
			Vector2 p = p3d_to_p2d(po_camera, periapsis.r);
			cairo_set_source_rgb(cr, 0.3, 1.0, 0.5);
			cairo_arc(cr, p.x, p.y, 4, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		if(parking_orbit_state_at(&po_orbit, po_body, po_orbit.true_anomaly, &start)) {
			Vector2 p = p3d_to_p2d(po_camera, start.r);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_arc(cr, p.x, p.y, 4, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}
	draw_camera_image(po_camera);
}

static void po_fit_camera(void) {
	if(po_camera == NULL || po_body == NULL) return;
	double extent = po_orbit_ok ? parking_orbit_apoapsis_radius(&po_orbit) : po_body->radius * 2;
	update_camera_position_from_angles(po_camera, 0.5, 0.6, extent * 7);
	camera_look_to_center(po_camera);
	po_camera->min_pos_dist = po_body->radius * 1.5;
	po_camera->max_pos_dist = fmax(extent, po_body->radius) * 60;
}

static void po_update(void) {
	ParkingOrbit parsed;
	char text[256];
	po_orbit_ok = 0;
	if(po_body == NULL) {
		gtk_label_set_text(GTK_LABEL(po_status), "No departure body selected.");
	} else if(!po_read_entries(&parsed)) {
		gtk_label_set_text(GTK_LABEL(po_status), "Invalid number in one of the fields.");
	} else if(!parking_orbit_is_valid(&parsed, po_body)) {
		g_snprintf(text, sizeof(text), "Invalid orbit: the apoapsis must not be below the periapsis, and the periapsis must be above the surface and atmosphere of %s.", po_body->name);
		gtk_label_set_text(GTK_LABEL(po_status), text);
	} else {
		po_orbit = parsed;
		po_orbit_ok = 1;
		double period = parking_orbit_period(&po_orbit, po_body);
		g_snprintf(text, sizeof(text), "%s: periapsis altitude %.1f km, apoapsis altitude %.1f km, period %.2f h",
			po_body->name, (parking_orbit_periapsis_radius(&po_orbit) - po_body->radius) / 1e3,
			(parking_orbit_apoapsis_radius(&po_orbit) - po_body->radius) / 1e3, period / 3600.0);
		gtk_label_set_text(GTK_LABEL(po_status), text);
	}
	po_redraw();
}

static void on_po_resize(GtkWidget *widget, cairo_t *cr, gpointer ptr) {
	(void)widget; (void)cr;
	if(ptr != po_camera) return;
	resize_camera_screen(po_camera);
	po_redraw();
}

static void on_po_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer ptr) {
	if(ptr != po_camera) return;
	on_camera_zoom(widget, event, po_camera);
	po_redraw();
}

static void on_po_motion(GtkWidget *widget, GdkEventButton *event, gpointer ptr) {
	(void)widget;
	if(ptr != po_camera) return;
	on_camera_rotate(po_camera, event);
	po_redraw();
}

void init_parking_orbit_editor(GtkBuilder *builder) {
	static const char *ids[PO_NUM] = {"tf_po_peri", "tf_po_apo", "tf_po_inc", "tf_po_raan", "tf_po_argp", "tf_po_ta"};
	po_window = GTK_WIDGET(gtk_builder_get_object(builder, "parking_orbit_window"));
	for(int i = 0; i < PO_NUM; i++) po_entries[i] = GTK_WIDGET(gtk_builder_get_object(builder, ids[i]));
	po_toggle = GTK_WIDGET(gtk_builder_get_object(builder, "cb_po_enable"));
	po_status = GTK_WIDGET(gtk_builder_get_object(builder, "lb_po_status"));
	po_area = GTK_WIDGET(gtk_builder_get_object(builder, "da_po_figure"));
	po_camera = new_camera(po_area, &on_po_resize, &on_enable_camera_rotation, &on_disable_camera_rotation, &on_po_motion, &on_po_scroll);
	po_body = NULL;
	po_orbit_ok = 0;
}

void end_parking_orbit_editor(void) {
	parking_orbit_set_active(NULL, NULL);
	if(po_camera != NULL) destroy_camera(po_camera);
	po_camera = NULL;
	po_body = NULL;
}

int parking_orbit_editor_prepare_search(Body *dep_body, char *error, size_t error_size) {
	parking_orbit_set_active(NULL, NULL);
	if(po_toggle == NULL || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(po_toggle))) return 1;
	if(dep_body == NULL || po_body != dep_body) {
		g_snprintf(error, error_size, "The custom parking orbit was defined for %s. Reopen the parking orbit editor for the current departure body or disable it.",
			po_body != NULL ? po_body->name : "another body");
		return 0;
	}
	ParkingOrbit parsed;
	if(!po_read_entries(&parsed) || !parking_orbit_is_valid(&parsed, dep_body)) {
		g_snprintf(error, error_size, "The custom parking orbit is invalid. Fix it in the parking orbit editor or disable it.");
		return 0;
	}
	parking_orbit_set_active(&parsed, dep_body);
	return 1;
}

void parking_orbit_editor_finish_search(void) {
	parking_orbit_set_active(NULL, NULL);
}

G_MODULE_EXPORT void on_open_parking_orbit_editor(void) {
	Body *dep_body = ic_get_departure_body();
	if(dep_body == NULL) {
		show_msg_window("Select a departure body first.");
		return;
	}
	if(dep_body != po_body) {
		po_body = dep_body;
		po_set_default(po_body);
	}
	char title[96];
	g_snprintf(title, sizeof(title), "Parking Orbit - %s", po_body->name);
	gtk_window_set_title(GTK_WINDOW(po_window), title);
	gtk_widget_show_all(po_window);
	gtk_window_present(GTK_WINDOW(po_window));
	po_update();
	po_fit_camera();
	po_redraw();
}

G_MODULE_EXPORT void on_parking_orbit_reset(void) {
	if(po_body == NULL) return;
	po_set_default(po_body);
	po_update();
	po_fit_camera();
	po_redraw();
}

G_MODULE_EXPORT void on_parking_orbit_changed(void) {
	if(po_loading || po_body == NULL) return;
	po_update();
}

G_MODULE_EXPORT gboolean on_parking_orbit_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data) {
	(void)event; (void)data;
	gtk_widget_hide(widget);
	return TRUE;
}
