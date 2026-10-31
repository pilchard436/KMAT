#ifndef KSP_DRAWING_H
#define KSP_DRAWING_H

#include <cairo.h>
#include "tools/celestial_systems.h"
#include "orbit_calculator/itin_tool.h"
#include "transfer_app/porkchop_analyzer_tools.h"
#include "gui/gui_tools/camera.h"

struct PorkchopPlotView;


enum CoordAxisLabelType {COORD_LABEL_NUMBER, COORD_LABEL_DATE, COORD_LABEL_DURATION};

void draw_stroke(cairo_t *cr, Vector2 p1, Vector2 p2);
// Line width used by draw_stroke; it stays set until changed.
void set_stroke_width(double width);
void draw_center_aligned_text(cairo_t *cr, double x, double y, char *text);
void draw_right_aligned_text(cairo_t *cr, double x, double y, double rotate_angle, char *text);
void draw_celestial_system(Camera *camera, CelestSystem *system, double jd_date);
void draw_body(Camera *camera, CelestSystem *system, Body *body, double jd_date);
// The moons of a planet with their faint orbits; positions follow the planet.
void draw_body_moons(Camera *camera, CelestSystem *system, Body *planet, double jd_date, int (*is_visible)(Body *moon));
void register_body_occluders(CelestSystem *system, double jd_date);
void draw_orbit(Camera *camera, Orbit orbit);
void draw_itinerary(Camera *camera, CelestSystem *system, struct ItinStep *tf, double current_time);
// The itinerary as conics relative to one body (its flyby, departure or arrival hyperbola), drawn around the body's position.
// The departure and arrival periapsis are altitudes above the body's surface.
void draw_itinerary_around_body(Camera *camera, CelestSystem *system, struct ItinStep *itin, Body *planet, double current_time,
		double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type);
// The route's path relative to a moon of the flyby planet, with the closest approach marked (and flagged on impact).
void draw_itinerary_around_moon(Camera *camera, CelestSystem *system, struct ItinStep *itin, Body *moon, double current_time,
		double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type);
// Where the spacecraft is on the itinerary at a date; 0 if there is no itinerary.
int itinerary_spacecraft_position(CelestSystem *system, struct ItinStep *itin, double date, Vector3 *r);
void draw_itinerary_legend(Camera *camera, CelestSystem *system, struct ItinStep *tf, double current_time);
void draw_body_label(Camera *camera, CelestSystem *system, Body *body, double jd_date);
void draw_orbit_2d(cairo_t *cr, Vector2 center, double scale, Vector3 r, Vector3 v, Body *attractor);
void draw_body_2d(cairo_t *cr, Vector2 center, double scale, Vector3 r);
double calc_scale(int area_width, int area_height, Body *farthest_body);
void set_cairo_body_color(cairo_t *cr, Body *body);
void draw_transfer_point_2d(cairo_t *cr, Vector2 center, double scale, Vector3 r);
void draw_trajectory_2d(cairo_t *cr, Vector2 center, double scale, struct ItinStep *tf, Body *attractor);
int get_porkchop_dur_yaxis_x();
int get_porkchop_arrdate_yaxis_x();
int get_porkchop_xaxis_y();
int get_porkchop_colorbar_width();
void draw_porkchop(cairo_t *cr, double width, double height, struct PorkchopAnalyzerPoint *porkchop, int num_itins,
		enum LastTransferType last_transfer_type, int dur0arrdate1, struct PorkchopPlotView *view);
void draw_plot(cairo_t *cr, double width, double height, double *x, double *y, int num_points);
void draw_multi_plot(cairo_t *cr, double width, double height, double *x, double **y, int num_plots, int num_points);

#endif //KSP_DRAWING_H
