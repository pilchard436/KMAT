#include "transfer_planner_internal.h"
#include "gui/drawing.h"
#include "gui/css_loader.h"
#include "gui/gui_manager.h"
#include "orbit_calculator/flyby_policy.h"

#include <math.h>
#include <stdlib.h>

// Simulated days per real second for each entry of the speed dropdown.
static const double tp_animation_speeds[] = {1.0 / 24, 0.25, 1, 5, 10, 30, 100, 365.25};

static guint tp_animation_source;
static gint64 tp_animation_last_time;

// TRANSFER PLANNER SYSTEM VIEW CALLBACKS -----------------------------------------------
// The body the view is centered on; NULL is the central body at the origin.
Body *tp_focus_body;
// Set when the view follows the spacecraft instead of a body.
int tp_focus_spacecraft;

static Vector3 tp_body_position(Body *body, double jd) {
	if(body == NULL || body == tp_system->cb) return vec3(0, 0, 0);
	OSV osv = tp_system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(body->orbit, jd) :
		osv_from_ephem(body->ephem, body->num_ephems, jd, tp_system->cb);
	return osv.r;
}

// Keeps the view on a moving body as the date changes.
static void tp_follow_focus_body(void) {
	if(tp_focus_spacecraft) {
		Vector3 r;
		if(curr_transfer_tp != NULL && itinerary_spacecraft_position(tp_system, curr_transfer_tp, current_date_tp, &r))
			camera_set_focus(tp_system_camera, r);
		else tp_focus_spacecraft = 0;
		return;
	}
	if(tp_focus_body == NULL) return;
	camera_set_focus(tp_system_camera, tp_body_position(tp_focus_body, current_date_tp));
}

static void tp_focus_on(Body *body) {
	if(tp_system == NULL) return;
	tp_focus_spacecraft = 0;
	double pitch = get_camera_pos_pitch(tp_system_camera), yaw = get_camera_pos_yaw(tp_system_camera);
	// A straight-down view loses its direction to rounding once the focus is far from the origin.
	if(!isfinite(pitch) || pitch > 1.5) pitch = 1.5;
	if(pitch < -1.5) pitch = -1.5;
	if(!isfinite(yaw)) yaw = 0;
	if(body == NULL || body == tp_system->cb) {
		tp_focus_body = NULL;
		update_camera_to_celestial_system(tp_system_camera, tp_system, pitch, yaw);
		return;
	}
	tp_focus_body = body;
	camera_set_focus(tp_system_camera, tp_body_position(body, current_date_tp));
	// Frame the sphere of influence, and allow zooming in close to the surface.
	double soi = fabs(body->orbit.a) * pow(body->mu / tp_system->cb->mu, 0.4);
	if(!isfinite(soi) || !(soi > body->radius)) soi = body->radius * 50;
	update_camera_position_from_angles(tp_system_camera, pitch, yaw, soi * 7);
	tp_system_camera->min_pos_dist = body->radius * 15;
	camera_look_to_center(tp_system_camera);
}

static void tp_focus_on_spacecraft(Vector3 r) {
	double pitch = get_camera_pos_pitch(tp_system_camera), yaw = get_camera_pos_yaw(tp_system_camera);
	if(!isfinite(pitch) || pitch > 1.5) pitch = 1.5;
	if(pitch < -1.5) pitch = -1.5;
	if(!isfinite(yaw)) yaw = 0;
	double distance = get_camera_distance_to_center(tp_system_camera);
	tp_focus_body = NULL;
	tp_focus_spacecraft = 1;
	camera_set_focus(tp_system_camera, r);
	update_camera_position_from_angles(tp_system_camera, pitch, yaw, distance);
	tp_system_camera->min_pos_dist = 1e6;
	camera_look_to_center(tp_system_camera);
}

