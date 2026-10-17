#include "drawing.h"
#include "settings.h"
#include <math.h>
#include <stdio.h>

void get_axis_ticks(enum CoordAxisLabelType axis_label_type, int num_labels, double min_value, double max_value, double *p_value_tick, Datetime *p_date_tick, double *p_min_label) {
	if(axis_label_type == COORD_LABEL_NUMBER || axis_label_type == COORD_LABEL_DURATION) {
		double tick_units[] = {1,2,5};
		double tick = tick_units[0];
		double tick_scale = 1e-9;
		do {
			for(int i = 0; i < sizeof(tick_units)/sizeof(tick_units[0]); i++) {
				tick = tick_units[i] * tick_scale;
				if(num_labels*tick > (max_value - min_value)) break;
			}
			tick_scale *= 10;
		} while(num_labels*tick < (max_value - min_value));
		*p_value_tick = tick;
		*p_min_label = (min_value == 0) ? tick : ceil(min_value/tick)*tick;
	} else if(axis_label_type == COORD_LABEL_DATE) {
		Datetime tick = {0, 0, 1};
		double min_label = 0;
		for(int i = 0; i < 3; i++) {
			if(get_settings_datetime_type() != DATE_ISO && i == 1) continue; // kerbal date doesn't have months
			tick = i == 0 ? (Datetime){0, 0, 1} : i == 1 ? (Datetime) {0, 1, 0} : (Datetime) {1, 0, 0};
			int tick_scale = 0;
			bool found_valid_tick = false;
			do {
				switch(tick_scale) {
					case 0: tick_scale = 1; break;
					case 1: tick_scale = 2; break;
					case 2: tick_scale = i!=1 ? 5 : 6; break;
					case 5: tick_scale = 10; break;
					case 10: tick_scale = 20; break;
					case 20: tick_scale = 50; break;
					case 50: tick_scale = 100; break;
					default: tick_scale *= 2;
				}
				tick = (Datetime) {(tick.y != 0)*tick_scale, (tick.m != 0)*tick_scale, (tick.d != 0)*tick_scale};
				Datetime min_date = convert_JD_date(min_value, get_settings_datetime_type());
				Datetime min_x_date_label = (Datetime) {
						min_date.y,
						tick.y == 0 && tick.m <6 ? min_date.m : 1,
										(tick.y == 0 && tick.m == 0) ? min_date.d : 1,
								.date_type = get_settings_datetime_type()
				};
				min_label = convert_date_JD(min_x_date_label);
				min_label = jd_change_date(
						min_label,
						tick.y,
						tick.m,
						tick.d,
						get_settings_datetime_type()
				);
				double max_label = jd_change_date(
						min_label,
						tick.y*num_labels,
						tick.m*num_labels,
						tick.d*num_labels,
						get_settings_datetime_type()
				);
				if(max_label > max_value) { found_valid_tick = true; break;}
			} while((tick.d < 10 || get_settings_datetime_type() != DATE_ISO) && tick.d < 100 && tick.m < 6);
			if(found_valid_tick) break;
		}
		*p_date_tick = tick;
		*p_min_label = min_label;
	}
}

