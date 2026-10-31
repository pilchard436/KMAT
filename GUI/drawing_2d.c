#include "drawing.h"
#include "orbit_calculator/flyby_policy.h"
#include <math.h>

void draw_orbit_2d(cairo_t *cr, Vector2 center, double scale, Vector3 r, Vector3 v, struct Body *attractor) {
	int steps = 100;
	struct OSV last_osv = {r,v};
	
	for(int i = 1; i <= steps; i++) {
		double ta = 2*M_PI/steps * i;
		struct OSV osv = propagate_osv_ta((OSV){r,v},attractor,ta);
		// y negative, because in GUI y gets bigger downwards
		Vector2 p1 = {last_osv.r.x, -last_osv.r.y};
		Vector2 p2 = {osv.r.x, -osv.r.y};
		
		p1 = scale_vec2(p1,scale);
		p2 = scale_vec2(p2,scale);
		
		draw_stroke(cr, add_vec2(p1,center), add_vec2(p2,center));
		last_osv = osv;
	}
}

void draw_body_2d(cairo_t *cr, Vector2 center, double scale, Vector3 r) {
	int body_radius = 5;
	r = scale_vec3(r, scale);
	// y negative, because in GUI y gets bigger downwards
	r.y *= -1;
	cairo_arc(cr, center.x+r.x, center.y+r.y,body_radius,0,M_PI*2);
	cairo_fill(cr);
}

void draw_transfer_point_2d(cairo_t *cr, Vector2 center, double scale, Vector3 r) {
	int cross_length = 4;
	cairo_set_source_rgb(cr, 1, 0, 0);
	r = scale_vec3(r, scale);
	// y negative, because in GUI y gets bigger downwards
	r.y *= -1;
	for(int i = -1; i<=1; i+=2) {
		Vector2 p1 = {r.x - cross_length, r.y + cross_length*i};
		Vector2 p2 = {r.x + cross_length, r.y - cross_length*i};
		draw_stroke(cr, add_vec2(center, p1), add_vec2(center, p2));
	}
}

// Rework trajectory drawing -> OSV + dt   ----------------------------------------------
void draw_trajectory_2d(cairo_t *cr, Vector2 center, double scale, struct ItinStep *tf, struct Body *attractor) {
	// if double swing-by is not worth drawing
	if(tf->body == NULL && tf->v_body.x == 0) return;

	cairo_set_source_rgb(cr, 0, 1, 0);
	struct ItinStep *prev = tf->prev;
	// skip not working double swing-by
	if(tf->prev->body == NULL && tf->prev->v_body.x == 0) prev = tf->prev->prev;
	double dt = (tf->date-prev->date)*24*60*60;

	if(prev->prev != NULL && prev->body != NULL && tf->prev->body != NULL && tf->body != NULL) {
		if(!flyby_is_viable(tf->prev->v_arr, tf->v_dep, tf->prev->v_body, tf->body, tf->prev->date)) cairo_set_source_rgb(cr, 1, 0, 0);
	}

	int steps = 1000;
	Vector3 r = prev->r;
	Vector3 v = tf->v_dep;
	struct OSV last_osv = {r,v};

	for(int i = 1; i <= steps; i++) {
		double time = dt/steps * i;
		struct OSV osv = propagate_osv_time((OSV){r,v},attractor,time);
		// y negative, because in GUI y gets bigger downwards
		Vector2 p1 = {last_osv.r.x, -last_osv.r.y};
		Vector2 p2 = {osv.r.x, -osv.r.y};
		
		p1 = scale_vec2(p1,scale);
		p2 = scale_vec2(p2,scale);
		
		draw_stroke(cr, add_vec2(p1,center), add_vec2(p2,center));
		last_osv = osv;
	}
}
