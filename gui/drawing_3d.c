#include "drawing.h"
#include "settings.h"
#include "orbit_calculator/flyby_policy.h"
#include <math.h>
#include <stdio.h>

typedef struct { Vector3 center; double radius; } BodyOccluder;
static BodyOccluder occluders[256];
static int num_occluders;

static Vector3 body_position(CelestSystem *system, Body *body, double jd_date) {
	if(body == system->cb) return vec3(0, 0, 0);
	// A moon orbits its planet, so its elements give a position relative to the planet.
	if(body->orbit.cb != NULL && body->orbit.cb != system->cb && body->orbit.cb != body)
		return add_vec3(body_position(system, body->orbit.cb, jd_date), osv_from_elements(body->orbit, jd_date).r);
	return system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(body->orbit, jd_date).r :
		osv_from_ephem(body->ephem, body->num_ephems, jd_date, system->cb).r;
}

// Call before drawing lines so bodies drawn later in the frame still hide them.
void register_body_occluders(CelestSystem *system, double jd_date) {
	num_occluders = 0;
	for(int i = -1; i < system->num_bodies && num_occluders < 256; i++) {
		Body *body = i < 0 ? system->cb : system->bodies[i];
		occluders[num_occluders++] = (BodyOccluder){body_position(system, body, jd_date), body->radius};
	}
	for(int i = 0; i < system->num_bodies; i++) {
		CelestSystem *moons = system->bodies[i]->system;
		for(int j = 0; moons != NULL && j < moons->num_bodies && num_occluders < 256; j++)
			occluders[num_occluders++] = (BodyOccluder){body_position(system, moons->bodies[j], jd_date), moons->bodies[j]->radius};
	}
}

static int point_is_hidden_by_body(Camera *camera, Vector3 point) {
	Vector3 ray = subtract_vec3(point, camera->pos);
	double distance = mag_vec3(ray);
	if(!(distance > 0)) return 0;
	Vector3 direction = scale_vec3(ray, 1 / distance);
	for(int i = 0; i < num_occluders; i++) {
		Vector3 to_camera = subtract_vec3(camera->pos, occluders[i].center);
		double b = dot_vec3(to_camera, direction);
		double discriminant = b * b - (dot_vec3(to_camera, to_camera) - occluders[i].radius * occluders[i].radius);
		if(discriminant <= 0) continue;
		double entry = -b - sqrt(discriminant);
		if(entry > 0 && entry < distance) return 1;
	}
	return 0;
}

void draw_body(Camera *camera, CelestSystem *system, Body *body, double jd_date) {
	set_cairo_body_color(get_camera_screen_cairo(camera), body);
	Vector3 position = body_position(system, body, jd_date);
	Vector2 p2d_body = p3d_to_p2d(camera, position);
	// Never smaller than the marker dot, but the real size once zoomed in close.
	double depth = camera_depth(camera, position);
	double marker_radius = 5;
	if(depth > 0) marker_radius = fmax(marker_radius, body->radius * camera_pixels_per_meter(camera, depth));
	cairo_arc(get_camera_screen_cairo(camera), p2d_body.x, p2d_body.y, marker_radius, 0, 2 * M_PI);
	cairo_fill(get_camera_screen_cairo(camera));
}

// Cut where the segment passes behind the camera so it cannot streak across the screen.
static void stroke_segment_3d(Camera *camera, Vector3 a, Vector3 b) {
	const double near_depth = 1;
	double za = camera_depth(camera, a), zb = camera_depth(camera, b);
	if(za <= near_depth && zb <= near_depth) return;
	if(za <= near_depth) a = add_vec3(a, scale_vec3(subtract_vec3(b, a), (near_depth - za) / (zb - za)));
	else if(zb <= near_depth) b = add_vec3(b, scale_vec3(subtract_vec3(a, b), (near_depth - zb) / (za - zb)));
	Vector2 pa = p3d_to_p2d(camera, a), pb = p3d_to_p2d(camera, b);
	double width = camera->screen->width, height = camera->screen->height;
	if((pa.x < 0 && pb.x < 0) || (pa.y < 0 && pb.y < 0) || (pa.x > width && pb.x > width) || (pa.y > height && pb.y > height)) return;
	// Only a segment that touches a body is worth splitting; most never do.
	int pieces = 1;
	if(num_occluders > 0 && (point_is_hidden_by_body(camera, a) || point_is_hidden_by_body(camera, b) ||
			point_is_hidden_by_body(camera, scale_vec3(add_vec3(a, b), 0.5))))
		pieces = fmax(1, fmin(16, ceil(hypot(pb.x - pa.x, pb.y - pa.y) / 4)));
	for(int i = 0; i < pieces; i++) {
		Vector3 from = add_vec3(a, scale_vec3(subtract_vec3(b, a), (double)i / pieces));
		Vector3 to = add_vec3(a, scale_vec3(subtract_vec3(b, a), (double)(i + 1) / pieces));
		if(num_occluders > 0 && point_is_hidden_by_body(camera, scale_vec3(add_vec3(from, to), 0.5))) continue;
		draw_stroke(get_camera_screen_cairo(camera), p3d_to_p2d(camera, from), p3d_to_p2d(camera, to));
	}
}

