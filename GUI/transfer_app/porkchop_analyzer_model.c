#include "gui/transfer_app/porkchop_analyzer_model.h"

#include <math.h>
#include <string.h>

int porkchop_plot_range_is_valid(const struct PorkchopPlotRange *range) {
	return range != NULL &&
		isfinite(range->min_x) && isfinite(range->max_x) &&
		isfinite(range->min_y) && isfinite(range->max_y) &&
		range->max_x > range->min_x && range->max_y > range->min_y;
}

int porkchop_plot_switch_y_axis(struct PorkchopPlotRange *range,
		enum PaYAxisType current_axis, enum PaYAxisType next_axis) {
	if(!porkchop_plot_range_is_valid(range) ||
		(current_axis != PA_YAXIS_DUR && current_axis != PA_YAXIS_ARRDATE) ||
		(next_axis != PA_YAXIS_DUR && next_axis != PA_YAXIS_ARRDATE)) return 0;
	if(current_axis == next_axis) return 1;

	double departure_midpoint = range->min_x + (range->max_x - range->min_x) / 2;
	double y_shift = next_axis == PA_YAXIS_ARRDATE ? departure_midpoint : -departure_midpoint;
	double min_y = range->min_y + y_shift;
	double max_y = range->max_y + y_shift;
	if(!isfinite(min_y) || !isfinite(max_y) || max_y <= min_y) return 0;
	range->min_y = min_y;
	range->max_y = max_y;
	return 1;
}

static void constrain_plot_axis(double *min_value, double *max_value, double bound_min, double bound_max) {
	double span = *max_value - *min_value;
	double bound_span = bound_max - bound_min;
	if(span >= bound_span) {
		*min_value = bound_min;
		*max_value = bound_max;
		return;
	}

	if(*min_value < bound_min) {
		*max_value += bound_min - *min_value;
		*min_value = bound_min;
	}
	if(*max_value > bound_max) {
		*min_value -= *max_value - bound_max;
		*max_value = bound_max;
	}
}

static void pan_plot_axis(double *min_value, double *max_value, double bound_min, double bound_max,
		double shift) {
	*min_value += shift;
	*max_value += shift;
	constrain_plot_axis(min_value, max_value, bound_min, bound_max);
}

void porkchop_plot_pan(struct PorkchopPlotRange *range, const struct PorkchopPlotRange *bounds,
		double mouse_dx_fraction, double mouse_dy_fraction) {
	if(!porkchop_plot_range_is_valid(range) || !porkchop_plot_range_is_valid(bounds) ||
		!isfinite(mouse_dx_fraction) || !isfinite(mouse_dy_fraction)) return;

	double x_shift = -mouse_dx_fraction * (range->max_x - range->min_x);
	double y_shift = mouse_dy_fraction * (range->max_y - range->min_y);
	pan_plot_axis(&range->min_x, &range->max_x, bounds->min_x, bounds->max_x, x_shift);
	pan_plot_axis(&range->min_y, &range->max_y, bounds->min_y, bounds->max_y, y_shift);
}

void porkchop_plot_zoom(struct PorkchopPlotRange *range, const struct PorkchopPlotRange *bounds, double factor,
		double anchor_x_fraction, double anchor_y_fraction) {
	if(!porkchop_plot_range_is_valid(range) || !porkchop_plot_range_is_valid(bounds) ||
		!isfinite(factor) || factor <= 0 ||
		!isfinite(anchor_x_fraction) || !isfinite(anchor_y_fraction) ||
		anchor_x_fraction < 0 || anchor_x_fraction > 1 ||
		anchor_y_fraction < 0 || anchor_y_fraction > 1) return;

	double x_span = (range->max_x - range->min_x) * factor;
	double y_span = (range->max_y - range->min_y) * factor;
	if(!isfinite(x_span) || !isfinite(y_span) || x_span <= 0 || y_span <= 0) return;

	double anchor_x = range->min_x + anchor_x_fraction * (range->max_x - range->min_x);
	double anchor_y = range->min_y + anchor_y_fraction * (range->max_y - range->min_y);
	range->min_x = anchor_x - anchor_x_fraction * x_span;
	range->max_x = range->min_x + x_span;
	range->min_y = anchor_y - anchor_y_fraction * y_span;
	range->max_y = range->min_y + y_span;
	constrain_plot_axis(&range->min_x, &range->max_x, bounds->min_x, bounds->max_x);
	constrain_plot_axis(&range->min_y, &range->max_y, bounds->min_y, bounds->max_y);
}

