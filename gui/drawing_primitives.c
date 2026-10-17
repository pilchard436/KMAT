#include "drawing.h"
#include <math.h>

static double stroke_width = 1;

void set_stroke_width(double width) {
	stroke_width = width;
}

void draw_stroke(cairo_t *cr, Vector2 p1, Vector2 p2) {
	cairo_set_line_width(cr, stroke_width);
	cairo_move_to(cr, p1.x, p1.y);
	cairo_line_to(cr, p2.x, p2.y);
	cairo_stroke(cr);
}

double calc_scale(int area_width, int area_height, struct Body *farthest_body) {
	if(farthest_body == NULL) return 1e-9;
	double apoapsis = calc_orbit_apoapsis(farthest_body->orbit);
	int wh = area_width < area_height ? area_width : area_height;
	return 1/apoapsis*wh/2.2;	// divided by 2.2 because apoapsis is only one side and buffer
}

void set_cairo_body_color(cairo_t *cr, struct Body *body) {
	if(body != NULL) {
		cairo_set_source_rgb(cr, body->color[0], body->color[1], body->color[2]);
	} else cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
}

void draw_center_aligned_text(cairo_t *cr, double x, double y, char *text) {
	// Calculate text width
	cairo_text_extents_t extents;
	cairo_text_extents(cr, text, &extents);
	double text_width = extents.width;

	// Position and draw text to the left
	cairo_move_to(cr, x - text_width/2, y); // adjust starting position to the left
	cairo_show_text(cr, text);
}

void draw_right_aligned_text(cairo_t *cr, double x, double y, double rotate_angle, char *text) {
	// Calculate text width
	cairo_text_extents_t extents;
	cairo_text_extents(cr, text, &extents);
	double text_width = extents.width;

	// Position and draw text to the left
	cairo_move_to(cr, x - text_width*cos(rotate_angle), y+sin(rotate_angle)*(text_width-5)); // adjust starting position to the left
	cairo_rotate(cr, -rotate_angle);
	cairo_show_text(cr, text);
	cairo_rotate(cr, rotate_angle);
}