static void draw_orbit_around(Camera *camera, Orbit orbit, Vector3 center) {
	OSV osv = osv_from_orbit(orbit);
	Vector3 last_r = add_vec3(osv.r, center);
	double ta_step = deg2rad(0.5);

	for(double dta = 0; dta < M_PI*2 + ta_step; dta += ta_step) {
		orbit.ta += ta_step;
		osv = osv_from_orbit(orbit);
		Vector3 r = add_vec3(osv.r, center);
		stroke_segment_3d(camera, last_r, r);
		last_r = r;
	}
}

void draw_orbit(Camera *camera, Orbit orbit) {
	draw_orbit_around(camera, orbit, vec3(0, 0, 0));
}

void draw_body_moons(Camera *camera, CelestSystem *system, Body *planet, double jd_date, int (*is_visible)(Body *moon)) {
	CelestSystem *moons = planet->system;
	if(moons == NULL) return;
	cairo_t *cr = get_camera_screen_cairo(camera);
	Vector3 planet_position = body_position(system, planet, jd_date);
	Vector2 planet_point = p3d_to_p2d(camera, planet_position);
	for(int i = 0; i < moons->num_bodies; i++) {
		Body *moon = moons->bodies[i];
		if(is_visible != NULL && !is_visible(moon)) continue;
		draw_body(camera, system, moon, jd_date);
		cairo_set_source_rgba(cr, moon->color[0], moon->color[1], moon->color[2], 0.4);
		draw_orbit_around(camera, moon->orbit, planet_position);
		// Labels of moons close to their planet would pile up on it.
		Vector2 moon_point = p3d_to_p2d(camera, body_position(system, moon, jd_date));
		if(hypot(moon_point.x - planet_point.x, moon_point.y - planet_point.y) > 30)
			draw_body_label(camera, system, moon, jd_date);
	}
}

void draw_celestial_system(Camera *camera, CelestSystem *system, double jd_date) {
	register_body_occluders(system, jd_date);
	draw_body(camera, system, system->cb, jd_date);

	for(int i = 0; i < system->num_bodies; i++) {
		draw_body(camera, system, system->bodies[i], jd_date);
		draw_orbit(camera, system->bodies[i]->orbit);
		draw_body_moons(camera, system, system->bodies[i], jd_date, NULL);
	}
}

void draw_trajectory(Camera *camera, struct OSV osv0, double dt, struct Body *attractor) {
	if(dt <= 1.0/86400) return;
	Orbit orbit = constr_orbit_from_osv(osv0.r, osv0.v, attractor);

	if(calc_orbital_period(orbit) < dt*86400 && orbit.e < 1) {
		draw_orbit(camera, orbit);
	}

	double ta0 = orbit.ta;
	double ta1 = propagate_orbit_time(orbit, dt*86400).ta;

	if(ta1 < ta0) ta1 += 2*M_PI;

	OSV osv = osv0;
	Vector3 last_r = osv.r;
	double ta_step = (ta1 - ta0)/1000;
	if(!(ta_step > 0) || !isfinite(ta_step)) return;

	for(int n = 0; n <= 1000; n++) {
		orbit.ta += ta_step;
		osv = osv_from_orbit(orbit);
		stroke_segment_3d(camera, last_r, osv.r);
		last_r = osv.r;
	}
}

static const double leg_palette[][3] = {
	{0.25, 0.75, 1.00}, {1.00, 0.65, 0.15}, {0.55, 0.90, 0.35}, {0.90, 0.45, 0.90},
	{1.00, 0.90, 0.25}, {0.30, 1.00, 0.80}, {0.60, 0.60, 1.00}, {1.00, 0.55, 0.55}
};
#define LEG_PALETTE_SIZE ((int)(sizeof(leg_palette) / sizeof(leg_palette[0])))

