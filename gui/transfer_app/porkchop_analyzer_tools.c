#include "porkchop_analyzer_tools.h"
#include "gui/drawing.h"
#include <stdlib.h>



static enum LastTransferType sort_last_transfer_type;

static double porkchop_total_dv(struct PorkchopAnalyzerPoint point) {
	double total_dv = point.data.dv_dep + point.data.dv_dsm;
	if(sort_last_transfer_type == TF_CAPTURE) total_dv += point.data.dv_arr_cap;
	if(sort_last_transfer_type == TF_CIRC) total_dv += point.data.dv_arr_circ;
	return total_dv;
}

static int compare_porkchop_points(const void *left, const void *right) {
	double left_dv = porkchop_total_dv(*(const struct PorkchopAnalyzerPoint *)left);
	double right_dv = porkchop_total_dv(*(const struct PorkchopAnalyzerPoint *)right);
	return (left_dv > right_dv) - (left_dv < right_dv);
}

void sort_porkchop(struct PorkchopAnalyzerPoint *pp, int num_itins, enum LastTransferType last_transfer_type) {
	sort_last_transfer_type = last_transfer_type;
	qsort(pp, num_itins, sizeof(struct PorkchopAnalyzerPoint), compare_porkchop_points);
}

void get_min_max_dep_arr_dur_range_from_mouse_rect(double *p_x0, double *p_x1, double *p_y0, double *p_y1, double min_x_val, double max_x_val, double min_y_val, double max_y_val, double screen_width, double screen_height, int dur0arrdate1) {
	double x0 = *p_x0, x1 = *p_x1, y0 = *p_y0, y1 = *p_y1;

	int min_x = dur0arrdate1 ? get_porkchop_arrdate_yaxis_x() : get_porkchop_dur_yaxis_x();
	int min_y = get_porkchop_xaxis_y();
	
	if(x0 < min_x) x0 = min_x;
	if(x1 < min_x) x1 = min_x;
	if(x0 > screen_width) x0 = screen_width;
	if(x1 > screen_width) x1 = screen_width;

	if(y0 < 0) y0 = 0;
	if(y1 < 0) y1 = 0;
	if(y0 > screen_height-min_y) y0 = screen_height-min_y;
	if(y1 > screen_height-min_y) y1 = screen_height-min_y;

	if(x0 > x1) { double temp = x0; x0 = x1; x1 = temp;	}
	if(y0 > y1) { double temp = y0; y0 = y1; y1 = temp; }

	x0 -= min_x;
	x1 -= min_x;
	x0 /= (screen_width-min_x);
	x1 /= (screen_width-min_x);
	y0 /= (screen_height-min_y);
	y1 /= (screen_height-min_y);

	double ddate = max_x_val - min_x_val;
	double ddur = max_y_val - min_y_val;

	// below from porkchop drawing...

	double margin = 0.05;
	
	min_x_val = min_x_val - ddate*margin;
	max_x_val = max_x_val + ddate*margin;
	min_y_val = min_y_val - ddur*margin;
	max_y_val = max_y_val + ddur*margin;

	x0 = x0*(max_x_val - min_x_val) + min_x_val;
	x1 = x1*(max_x_val - min_x_val) + min_x_val;
	y0 = (1-y0)*(max_y_val - min_y_val) + min_y_val;
	y1 = (1-y1)*(max_y_val - min_y_val) + min_y_val;
	
	*p_x0 = x0, *p_x1 = x1, *p_y0 = y0, *p_y1 = y1;
}
