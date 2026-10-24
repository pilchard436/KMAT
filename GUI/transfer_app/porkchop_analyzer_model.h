#ifndef KMAT_PORKCHOP_ANALYZER_MODEL_H
#define KMAT_PORKCHOP_ANALYZER_MODEL_H

#include "gui/transfer_app/porkchop_analyzer_tools.h"

enum PaMinMaxType {PA_DEP, PA_ARR, PA_DUR, PA_TOTDV, PA_DEPDV, PA_SATDV};
enum PaYAxisType {PA_YAXIS_DUR, PA_YAXIS_ARRDATE};

struct PorkchopPlotRange {
	double min_x;
	double max_x;
	double min_y;
	double max_y;
};

struct PorkchopPlotView {
	struct PorkchopPlotRange range;
	struct PorkchopPlotRange data_range;
	int initialized;
};

enum PorkchopDirection {PORKCHOP_LEFT, PORKCHOP_RIGHT, PORKCHOP_UP, PORKCHOP_DOWN};

int find_first_visible_porkchop_point(const struct PorkchopAnalyzerPoint *points, int num_points);
int porkchop_axis_filter_field_is_active(enum PaMinMaxType field, enum PaYAxisType axis);
int find_best_porkchop_point_in_view(const struct PorkchopAnalyzerPoint *points, int num_points,
		enum LastTransferType last_transfer_type, int arrival_date_axis, const struct PorkchopPlotRange *range);
int find_directional_porkchop_neighbor(const struct PorkchopAnalyzerPoint *points, int num_points,
		const struct ItinStep *selected_arrival, enum PorkchopDirection direction, int arrival_date_axis,
		const struct PorkchopPlotRange *range, double plot_width, double plot_height);
int find_nearest_porkchop_point(const struct PorkchopAnalyzerPoint *points, int num_points,
		int arrival_date_axis, const struct PorkchopPlotRange *range, double left, double right, double bottom,
		double mouse_x, double mouse_y, double max_distance);
// Shows or hides every group that has an itinerary inside the filter; other groups keep their state.
// Returns how many groups changed.
int set_porkchop_groups_visibility(struct PorkchopGroup *groups, int num_groups, int show);
void mark_itinerary_bodies_visible(int *visible, size_t num_bodies, const struct ItinStep *selected_step, const CelestSystem *system);
int porkchop_plot_range_is_valid(const struct PorkchopPlotRange *range);
int porkchop_plot_switch_y_axis(struct PorkchopPlotRange *range,
		enum PaYAxisType current_axis, enum PaYAxisType next_axis);
void porkchop_plot_pan(struct PorkchopPlotRange *range, const struct PorkchopPlotRange *bounds,
		double mouse_dx_fraction, double mouse_dy_fraction);
void porkchop_plot_zoom(struct PorkchopPlotRange *range, const struct PorkchopPlotRange *bounds, double factor,
		double anchor_x_fraction, double anchor_y_fraction);
int porkchop_plot_zoom_to_box(struct PorkchopPlotRange *range, double x0_fraction, double x1_fraction,
		double y0_fraction, double y1_fraction);
int porkchop_plot_value_at_fraction(const struct PorkchopPlotRange *range, double x_fraction, double y_fraction,
		double *x_value, double *y_value);

#endif