// Unflyable legs stay red; a leg already flown is dimmed.
static void set_leg_color(cairo_t *cr, int leg, int trajectory_is_in_the_past, int trajectory_is_viable) {
	const double *color = leg_palette[leg % LEG_PALETTE_SIZE];
	double alpha = trajectory_is_in_the_past ? 0.45 : 1.0;
	if(trajectory_is_viable) cairo_set_source_rgba(cr, color[0], color[1], color[2], alpha);
	else cairo_set_source_rgba(cr, 1, 0.2, 0.2, alpha);
}

static void set_label_font(cairo_t *cr, double size, int bold) {
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

static void draw_trajectory_arc(Camera *camera, OSV osv0, double dt, Body *attractor) {
	set_stroke_width(2);
	draw_trajectory(camera, osv0, dt, attractor);
	set_stroke_width(1);
}

void draw_itinerary_spacecraft(Camera *camera, Vector3 r) {
	cairo_t *cr = get_camera_screen_cairo(camera);
	Vector2 p2d = p3d_to_p2d(camera, r);
	cairo_arc(cr, p2d.x, p2d.y, 4.5, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 1, 0.4, 0.1);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 1.2);
	cairo_stroke(cr);
}

// A thin outlined starburst, kept light so it does not hide the trajectory.
static void draw_crash_symbol(cairo_t *cr, Vector2 p) {
	const int points = 8;
	for(int i = 0; i < points * 2; i++) {
		double angle = M_PI * i / points - M_PI / 2;
		double radius = i % 2 == 0 ? 11 : 5;
		double x = p.x + radius * cos(angle), y = p.y + radius * sin(angle);
		if(i == 0) cairo_move_to(cr, x, y);
		else cairo_line_to(cr, x, y);
	}
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, 1, 0.45, 0.2, 0.35);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 1, 0.35, 0.25);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

// A ring marks a flyby or an endpoint, a diamond a deep-space maneuver.
static void draw_itinerary_step_marker(Camera *camera, struct ItinStep *step) {
	cairo_t *cr = get_camera_screen_cairo(camera);
	Vector2 p = p3d_to_p2d(camera, step->r);
	int crashes = flyby_below_min_safe(step);
	cairo_new_path(cr);
	if(crashes) draw_crash_symbol(cr, p);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 1.5);
	if(step->body != NULL) {
		cairo_arc(cr, p.x, p.y, 8, 0, 2 * M_PI);
	} else {
		cairo_move_to(cr, p.x, p.y - 7);
		cairo_line_to(cr, p.x + 7, p.y);
		cairo_line_to(cr, p.x, p.y + 7);
		cairo_line_to(cr, p.x - 7, p.y);
		cairo_close_path(cr);
	}
	cairo_stroke(cr);

	char date_text[20], label[80];
	date_to_string(convert_JD_date(step->date, get_settings_datetime_type()), date_text, 0);
	snprintf(label, sizeof(label), "%s  %s%s", step->body != NULL ? step->body->name : "DSM", date_text,
		crashes ? "  CRASH" : "");
	set_label_font(cr, 11, crashes);
	if(crashes) cairo_set_source_rgb(cr, 1, 0.45, 0.35);
	cairo_move_to(cr, p.x + 11, p.y - 9);
	cairo_show_text(cr, label);
	cairo_new_path(cr);
	cairo_set_line_width(cr, 1);
}

// Powered flybys change excess speed, so only the periapsis clearance decides viability.
static int is_flyby_leg_viable(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double jd) {
	return flyby_is_viable(v_arr, v_dep, v_body, body, jd);
}

// Stored leg velocities start at the SOI boundary, so propagating them from the node would miss the next node.
static Vector3 leg_drawing_velocity(CelestSystem *system, struct ItinStep *tf) {
	double dt = (tf->next[0]->date - tf->date) * 86400;
	if(dt > 0) {
		Lambert3 leg;
		// Legs of a resonant route span revolutions, so pick the arc matching the stored velocity.
		if(nearest_leg_variant_lambert(system->cb, tf->r, tf->next[0]->r, dt, tf->next[0]->v_dep, &leg, NULL)) return leg.v0;
	}
	return tf->next[0]->v_dep;
}