int porkchop_plot_zoom_to_box(struct PorkchopPlotRange *range, double x0_fraction, double x1_fraction,
		double y0_fraction, double y1_fraction) {
	if(!porkchop_plot_range_is_valid(range) ||
		!isfinite(x0_fraction) || !isfinite(x1_fraction) ||
		!isfinite(y0_fraction) || !isfinite(y1_fraction) ||
		x0_fraction < 0 || x0_fraction > 1 || x1_fraction < 0 || x1_fraction > 1 ||
		y0_fraction < 0 || y0_fraction > 1 || y1_fraction < 0 || y1_fraction > 1 ||
		x0_fraction == x1_fraction || y0_fraction == y1_fraction) return 0;

	double min_x_fraction = fmin(x0_fraction, x1_fraction);
	double max_x_fraction = fmax(x0_fraction, x1_fraction);
	double min_y_fraction = fmin(y0_fraction, y1_fraction);
	double max_y_fraction = fmax(y0_fraction, y1_fraction);
	struct PorkchopPlotRange original = *range;
	range->min_x = original.min_x + min_x_fraction * (original.max_x - original.min_x);
	range->max_x = original.min_x + max_x_fraction * (original.max_x - original.min_x);
	range->min_y = original.min_y + min_y_fraction * (original.max_y - original.min_y);
	range->max_y = original.min_y + max_y_fraction * (original.max_y - original.min_y);
	return porkchop_plot_range_is_valid(range);
}

int porkchop_plot_value_at_fraction(const struct PorkchopPlotRange *range, double x_fraction, double y_fraction,
		double *x_value, double *y_value) {
	if(!porkchop_plot_range_is_valid(range) || x_value == NULL || y_value == NULL ||
		!isfinite(x_fraction) || !isfinite(y_fraction) ||
		x_fraction < 0 || x_fraction > 1 || y_fraction < 0 || y_fraction > 1) return 0;

	*x_value = range->min_x + x_fraction * (range->max_x - range->min_x);
	*y_value = range->min_y + y_fraction * (range->max_y - range->min_y);
	return 1;
}

int find_first_visible_porkchop_point(const struct PorkchopAnalyzerPoint *points, int num_points) {
	if(points == NULL || num_points <= 0) return -1;
	for(int i = 0; i < num_points; i++) {
		if(points[i].inside_filter && points[i].group != NULL && points[i].group->show_group)
			return i;
	}
	return -1;
}

int porkchop_axis_filter_field_is_active(enum PaMinMaxType field, enum PaYAxisType axis) {
	if(field == PA_ARR) return axis == PA_YAXIS_ARRDATE;
	if(field == PA_DUR) return axis == PA_YAXIS_DUR;
	return 1;
}

int find_best_porkchop_point_in_view(const struct PorkchopAnalyzerPoint *points, int num_points,
		enum LastTransferType last_transfer_type, int arrival_date_axis, const struct PorkchopPlotRange *range) {
	if(points == NULL || num_points <= 0 || !porkchop_plot_range_is_valid(range)) return -1;
	int best_index = -1;
	double best_dv = INFINITY;
	for(int i = 0; i < num_points; i++) {
		if(!points[i].inside_filter || points[i].group == NULL || !points[i].group->show_group) continue;
		struct PorkchopPoint point = points[i].data;
		double y_value = arrival_date_axis ? point.dep_date + point.dur : point.dur;
		if(point.dep_date < range->min_x || point.dep_date > range->max_x ||
			y_value < range->min_y || y_value > range->max_y) continue;
		double total_dv = porkchop_point_total_dv(point, last_transfer_type);
		if(isfinite(total_dv) && total_dv < best_dv) {
			best_index = i;
			best_dv = total_dv;
		}
	}
	return best_index;
}