void draw_coordinate_system(cairo_t *cr, double width, double height, enum CoordAxisLabelType x_axis_label_type, enum CoordAxisLabelType y_axis_label_type,
		double min_x, double max_x, double min_y, double max_y, Vector2 origin, int num_x_labels, int num_y_labels) {
	// Set text color
	cairo_set_source_rgb(cr, 1, 1, 1);
	// Set font options
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 12.0);
	int half_font_size = 5;

	// axes
	draw_stroke(cr, vec2(origin.x, 0), vec2(origin.x, origin.y));
	draw_stroke(cr, vec2(origin.x, origin.y), vec2(width, origin.y));

	double y_label_tick, x_label_tick;
	double min_x_label, min_y_label;
	Datetime x_date_label_tick, y_date_label_tick;
	
	if(x_axis_label_type != COORD_LABEL_DATE) {
		get_axis_ticks(x_axis_label_type, num_x_labels, min_x, max_x, &x_label_tick, NULL, &min_x_label);
	} else {
		get_axis_ticks(x_axis_label_type, num_x_labels, min_x, max_x, NULL, &x_date_label_tick, &min_x_label);
	}
	
	if(y_axis_label_type != COORD_LABEL_DATE) {
		get_axis_ticks(y_axis_label_type, num_y_labels, min_y, max_y, &y_label_tick, NULL, &min_y_label);
	} else {
		get_axis_ticks(y_axis_label_type, num_y_labels, min_y, max_y, NULL, &y_date_label_tick, &min_y_label);
	}

	// gradients
	double m_y, m_x;
	m_x = (width-origin.x)/(max_x - min_x);
	m_y = -origin.y/(max_y - min_y); // negative, because positive is down

	double y_label_x = origin.x-5,	x_label_y = origin.y + 20;

	// x-labels and x grid
	char string[32];
	for(int i = 0; i < num_x_labels; i++) {
		double label = x_axis_label_type != COORD_LABEL_DATE ?
				min_x_label + i * x_label_tick :
				jd_change_date(min_x_label, i*x_date_label_tick.y, i*x_date_label_tick.m, i*x_date_label_tick.d, get_settings_datetime_type());
		// Labels past the axis end would run into the area beside the plot.
		if(label < min_x || label > max_x + 1e-9 * (max_x - min_x)) continue;
		double x = (label-min_x)*m_x + origin.x;
		cairo_set_source_rgb(cr, 1, 1, 1);
		if(x_axis_label_type == COORD_LABEL_NUMBER)
			sprintf(string, "%g", label);
		else if(x_axis_label_type == COORD_LABEL_DURATION)
			sprintf(string, "%g", get_settings_datetime_type() == DATE_KERBAL ? label*4 : label);
		else if(x_axis_label_type == COORD_LABEL_DATE)
			date_to_string(convert_JD_date(label, get_settings_datetime_type()), string, 0);
		draw_center_aligned_text(cr, x, x_label_y, string);
		cairo_set_source_rgb(cr, 0, 0, 0);
		draw_stroke(cr, vec2(x, origin.y), vec2(x, 0));
	}

	// y-labels and y grid
	for(int i = 0; i < num_y_labels; i++) {
		double label = y_axis_label_type != COORD_LABEL_DATE ?
					   min_y_label + i * y_label_tick :
					   jd_change_date(min_y_label, i*y_date_label_tick.y, i*y_date_label_tick.m, i*y_date_label_tick.d, get_settings_datetime_type());
		if(label < min_y || label > max_y + 1e-9 * (max_y - min_y)) continue;
		double y = (label-min_y)*m_y + origin.y;
		cairo_set_source_rgb(cr, 1, 1, 1);
		if(y_axis_label_type == COORD_LABEL_NUMBER)
			sprintf(string, "%g", label);
		else if(y_axis_label_type == COORD_LABEL_DURATION)
			sprintf(string, "%g", get_settings_datetime_type() == DATE_KERBAL ? label*4 : label);
		else if(y_axis_label_type == COORD_LABEL_DATE)
			date_to_string(convert_JD_date(label, get_settings_datetime_type()), string, 0);
		draw_right_aligned_text(cr, y_label_x, y+half_font_size, M_PI/4, string);
		cairo_set_source_rgb(cr, 0, 0, 0);
		draw_stroke(cr, vec2(origin.x, y), vec2(width, y));
	}
}

void draw_data_point(cairo_t *cr, double x, double y, double radius) {
	cairo_arc(cr, x, y, radius, 0, M_PI*2);
	cairo_fill(cr);
}

// The best route, semi-transparent so its own dot shows through.
static void draw_best_route_marker(cairo_t *cr, double x, double y) {
	const double arm = 3.5;
	cairo_set_source_rgba(cr, 1, 0, 0, 0.6);
	cairo_set_line_width(cr, 1.5);
	cairo_move_to(cr, x - arm, y - arm);
	cairo_line_to(cr, x + arm, y + arm);
	cairo_move_to(cr, x - arm, y + arm);
	cairo_line_to(cr, x + arm, y - arm);
	cairo_stroke(cr);
}

const int porkchop_dur_yaxis_x = 40;
const int porkchop_arrdate_yaxis_x = 60;
const int porkchop_xaxis_y = 40;
// Space right of the plot for the delta-v color scale.
const int porkchop_colorbar_width = 90;