int itinerary_spacecraft_position(CelestSystem *system, struct ItinStep *itin, double date, Vector3 *r) {
	if(itin == NULL) return 0;
	struct ItinStep *step = get_first(itin);
	while(step->next != NULL && date >= step->next[0]->date) step = step->next[0];
	if(step->next != NULL && date > step->date) {
		double dt = date - step->date;
		OSV start = {step->r, leg_drawing_velocity(system, step)};
		*r = propagate_osv_time(start, system->cb, dt * 86400).r;
		return 1;
	}
	// Before the departure and after the arrival the craft sits with its body.
	if(step->body == NULL) { *r = step->r; return 1; }
	*r = system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(step->body->orbit, date).r :
		osv_from_ephem(step->body->ephem, step->body->num_ephems, date, system->cb).r;
	return 1;
}

void draw_itinerary(Camera *camera, CelestSystem *system, struct ItinStep *tf, double current_time) {
	if(tf == NULL) return;
	cairo_t *cr = get_camera_screen_cairo(camera);

	// draw trajectories
	tf = get_first(tf);
	int leg = 0;
	while(tf->next != NULL) {
		OSV tf_osv0 = {tf->r, leg_drawing_velocity(system, tf)};
		int trajectory_is_viable = 1;
		double dt = tf->next[0]->date - tf->date;

		if(tf->prev != NULL && tf->body != NULL) {
			trajectory_is_viable = is_flyby_leg_viable(tf->v_arr, tf->next[0]->v_dep, tf->v_body, tf->body, tf->date);
		}

		if(current_time >= tf->date && current_time < tf->next[0]->date) {
			set_leg_color(cr, leg, 1, trajectory_is_viable);
			dt = current_time - tf->date;
			draw_trajectory_arc(camera, tf_osv0, dt, system->cb);

			OSV current_osv = dt != 0 ? propagate_osv_time(tf_osv0, system->cb, dt*86400) : tf_osv0;

			set_leg_color(cr, leg, 0, trajectory_is_viable);
			dt = tf->next[0]->date - current_time;
			draw_trajectory_arc(camera, current_osv, dt, system->cb);

			draw_itinerary_spacecraft(camera, current_osv.r);
		} else {
			set_leg_color(cr, leg, current_time > tf->date, trajectory_is_viable);
			draw_trajectory_arc(camera, tf_osv0, dt, system->cb);
		}
		tf = tf->next[0];
		leg++;
	}

	if(current_time <= get_first(tf)->date && get_first(tf)->body != NULL) {
		OSV osv_body = system->prop_method == ORB_ELEMENTS ?
				   osv_from_elements(get_first(tf)->body->orbit, current_time) :
				   osv_from_ephem(get_first(tf)->body->ephem, get_first(tf)->body->num_ephems, current_time, system->cb);
		draw_itinerary_spacecraft(camera, osv_body.r);
	} else if(current_time >= tf->date && tf->body != NULL) {
		OSV osv_body = system->prop_method == ORB_ELEMENTS ?
							  osv_from_elements(tf->body->orbit, current_time) :
							  osv_from_ephem(tf->body->ephem, tf->body->num_ephems, current_time, system->cb);
		draw_itinerary_spacecraft(camera, osv_body.r);
	}

	// draw itin step markers
	while(tf != NULL) {
		draw_itinerary_step_marker(camera, tf);
		tf = tf->prev;
	}
}

// Itinerary seen from inside one body's sphere of influence, as in the KSP map view: the conics are relative to the
// body and drawn around its current position, so they stay put while the body moves.
static double wrapped_true_anomaly(double ta) {
	while(ta > M_PI) ta -= 2 * M_PI;
	while(ta < -M_PI) ta += 2 * M_PI;
	return ta;
}

static void draw_conic_arc(Camera *camera, Orbit orbit, double ta_from, double ta_to, Vector3 offset) {
	const int steps = 240;
	orbit.ta = ta_from;
	Vector3 last = add_vec3(osv_from_orbit(orbit).r, offset);
	for(int i = 1; i <= steps; i++) {
		orbit.ta = ta_from + (ta_to - ta_from) * i / steps;
		Vector3 r = add_vec3(osv_from_orbit(orbit).r, offset);
		stroke_segment_3d(camera, last, r);
		last = r;
	}
}

static double sphere_of_influence_radius(Body *body) {
	if(body->orbit.cb == NULL || !(body->orbit.cb->mu > 0)) return 0;
	return fabs(body->orbit.a) * pow(body->mu / body->orbit.cb->mu, 0.4);
}