gboolean on_tp_screen_button_press(GtkWidget *widget, GdkEventButton *event, gpointer ptr) {
	(void)widget; (void)ptr;
	if(event->type != GDK_2BUTTON_PRESS || event->button != 1 || tp_system == NULL) return FALSE;
	const double pick_radius = 12;
	Body *hit = NULL;
	for(int i = -1; i < tp_system->num_bodies; i++) {
		Body *body = i < 0 ? tp_system->cb : tp_system->bodies[i];
		if(i >= 0 && !body_show_status_tp[i]) continue;
		Vector2 p = p3d_to_p2d(tp_system_camera, tp_body_position(body, current_date_tp));
		if(hypot(p.x - event->x, p.y - event->y) > pick_radius) continue;
		// Overlapping markers resolve to the parent, which is the more massive body.
		if(hit == NULL || body->mu > hit->mu) hit = body;
	}
	Vector3 craft;
	if(hit == NULL && curr_transfer_tp != NULL && itinerary_spacecraft_position(tp_system, curr_transfer_tp, current_date_tp, &craft)) {
		Vector2 p = p3d_to_p2d(tp_system_camera, craft);
		if(hypot(p.x - event->x, p.y - event->y) <= pick_radius) {
			tp_focus_on_spacecraft(craft);
			update_tp_system_view();
			return FALSE;
		}
	}
	tp_focus_on(hit);
	update_tp_system_view();
	return FALSE;
}

void update_tp_system_view() {
	int previous_restriction = set_flyby_unrestricted_periapsis(1);
	clear_camera_screen(tp_system_camera);
	if(tp_system == NULL) { set_flyby_unrestricted_periapsis(previous_restriction); return; }
	tp_follow_focus_body();
	register_body_occluders(tp_system, current_date_tp);

	draw_body(tp_system_camera, tp_system, tp_system->cb, current_date_tp);

	for(int i = 0; i < tp_system->num_bodies; i++) {
		if(!body_show_status_tp[i]) continue;
		draw_body(tp_system_camera, tp_system, tp_system->bodies[i], current_date_tp);
		draw_body_label(tp_system_camera, tp_system, tp_system->bodies[i], current_date_tp);
		struct Orbit orbit = tp_system->bodies[i]->orbit;
		if(tp_system->prop_method == EPHEMS) {
			struct OSV body_osv = osv_from_ephem(tp_system->bodies[i]->ephem, tp_system->bodies[i]->num_ephems, current_date_tp, tp_system->cb);
			orbit = constr_orbit_from_osv(body_osv.r, body_osv.v, tp_system->cb);
		}
		// Body orbits are kept faint so the trajectory stands out.
		cairo_set_source_rgba(get_camera_screen_cairo(tp_system_camera), tp_system->bodies[i]->color[0],
			tp_system->bodies[i]->color[1], tp_system->bodies[i]->color[2], 0.4);
		draw_orbit(tp_system_camera, orbit);
		draw_body_moons(tp_system_camera, tp_system, tp_system->bodies[i], current_date_tp);
	}

	draw_itinerary(tp_system_camera, tp_system, curr_transfer_tp, current_date_tp);
	draw_itinerary_legend(tp_system_camera, tp_system, curr_transfer_tp, current_date_tp);

	draw_camera_image(tp_system_camera);
	set_flyby_unrestricted_periapsis(previous_restriction);
}

void on_tp_screen_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer *ptr) {
	on_camera_zoom(widget, event, tp_system_camera);
	update_tp_system_view();
}

void on_tp_screen_resize(GtkWidget *widget, cairo_t *cr, gpointer *ptr) {
	resize_camera_screen(tp_system_camera);
	update_tp_system_view();
}

void on_tp_screen_mouse_move(GtkWidget *widget, GdkEventButton *event, gpointer *ptr) {
	if (tp_system_camera->rotation_sensitive) {
		on_camera_rotate(tp_system_camera, event);
		update_tp_system_view();
	}
}

void tp_update_show_body_list() {
	// Remove grid if exists
	if (grid_tp_bodies != NULL && GTK_WIDGET(vp_tp_bodies) == gtk_widget_get_parent(grid_tp_bodies)) {
		gtk_container_remove(GTK_CONTAINER(vp_tp_bodies), grid_tp_bodies);
	}

	grid_tp_bodies = gtk_grid_new();

	// Create a GtkLabel
	GtkWidget *label = gtk_label_new("Show Orbit");
	// width request
	gtk_widget_set_size_request(GTK_WIDGET(label), -1, -1);
	// set css class
	set_css_class_for_widget(GTK_WIDGET(label), "pag-header");

	// Set the label in the grid at the specified row and column
	gtk_grid_attach(GTK_GRID(grid_tp_bodies), label, 0, 0, 1, 1);
	gtk_widget_set_halign(label, GTK_ALIGN_START);

	// Create labels and buttons and add them to the grid
	for (int body_idx = 0; body_idx < tp_system->num_bodies; body_idx++) {
		int row = body_idx+1;
		GtkWidget *widget;
		// Create a show body check button
		widget = gtk_check_button_new_with_label(tp_system->bodies[body_idx]->name);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), body_show_status_tp[body_idx]);
		g_signal_connect(widget, "clicked", G_CALLBACK(on_body_toggle), &(body_show_status_tp[body_idx]));
		gtk_widget_set_halign(widget, GTK_ALIGN_START);

		// Set the label in the grid at the specified row and column
		gtk_grid_attach(GTK_GRID(grid_tp_bodies), widget, 0, row, 1, 1);
	}
	gtk_container_add (GTK_CONTAINER (vp_tp_bodies), grid_tp_bodies);
	gtk_widget_show_all(GTK_WIDGET(vp_tp_bodies));
}