// Point color from the relative delta-v (0 lowest, 1 highest); the same colors fill the scale.
static void porkchop_color(double bias, double *r, double *g, double *b) {
	*r = bias;
	*g = 1 - bias;
	*b = 4 * pow(bias - 0.5, 2);
}

static void draw_porkchop_color_scale(cairo_t *cr, double plot_width, double height, double min_dv, double max_dv) {
	double x = plot_width + 22, bar_width = 16, top = 30, bottom = height - porkchop_xaxis_y;
	// A darker panel with a border line keeps the scale visibly outside the plot.
	cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
	// Ends at the x axis so the last date label may extend under it.
	cairo_rectangle(cr, plot_width, 0, porkchop_colorbar_width + 1, height - porkchop_xaxis_y);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 1);
	cairo_move_to(cr, plot_width + 0.5, 0);
	cairo_line_to(cr, plot_width + 0.5, height - porkchop_xaxis_y);
	cairo_stroke(cr);
	if(!(bottom - top > 20) || !(max_dv > min_dv)) return;
	cairo_pattern_t *gradient = cairo_pattern_create_linear(0, bottom, 0, top);
	for(int i = 0; i <= 16; i++) {
		double r, g, b;
		porkchop_color(i / 16.0, &r, &g, &b);
		cairo_pattern_add_color_stop_rgb(gradient, i / 16.0, r, g, b);
	}
	cairo_rectangle(cr, x, top, bar_width, bottom - top);
	cairo_set_source(cr, gradient);
	cairo_fill(cr);
	cairo_pattern_destroy(gradient);

	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 12.0);
	cairo_rectangle(cr, x, top, bar_width, bottom - top);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	double tick, first_label;
	get_axis_ticks(COORD_LABEL_NUMBER, 6, min_dv, max_dv, &tick, NULL, &first_label);
	for(double label = first_label; label <= max_dv; label += tick) {
		double y = bottom - (label - min_dv) / (max_dv - min_dv) * (bottom - top);
		char text[32];
		snprintf(text, sizeof(text), "%.0f", label);
		cairo_move_to(cr, x + bar_width, y);
		cairo_line_to(cr, x + bar_width + 4, y);
		cairo_stroke(cr);
		cairo_move_to(cr, x + bar_width + 7, y + 4);
		cairo_show_text(cr, text);
		cairo_new_path(cr);
	}
	cairo_move_to(cr, x, top - 10);
	cairo_show_text(cr, "dv [m/s]");
	cairo_new_path(cr);
}