// Hyperbola with the given asymptotic travel direction and periapsis radius, from the edge of the sphere of influence.
static int hyperbola_from_asymptote(Body *body, Vector3 direction, double excess_speed, double periapsis_radius, int incoming, Orbit *orbit) {
	double soi = sphere_of_influence_radius(body);
	if(!(soi > periapsis_radius) || !(excess_speed > 0) || !(body->mu > 0)) return 0;
	double impact = periapsis_radius * sqrt(1 + 2 * body->mu / (periapsis_radius * excess_speed * excess_speed));
	if(!(soi > impact)) return 0;
	Vector3 axis = fabs(direction.z) < 0.95 ? vec3(0, 0, 1) : vec3(1, 0, 0);
	Vector3 normal = norm_vec3(cross_vec3(direction, axis));
	Vector3 side = cross_vec3(normal, direction);
	double along = sqrt(soi * soi - impact * impact);
	Vector3 r = add_vec3(scale_vec3(direction, incoming ? -along : along), scale_vec3(side, impact));
	Vector3 v = scale_vec3(direction, sqrt(excess_speed * excess_speed + 2 * body->mu / soi));
	*orbit = constr_orbit_from_osv(r, v, body);
	return isfinite(orbit->a) && isfinite(orbit->e) && isfinite(orbit->ta);
}

static void draw_periapsis_marker(Camera *camera, Vector3 r, Body *body, struct ItinStep *step, int crashes) {
	cairo_t *cr = get_camera_screen_cairo(camera);
	Vector2 p = p3d_to_p2d(camera, r);
	cairo_new_path(cr);
	if(crashes) draw_crash_symbol(cr, p);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 1.5);
	cairo_arc(cr, p.x, p.y, 6, 0, 2 * M_PI);
	cairo_stroke(cr);
	char date_text[20], label[80];
	date_to_string(convert_JD_date(step->date, get_settings_datetime_type()), date_text, 0);
	snprintf(label, sizeof(label), "%s  %s%s", body->name, date_text, crashes ? "  CRASH" : "");
	set_label_font(cr, 11, crashes);
	if(crashes) cairo_set_source_rgb(cr, 1, 0.45, 0.35);
	cairo_move_to(cr, p.x + 10, p.y - 8);
	cairo_show_text(cr, label);
	cairo_new_path(cr);
	cairo_set_line_width(cr, 1);
}

void draw_itinerary_around_body(Camera *camera, CelestSystem *system, struct ItinStep *itin, Body *planet, double current_time) {
	if(itin == NULL || planet == NULL) return;
	cairo_t *cr = get_camera_screen_cairo(camera);
	Vector3 offset = body_position(system, planet, current_time);
	double clearance = planet->radius + planet->atmo_alt + 50e3;
	int leg = 0;
	for(struct ItinStep *step = get_first(itin); step != NULL; step = step->next != NULL ? step->next[0] : NULL, leg++) {
		if(step->body != planet) continue;
		int past = current_time > step->date;
		set_stroke_width(2);
		if(step->prev != NULL && step->next != NULL) {
			PoweredFlybyResult flyby;
			if(!calculate_flyby_with_moons(step->v_arr, step->next[0]->v_dep, step->v_body, planet,
				flyby_minimum_periapsis(planet), step->date, &flyby)) { set_stroke_width(1); continue; }
			Orbit incoming = constr_orbit_from_osv(flyby.incoming_soi_state.r, flyby.incoming_soi_state.v, planet);
			Orbit outgoing = constr_orbit_from_osv(flyby.outgoing_soi_state.r, flyby.outgoing_soi_state.v, planet);
			int viable = is_flyby_leg_viable(step->v_arr, step->next[0]->v_dep, step->v_body, planet, step->date);
			set_leg_color(cr, leg - 1, past, viable);
			draw_conic_arc(camera, incoming, wrapped_true_anomaly(incoming.ta), 0, offset);
			set_leg_color(cr, leg, past, viable);
			draw_conic_arc(camera, outgoing, 0, wrapped_true_anomaly(outgoing.ta), offset);
			set_stroke_width(1);

			Orbit at_periapsis = incoming;
			at_periapsis.ta = 0;
			draw_periapsis_marker(camera, add_vec3(osv_from_orbit(at_periapsis).r, offset), planet, step, flyby_below_min_safe(step));

			double entry = step->date - flyby.incoming_soi_time / 86400, exit = step->date + flyby.outgoing_soi_time / 86400;
			if(current_time >= entry && current_time < step->date) {
				OSV start = {flyby.incoming_soi_state.r, flyby.incoming_soi_state.v};
				draw_itinerary_spacecraft(camera, add_vec3(propagate_osv_time(start, planet, (current_time - entry) * 86400).r, offset));
			} else if(current_time >= step->date && current_time <= exit) {
				Orbit leaving = outgoing;
				leaving.ta = 0;
				OSV start = osv_from_orbit(leaving);
				draw_itinerary_spacecraft(camera, add_vec3(propagate_osv_time(start, planet, (current_time - step->date) * 86400).r, offset));
			}
			continue;
		}
		Orbit orbit;
		if(step->next != NULL) {
			Vector3 excess = subtract_vec3(step->next[0]->v_dep, step->v_body);
			if(hyperbola_from_asymptote(planet, norm_vec3(excess), mag_vec3(excess), clearance, 0, &orbit)) {
				set_leg_color(cr, leg, past, 1);
				draw_conic_arc(camera, orbit, 0, wrapped_true_anomaly(orbit.ta), offset);
			}
		} else if(step->prev != NULL) {
			Vector3 excess = subtract_vec3(step->v_arr, step->v_body);
			if(hyperbola_from_asymptote(planet, norm_vec3(excess), mag_vec3(excess), clearance, 1, &orbit)) {
				set_leg_color(cr, leg - 1, past, 1);
				draw_conic_arc(camera, orbit, wrapped_true_anomaly(orbit.ta), 0, offset);
			}
		}
		set_stroke_width(1);
	}
}