int find_directional_porkchop_neighbor(const struct PorkchopAnalyzerPoint *points, int num_points,
		const struct ItinStep *selected_arrival, enum PorkchopDirection direction, int arrival_date_axis,
		const struct PorkchopPlotRange *range, double plot_width, double plot_height) {
	if(points == NULL || num_points <= 0 || selected_arrival == NULL ||
		direction < PORKCHOP_LEFT || direction > PORKCHOP_DOWN ||
		!porkchop_plot_range_is_valid(range) || !isfinite(plot_width) || !isfinite(plot_height) ||
		plot_width <= 0 || plot_height <= 0) return -1;

	const struct PorkchopPoint *selected_point = NULL;
	for(int i = 0; i < num_points; i++) {
		if(points[i].data.arrival == selected_arrival) {
			selected_point = &points[i].data;
			break;
		}
	}
	if(selected_point == NULL) return -1;

	double x_span = range->max_x - range->min_x;
	double y_span = range->max_y - range->min_y;
	double selected_y = arrival_date_axis ?
			selected_point->dep_date + selected_point->dur : selected_point->dur;
	double selected_x_pixel = (selected_point->dep_date - range->min_x) / x_span * plot_width;
	double selected_y_pixel = -(selected_y - range->min_y) / y_span * plot_height;
	double best_cardinal_distance_squared = INFINITY;
	double best_any_distance_squared = INFINITY;
	int best_cardinal_index = -1;
	int best_any_index = -1;

	for(int i = 0; i < num_points; i++) {
		if(!points[i].inside_filter || points[i].group == NULL || !points[i].group->show_group ||
			points[i].data.arrival == selected_arrival) continue;
		const struct PorkchopPoint *candidate = &points[i].data;
		double candidate_y = arrival_date_axis ?
				candidate->dep_date + candidate->dur : candidate->dur;
		if(candidate->dep_date < range->min_x || candidate->dep_date > range->max_x ||
			candidate_y < range->min_y || candidate_y > range->max_y) continue;

		double dx = (candidate->dep_date - range->min_x) / x_span * plot_width - selected_x_pixel;
		double dy = -(candidate_y - range->min_y) / y_span * plot_height - selected_y_pixel;
		if((direction == PORKCHOP_LEFT && dx >= 0) ||
			(direction == PORKCHOP_RIGHT && dx <= 0) ||
			(direction == PORKCHOP_UP && dy >= 0) ||
			(direction == PORKCHOP_DOWN && dy <= 0)) continue;
		double distance_squared = dx * dx + dy * dy;
		if(distance_squared < best_any_distance_squared) {
			best_any_distance_squared = distance_squared;
			best_any_index = i;
		}
		double primary_distance = direction == PORKCHOP_LEFT || direction == PORKCHOP_RIGHT ?
				fabs(dx) : fabs(dy);
		double secondary_distance = direction == PORKCHOP_LEFT || direction == PORKCHOP_RIGHT ?
				fabs(dy) : fabs(dx);
		if(primary_distance >= 4 * secondary_distance &&
			distance_squared < best_cardinal_distance_squared) {
			best_cardinal_distance_squared = distance_squared;
			best_cardinal_index = i;
		}
	}
	return best_cardinal_index >= 0 ? best_cardinal_index : best_any_index;
}

int find_nearest_porkchop_point(const struct PorkchopAnalyzerPoint *points, int num_points,
		int arrival_date_axis, const struct PorkchopPlotRange *range, double left, double right, double bottom,
		double mouse_x, double mouse_y, double max_distance) {
	if(points == NULL || num_points <= 0 || !porkchop_plot_range_is_valid(range) ||
		!isfinite(left) || !isfinite(right) || !isfinite(bottom) ||
		!isfinite(mouse_x) || !isfinite(mouse_y) || !isfinite(max_distance) ||
		right <= left || bottom <= 0 || max_distance < 0) return -1;
	double x_span = range->max_x - range->min_x;
	double y_span = range->max_y - range->min_y;
	double nearest_distance_squared = max_distance * max_distance;
	int nearest_index = -1;
	for(int i = 0; i < num_points; i++) {
		if(!points[i].inside_filter || points[i].group == NULL || !points[i].group->show_group) continue;
		struct PorkchopPoint point = points[i].data;
		double plot_y = arrival_date_axis ? point.dep_date + point.dur : point.dur;
		double point_x = left + (point.dep_date - range->min_x) / x_span * (right - left);
		double point_y = bottom - (plot_y - range->min_y) / y_span * bottom;
		if(point_x < left || point_x > right || point_y < 0 || point_y > bottom) continue;
		double dx = point_x - mouse_x;
		double dy = point_y - mouse_y;
		double distance_squared = dx * dx + dy * dy;
		if(distance_squared <= nearest_distance_squared) {
			nearest_index = i;
			nearest_distance_squared = distance_squared;
		}
	}
	return nearest_index;
}

void mark_itinerary_bodies_visible(int *visible, size_t num_bodies, const struct ItinStep *selected_step, const CelestSystem *system) {
	if(visible == NULL || system == NULL) return;
	memset(visible, 0, num_bodies * sizeof(*visible));
	if(system->num_bodies <= 0 || system->bodies == NULL) return;

	while(selected_step != NULL) {
		if(selected_step->body != NULL) {
			for(size_t body_index = 0; body_index < num_bodies && body_index < (size_t)system->num_bodies; body_index++) {
				if(system->bodies[body_index] == selected_step->body)
					visible[body_index] = 1;
			}
		}
		selected_step = selected_step->prev;
	}
}

int set_porkchop_groups_visibility(struct PorkchopGroup *groups, int num_groups, int show) {
        if(groups == NULL) return 0;
        int changed = 0;
        for(int group_idx = 0; group_idx < num_groups; group_idx++) {
                if(!groups[group_idx].has_itin_inside_filter) continue;
                changed += groups[group_idx].show_group != show;
                groups[group_idx].show_group = show;
        }
        return changed;
}