void draw_porkchop(cairo_t *cr, double width, double height, struct PorkchopAnalyzerPoint *porkchop, int num_itins, enum LastTransferType last_transfer_type, int dur0arrdate1) {
	double dv, depdate, dur, arrdate;

	double plot_width = width - porkchop_colorbar_width;
	Vector2 origin = {dur0arrdate1 ? porkchop_arrdate_yaxis_x : porkchop_dur_yaxis_x, height-porkchop_xaxis_y};

	int first_show_ind = 0;
	while(!porkchop[first_show_ind].inside_filter) first_show_ind++;

	struct PorkchopPoint pp = porkchop[first_show_ind].data;
	double dv_sat = pp.dv_dsm;
	if(last_transfer_type == TF_CAPTURE)	dv_sat += pp.dv_arr_cap;
	if(last_transfer_type == TF_CIRC)		dv_sat += pp.dv_arr_circ;

	double min_depdate = pp.dep_date, max_depdate = pp.dep_date;
	double min_dur = pp.dur, max_dur = pp.dur;
	double min_arrdate = pp.dep_date+pp.dur, max_arrdate = pp.dep_date+pp.dur;
	double min_dv = pp.dv_dep + dv_sat + pp.dv_flyby;
	double max_dv = pp.dv_dep + dv_sat + pp.dv_flyby;

	// find min and max
	for(int i = 1; i < num_itins; i++) {
		if(!porkchop[i].inside_filter) continue;
		pp = porkchop[i].data;

		dv_sat = pp.dv_dsm;
		if(last_transfer_type == TF_CAPTURE)	dv_sat += pp.dv_arr_cap;
		if(last_transfer_type == TF_CIRC)		dv_sat += pp.dv_arr_circ;

		dv = pp.dv_dep + dv_sat + pp.dv_flyby;
		depdate = pp.dep_date;
		dur = pp.dur;
		arrdate = pp.dep_date+pp.dur;
		

		if(dv < min_dv) min_dv = dv;
		else if(dv > max_dv) max_dv = dv;
		if(depdate < min_depdate) min_depdate = depdate;
		else if(depdate > max_depdate) max_depdate = depdate;
		if(dur < min_dur) min_dur = dur;
		else if(dur > max_dur) max_dur = dur;
		if(arrdate < min_arrdate) min_arrdate = arrdate;
		else if(arrdate > max_arrdate) max_arrdate = arrdate;
	}


	double ddepdate = max_depdate - min_depdate;
	double ddur = max_dur - min_dur;
	double darrdate = max_arrdate - min_arrdate;

	double margin = 0.05;

	min_depdate = min_depdate - ddepdate*margin;
	max_depdate = max_depdate + ddepdate*margin;
	min_dur = min_dur - ddur * margin;
	max_dur = max_dur + ddur * margin;
	min_arrdate = min_arrdate - darrdate * margin;
	max_arrdate = max_arrdate + darrdate * margin;

	// gradients
	double m_dur, m_depdate, m_arrdate;
	m_depdate = (plot_width - origin.x)/(max_depdate - min_depdate);
	m_arrdate = -origin.y/(max_arrdate - min_arrdate); // negative, because positive is down
	m_dur = -origin.y/(max_dur - min_dur); // negative, because positive is down

	if(dur0arrdate1) draw_coordinate_system(cr, plot_width, height, COORD_LABEL_DATE, COORD_LABEL_DATE, min_depdate, max_depdate, min_arrdate, max_arrdate, origin, 8, 10);
	else draw_coordinate_system(cr, plot_width, height, COORD_LABEL_DATE, COORD_LABEL_DURATION, min_depdate, max_depdate, min_dur, max_dur, origin, 8, 10);

	// find points to draw
	int *draw_idx = calloc(sizeof(int), num_itins);
	int num_draw_itins = 0;
	for(int i = 0; i < num_itins; i++) {
		if(porkchop[i].inside_filter && porkchop[i].group->show_group) {
			draw_idx[num_draw_itins] = i;
			num_draw_itins++;
		}
	}

	// Create an off-screen surface to draw porkchop all at onece
	cairo_surface_t* buffer_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int) width, (int) height);
	cairo_t* buffer_cr = cairo_create(buffer_surface);

	// Dot size falls smoothly from 5 px for a few points to 1.5 px for 100,000 or more.
	double point_radius = 5 - 3.5 * log10(fmax(num_draw_itins, 100) / 100.0) / 3;
	if(point_radius < 1.5) point_radius = 1.5;

	// draw points
	double color_bias;
	int i = num_draw_itins-1;
	while(i >= 0) {
		pp = porkchop[draw_idx[i]].data;

		dv_sat = pp.dv_dsm;
		if(last_transfer_type == TF_CAPTURE)	dv_sat += pp.dv_arr_cap;
		if(last_transfer_type == TF_CIRC)		dv_sat += pp.dv_arr_circ;

		dv = pp.dv_dep + dv_sat + pp.dv_flyby;
		depdate = pp.dep_date;
		dur = pp.dur;
		arrdate = pp.dep_date+pp.dur;

		// color coding
		color_bias = (dv - min_dv) / (max_dv - min_dv);
		double r, g, b;
		porkchop_color(color_bias, &r, &g, &b);
		cairo_set_source_rgb(buffer_cr, r,g,b);

		Vector2 data_point = vec2(origin.x + m_depdate*(depdate - min_depdate), origin.y + (dur0arrdate1 ? (m_arrdate*(arrdate - min_arrdate)) : (m_dur*(dur - min_dur))));
		draw_data_point(buffer_cr, data_point.x, data_point.y, point_radius);
		if(i == 0) draw_best_route_marker(buffer_cr, data_point.x, data_point.y);

		i--;
	}

	// Copy the buffer to the main canvas and buffer cleanup
	cairo_set_source_surface(cr, buffer_surface, 0, 0);
	cairo_paint(cr);
	cairo_destroy(buffer_cr);
	cairo_surface_destroy(buffer_surface);

	draw_porkchop_color_scale(cr, plot_width, height, min_dv, max_dv);

	free(draw_idx);
}

