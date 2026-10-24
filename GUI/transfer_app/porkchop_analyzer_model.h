#ifndef KMAT_PORKCHOP_ANALYZER_MODEL_H
#define KMAT_PORKCHOP_ANALYZER_MODEL_H

#include "gui/transfer_app/porkchop_analyzer_tools.h"

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

int find_first_visible_porkchop_point(const struct PorkchopAnalyzerPoint *points, int num_points);
// Shows or hides every group that has an itinerary inside the filter; other groups keep their state.
// Returns how many groups changed.
int set_porkchop_groups_visibility(struct PorkchopGroup *groups, int num_groups, int show);
void mark_itinerary_bodies_visible(int *visible, size_t num_bodies, const struct ItinStep *selected_step, const CelestSystem *system);
int porkchop_plot_range_is_valid(const struct PorkchopPlotRange *range);
void porkchop_plot_pan(struct PorkchopPlotRange *range, const struct PorkchopPlotRange *bounds,
		double mouse_dx_fraction, double mouse_dy_fraction);
void porkchop_plot_zoom(struct PorkchopPlotRange *range, const struct PorkchopPlotRange *bounds, double factor,
		double anchor_x_fraction, double anchor_y_fraction);
int porkchop_plot_zoom_to_box(struct PorkchopPlotRange *range, double x0_fraction, double x1_fraction,
		double y0_fraction, double y1_fraction);
int porkchop_plot_value_at_fraction(const struct PorkchopPlotRange *range, double x_fraction, double y_fraction,
		double *x_value, double *y_value);

#endif