void draw_body_label(Camera *camera, CelestSystem *system, Body *body, double jd_date) {
	cairo_t *cr = get_camera_screen_cairo(camera);
	Vector2 p = p3d_to_p2d(camera, body_position(system, body, jd_date));
	set_cairo_body_color(cr, body);
	set_label_font(cr, 10, 0);
	cairo_move_to(cr, p.x + 8, p.y + 14);
	cairo_show_text(cr, body->name);
	cairo_new_path(cr);
}

// The legs with their colors, the leg under way highlighted, and the progress through it.
void draw_itinerary_legend(Camera *camera, CelestSystem *system, struct ItinStep *tf, double current_time) {
	if(tf == NULL) return;
	cairo_t *cr = get_camera_screen_cairo(camera);
	tf = get_first(tf);
	double width = camera->screen->width;
	int active_leg = -1, leg = 0;
	for(struct ItinStep *step = tf; step->next != NULL; step = step->next[0], leg++)
		if(current_time >= step->date && current_time < step->next[0]->date) active_leg = leg;

	set_label_font(cr, 11, 0);
	double y = 18;
	leg = 0;
	for(struct ItinStep *step = tf; step->next != NULL; step = step->next[0], leg++) {
		struct ItinStep *next = step->next[0];
		char text[96];
		snprintf(text, sizeof(text), "%s -> %s  (%.0f d)", step->body != NULL ? step->body->name : "DSM",
			next->body != NULL ? next->body->name : "DSM", next->date - step->date);
		cairo_text_extents_t extents;
		cairo_text_extents(cr, text, &extents);
		double x = width - 12 - extents.width;
		set_label_font(cr, 11, leg == active_leg);
		set_leg_color(cr, leg, 0, 1);
		cairo_set_line_width(cr, 3);
		cairo_move_to(cr, x - 26, y - 4);
		cairo_line_to(cr, x - 8, y - 4);
		cairo_stroke(cr);
		cairo_set_line_width(cr, 1);
		cairo_set_source_rgb(cr, leg == active_leg ? 1 : 0.7, leg == active_leg ? 1 : 0.7, leg == active_leg ? 1 : 0.7);
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, text);
		cairo_new_path(cr);
		y += 17;
	}

	struct ItinStep *last = get_last(tf);
	char status[96];
	if(current_time < tf->date) snprintf(status, sizeof(status), "Departs in %.1f d", tf->date - current_time);
	else if(current_time >= last->date) snprintf(status, sizeof(status), "Arrived");
	else if(active_leg >= 0) {
		struct ItinStep *step = tf;
		for(int i = 0; i < active_leg; i++) step = step->next[0];
		snprintf(status, sizeof(status), "Leg %d of %d, day %.1f of %.1f", active_leg + 1, leg,
			current_time - step->date, step->next[0]->date - step->date);
	} else status[0] = '\0';
	set_label_font(cr, 12, 1);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_move_to(cr, 12, 20);
	cairo_show_text(cr, status);
	cairo_new_path(cr);
}