void draw_plot(cairo_t *cr, double width, double height, double *x, double *y, int num_points) {
	Vector2 origin = {60, height-30};

	double min_x = x[0], max_x = x[0];
	double min_y = y[0], max_y = y[0];
	if(min_y > 0) min_y = 0;

	// find min and max
	for(int i = 1; i < num_points; i++) {
		if(x[i] < min_x) min_x = x[i];
		else if(x[i] > max_x) max_x = x[i];
		if(y[i] < min_y) min_y = y[i];
		else if(y[i] > max_y) max_y = y[i];
	}

	double dx = max_x-min_x;
	double dy = max_y-min_y;


	double margin = 0.05;

	//min_x = min_x != 0 ? min_x - dx * margin : 0;
	//max_x = max_x != 0 ? max_x + dx * margin : 0;
	//dx = max_x-min_x;
	min_y = min_y != 0 ? min_y - dy * margin : 0;
	max_y = max_y != 0 ? max_y + dy * margin : 0;
	dy = max_y-min_y;

	// gradients
	double m_x, m_y;
	m_x = (width-origin.x)/dx;
	m_y = -origin.y/dy; // negative, because positive is down

	draw_coordinate_system(cr, width, height, COORD_LABEL_NUMBER, COORD_LABEL_NUMBER, min_x, max_x, min_y, max_y, origin, 10, 15);

	// data
	cairo_set_source_rgb(cr, 0, 0.8, 0.8);
	for(int i = 1; i < num_points; i++) {
		Vector2 point0 = vec2(origin.x + m_x*(x[i-1] - min_x), origin.y + m_y * (y[i-1] - min_y));
		Vector2 point1 = vec2(origin.x + m_x*(x[i  ] - min_x), origin.y + m_y * (y[i  ] - min_y));
		draw_stroke(cr, point0, point1);
	}
}

void draw_multi_plot(cairo_t *cr, double width, double height, double *x, double **y, int num_plots, int num_points) {
	Vector2 origin = {60, height-30};

	Vector3 colors[2] = {
			vec3(0, 0.8, 0.8),
			vec3(0.8, 0, 0.4),
	};

	double min_x = x[0], max_x = x[0];
	double min_y = y[0][0], max_y = y[0][0];
	if(min_y > 0) min_y = 0;

	// find min and max
	for(int i = 1; i < num_points; i++) {
		if(x[i] < min_x) min_x = x[i];
		else if(x[i] > max_x) max_x = x[i];
	}

	for(int p = 0; p < num_plots; p++) {
		for(int i = 0; i < num_points; i++) {
			if(y[p][i] < min_y) min_y = y[p][i];
			else if(y[p][i] > max_y) max_y = y[p][i];
		}
	}

	double dx = max_x-min_x;
	double dy = max_y-min_y;


	double margin = 0.05;

	//min_x = min_x != 0 ? min_x - dx * margin : 0;
	//max_x = max_x != 0 ? max_x + dx * margin : 0;
	//dx = max_x-min_x;
	min_y = min_y != 0 ? min_y - dy * margin : 0;
	max_y = max_y != 0 ? max_y + dy * margin : 0;
	dy = max_y-min_y;

	// gradients
	double m_x, m_y;
	m_x = (width-origin.x)/dx;
	m_y = -origin.y/dy; // negative, because positive is down

	draw_coordinate_system(cr, width, height, COORD_LABEL_NUMBER, COORD_LABEL_NUMBER, min_x, max_x, min_y, max_y, origin, 10, 15);

	// data
	for(int p = 0; p < num_plots; p++) {
		cairo_set_source_rgb(cr, colors[p].x, colors[p].y, colors[p].z);
		for(int i = 1; i < num_points; i++) {
			Vector2 point0 = vec2(origin.x + m_x * (x[i - 1] - min_x), origin.y + m_y * (y[p][i - 1] - min_y));
			Vector2 point1 = vec2(origin.x + m_x * (x[i] - min_x), origin.y + m_y * (y[p][i] - min_y));
			draw_stroke(cr, point0, point1);
		}
	}
}

int get_porkchop_dur_yaxis_x() {return porkchop_dur_yaxis_x;}
int get_porkchop_arrdate_yaxis_x() {return porkchop_arrdate_yaxis_x;}
int get_porkchop_xaxis_y() {return porkchop_xaxis_y;}
int get_porkchop_colorbar_width() {return porkchop_colorbar_width;}