void show_bodies_of_itinerary(struct ItinStep *step) {
	while(step != NULL) {
		if(step->body != NULL) {
			body_show_status_tp[get_body_system_id(step->body, tp_system)] = 1;
		}
		if(step->num_next_nodes > 0) step = step->next[0];
		else step = NULL;
	}
}

void tp_update_bodies() {
	if(body_show_status_tp != NULL) free(body_show_status_tp);
	body_show_status_tp = (gboolean*) malloc(tp_system->num_bodies*sizeof(gboolean));
	if(curr_transfer_tp != NULL) {
		for(int i = 0; i < tp_system->num_bodies; i++) body_show_status_tp[i] = 0;
		show_bodies_of_itinerary(get_first(curr_transfer_tp));
	} else {
		for(int i = 0; i < tp_system->num_bodies; i++) body_show_status_tp[i] = 1;
	}
	tp_update_show_body_list();
}

G_MODULE_EXPORT void on_body_toggle(GtkWidget* widget, gpointer data) {
	gboolean *show_body = (gboolean *) data;  // Cast data back to group struct
	*show_body = !*show_body;
	update_tp_system_view();
}

void tp_stop_animation(void) {
	if(tp_animation_source != 0) {
		g_source_remove(tp_animation_source);
		tp_animation_source = 0;
	}
	if(tb_tp_animate != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tb_tp_animate)))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_animate), FALSE);
}

void on_tp_view_destroyed(GtkWidget *widget, gpointer data) {
	(void)widget; (void)data;
	if(tp_animation_source != 0) {
		g_source_remove(tp_animation_source);
		tp_animation_source = 0;
	}
	// The window is closing, so the tick must not touch these any more.
	tb_tp_animate = NULL;
	cb_tp_anim_speed = NULL;
}

static gboolean tp_animation_tick(gpointer data) {
	(void)data;
	if(cb_tp_anim_speed == NULL) return G_SOURCE_REMOVE;
	gint64 now = g_get_monotonic_time();
	double elapsed = (now - tp_animation_last_time) / 1e6;
	tp_animation_last_time = now;
	int speed_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_anim_speed));
	if(speed_index < 0 || speed_index >= (int)G_N_ELEMENTS(tp_animation_speeds)) speed_index = 0;
	current_date_tp += elapsed * tp_animation_speeds[speed_index];

	// An itinerary is played once and stops where it ends.
	if(curr_transfer_tp != NULL && current_date_tp >= get_last(curr_transfer_tp)->date) {
		current_date_tp = get_last(curr_transfer_tp)->date;
		tp_animation_source = 0;
		update_date_label();
		update_tp_system_view();
		tp_stop_animation();
		return G_SOURCE_REMOVE;
	}
	update_date_label();
	update_tp_system_view();
	return G_SOURCE_CONTINUE;
}

G_MODULE_EXPORT void on_tp_animate_toggled(GtkToggleButton *button, gpointer data) {
	(void)data;
	if(!gtk_toggle_button_get_active(button)) {
		if(tp_animation_source != 0) {
			g_source_remove(tp_animation_source);
			tp_animation_source = 0;
		}
		gtk_button_set_label(GTK_BUTTON(button), "Animate");
		return;
	}
	if(tp_system == NULL) {
		gtk_toggle_button_set_active(button, FALSE);
		return;
	}
	// Start from the departure when the clock is outside the itinerary.
	if(curr_transfer_tp != NULL) {
		struct ItinStep *first = get_first(curr_transfer_tp), *last = get_last(curr_transfer_tp);
		if(current_date_tp < first->date || current_date_tp >= last->date) current_date_tp = first->date;
	}
	gtk_button_set_label(GTK_BUTTON(button), "Stop");
	tp_animation_last_time = g_get_monotonic_time();
	if(tp_animation_source == 0) tp_animation_source = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, 33, tp_animation_tick, NULL, NULL);
}
