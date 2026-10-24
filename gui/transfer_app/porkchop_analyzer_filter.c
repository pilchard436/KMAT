#include "porkchop_analyzer_internal.h"
#include "tools/itinerary_date_input.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int pa_filter_entry_update_depth;

static gboolean pa_numeric_entry_is_valid(const char *text) {
	if(text == NULL || *text == '\0') return FALSE;
	char *end = NULL;
	double value = g_ascii_strtod(text, &end);
	while(end != NULL && g_ascii_isspace(*end)) end++;
	return end != text && end != NULL && *end == '\0' && isfinite(value) && value >= 0;
}

static gboolean pa_numeric_entry_is_valid_prefix(const char *text) {
	if(text == NULL) return FALSE;
	int has_decimal = 0;
	for(const char *character = text; *character != '\0'; character++) {
		if(g_ascii_isdigit(*character)) continue;
		if(*character == '.' && !has_decimal) {
			has_decimal = 1;
			continue;
		}
		return FALSE;
	}
	return TRUE;
}

static void on_pa_numeric_entry_insert_text(
		GtkEditable *editable, const gchar *new_text, gint new_text_length,
		gint *position, gpointer user_data) {
	(void)user_data;
	const char *current_text = gtk_entry_get_text(GTK_ENTRY(editable));
	gsize current_length = strlen(current_text);
	gsize insert_position = *position < 0 ? current_length : (gsize)*position;
	gsize insert_length = new_text_length < 0 ? strlen(new_text) : (gsize)new_text_length;
	if(insert_position > current_length) {
		g_signal_stop_emission_by_name(editable, "insert-text");
		return;
	}
	GString *candidate = g_string_sized_new(current_length + insert_length);
	g_string_append_len(candidate, current_text, insert_position);
	g_string_append_len(candidate, new_text, insert_length);
	g_string_append(candidate, current_text + insert_position);
	gboolean valid = pa_numeric_entry_is_valid_prefix(candidate->str);
	g_string_free(candidate, TRUE);
	if(!valid) g_signal_stop_emission_by_name(editable, "insert-text");
}

static void on_pa_numeric_entry_changed(GtkEditable *editable, gpointer user_data) {
	(void)user_data;
	GtkWidget *entry = GTK_WIDGET(editable);
	const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
	GtkStyleContext *style = gtk_widget_get_style_context(entry);
	if(pa_numeric_entry_is_valid(text)) {
		g_object_set_data_full(G_OBJECT(entry), "pa-last-valid-number", g_strdup(text), g_free);
		gtk_style_context_remove_class(style, "error");
	} else {
		gtk_style_context_add_class(style, "error");
	}
}

static gboolean on_pa_numeric_entry_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data) {
	(void)event;
	(void)user_data;
	const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
	if(!pa_numeric_entry_is_valid(text)) {
		const char *last_valid = g_object_get_data(G_OBJECT(widget), "pa-last-valid-number");
		gtk_entry_set_text(GTK_ENTRY(widget), last_valid != NULL ? last_valid : "");
	}
	return FALSE;
}

void connect_pa_filter_entry_validation(void) {
	for(int field = 0; field < 6; field++) {
		GtkEntry *entries[] = {
			GTK_ENTRY(tf_pa_min_feedback[field]),
			GTK_ENTRY(tf_pa_max_feedback[field])
		};
		for(size_t entry_index = 0; entry_index < G_N_ELEMENTS(entries); entry_index++) {
			if(field == PA_DEP || field == PA_ARR) {
				connect_itinerary_date_entry_validation(entries[entry_index]);
			} else {
				g_signal_connect(entries[entry_index], "insert-text",
						G_CALLBACK(on_pa_numeric_entry_insert_text), NULL);
				g_signal_connect(entries[entry_index], "changed",
						G_CALLBACK(on_pa_numeric_entry_changed), NULL);
				g_signal_connect(entries[entry_index], "focus-out-event",
						G_CALLBACK(on_pa_numeric_entry_focus_out), NULL);
				on_pa_numeric_entry_changed(GTK_EDITABLE(entries[entry_index]), NULL);
			}
		}
	}
}

void begin_pa_filter_entry_update(void) {
	pa_filter_entry_update_depth++;
}

void end_pa_filter_entry_update(void) {
	if(pa_filter_entry_update_depth > 0) pa_filter_entry_update_depth--;
}

void reset_min_max_feedback(int take_hidden_group_into_account) {
	if(pa_num_itins == 0) return;
	int cap = pa_last_transfer_type == TF_CAPTURE, circ = pa_last_transfer_type == TF_CIRC;

	int first_show_idx = 0;
	while(first_show_idx < pa_num_itins &&
		(!pa_porkchop_points[first_show_idx].inside_filter ||
		 (!take_hidden_group_into_account && !pa_porkchop_points[first_show_idx].group->show_group)))
		first_show_idx++;
	if(first_show_idx >= pa_num_itins) return;
	struct PorkchopPoint pp = pa_porkchop_points[first_show_idx].data;

	double min[6] = {
			/* depdate */ pp.dep_date,
			/* arrdate */ pp.dep_date + pp.dur,
			/* duration */ pp.dur,
			/* total dv */ pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_cap * cap + pp.dv_arr_circ * circ,
			/* dep dv */ pp.dv_dep,
			/* sat dv */ pp.dv_dsm + pp.dv_flyby + pp.dv_arr_cap * cap + pp.dv_arr_circ * circ,
	};
	double max[6];
	memcpy(max, min, sizeof(max));

	for(int i = first_show_idx + 1; i < pa_num_itins; i++) {
		if(!pa_porkchop_points[i].inside_filter ||
			(!take_hidden_group_into_account && !pa_porkchop_points[i].group->show_group)) continue;
		pp = pa_porkchop_points[i].data;
		double values[6] = {
			pp.dep_date,
			pp.dep_date + pp.dur,
			pp.dur,
			pp.dv_dep + pp.dv_dsm + pp.dv_flyby + pp.dv_arr_cap * cap + pp.dv_arr_circ * circ,
			pp.dv_dep,
			pp.dv_dsm + pp.dv_flyby + pp.dv_arr_cap * cap + pp.dv_arr_circ * circ,
		};
		for(int value_idx = 0; value_idx < 6; value_idx++) {
			min[value_idx] = MIN(min[value_idx], values[value_idx]);
			max[value_idx] = MAX(max[value_idx], values[value_idx]);
		}
	}

	char string[32];
	begin_pa_filter_entry_update();
	date_to_string(convert_JD_date(min[PA_DEP], get_settings_datetime_type()), string, 0);
	gtk_entry_set_text(GTK_ENTRY(tf_pa_min_feedback[PA_DEP]), string);
	date_to_string(convert_JD_date(min[PA_ARR], get_settings_datetime_type()), string, 0);
	gtk_entry_set_text(GTK_ENTRY(tf_pa_min_feedback[PA_ARR]), string);
	sprintf(string, "%.0f", get_settings_datetime_type() == DATE_KERBAL ? min[PA_DUR] * 4 : min[PA_DUR]);
	gtk_entry_set_text(GTK_ENTRY(tf_pa_min_feedback[PA_DUR]), string);
	for(int i = 3; i < 6; i++) {
		sprintf(string, "%.2f", min[i]);
		gtk_entry_set_text(GTK_ENTRY(tf_pa_min_feedback[i]), string);
	}
	date_to_string(convert_JD_date(max[PA_DEP], get_settings_datetime_type()), string, 0);
	gtk_entry_set_text(GTK_ENTRY(tf_pa_max_feedback[PA_DEP]), string);
	date_to_string(convert_JD_date(max[PA_ARR], get_settings_datetime_type()), string, 0);
	gtk_entry_set_text(GTK_ENTRY(tf_pa_max_feedback[PA_ARR]), string);
	sprintf(string, "%.0f", get_settings_datetime_type() == DATE_KERBAL ? max[PA_DUR] * 4 : max[PA_DUR]);
	gtk_entry_set_text(GTK_ENTRY(tf_pa_max_feedback[PA_DUR]), string);
	for(int i = 3; i < 6; i++) {
		sprintf(string, "%.2f", max[i]);
		gtk_entry_set_text(GTK_ENTRY(tf_pa_max_feedback[i]), string);
	}
	memcpy(pa_min_vals, min, sizeof(min));
	memcpy(pa_max_vals, max, sizeof(max));
	end_pa_filter_entry_update();
}

gboolean are_any_porkchop_points_in_filter(const double min[6], const double max[6]) {
	struct PorkchopPoint pp;
	for(int i = 0; i < pa_num_itins; i++) {
		pp = pa_porkchop_points[i].data;

		double dv_sat = pp.dv_dsm;
		if(pa_last_transfer_type == TF_CAPTURE)	dv_sat += pp.dv_arr_cap;
		if(pa_last_transfer_type == TF_CIRC)	dv_sat += pp.dv_arr_circ;

		if(	!(pp.dep_date 			< min[PA_DEP] || pp.dep_date 			> max[PA_DEP] ||
			  pp.dep_date+pp.dur 	< min[PA_ARR] || pp.dep_date+pp.dur 	> max[PA_ARR] ||
			  pp.dur 				< min[PA_DUR] || pp.dur 				> max[PA_DUR] ||
			  pp.dv_dep + dv_sat + pp.dv_flyby	< min[PA_TOTDV] || pp.dv_dep + dv_sat + pp.dv_flyby	> max[PA_TOTDV] ||
			  pp.dv_dep 			< min[PA_DEPDV] || pp.dv_dep 			> max[PA_DEPDV] ||
			  dv_sat + pp.dv_flyby 	< min[PA_SATDV] || dv_sat + pp.dv_flyby 	> max[PA_SATDV])) return TRUE;
	}
	return FALSE;
}

static int read_pa_filter_bounds(double min[6], double max[6]) {
	enum DateType date_type = get_settings_datetime_type();
	const int date_fields[] = {PA_DEP, PA_ARR};
	for(size_t i = 0; i < sizeof(date_fields) / sizeof(date_fields[0]); i++) {
		int field = date_fields[i];
		if(!porkchop_axis_filter_field_is_active(field, pa_yaxis_type)) {
			min[field] = -DBL_MAX;
			max[field] = DBL_MAX;
			continue;
		}
		const char *min_text = gtk_entry_get_text(GTK_ENTRY(tf_pa_min_feedback[field]));
		const char *max_text = gtk_entry_get_text(GTK_ENTRY(tf_pa_max_feedback[field]));
		if(!itinerary_date_entry_is_complete(min_text, date_type) ||
			!itinerary_date_entry_is_complete(max_text, date_type)) return 0;
		min[field] = convert_date_JD(date_from_string((char *)min_text, date_type)) - 1;
		max[field] = convert_date_JD(date_from_string((char *)max_text, date_type)) + 1;
	}

	for(int i = PA_DUR; i < 6; i++) {
		if(!porkchop_axis_filter_field_is_active((enum PaMinMaxType)i, pa_yaxis_type)) {
			min[i] = -DBL_MAX;
			max[i] = DBL_MAX;
			continue;
		}
		const char *min_text = gtk_entry_get_text(GTK_ENTRY(tf_pa_min_feedback[i]));
		const char *max_text = gtk_entry_get_text(GTK_ENTRY(tf_pa_max_feedback[i]));
		char *min_end, *max_end;
		min[i] = strtod(min_text, &min_end);
		max[i] = strtod(max_text, &max_end);
		while(g_ascii_isspace(*min_end)) min_end++;
		while(g_ascii_isspace(*max_end)) max_end++;
		if(min_end == min_text || max_end == max_text || *min_end != '\0' || *max_end != '\0' ||
			!isfinite(min[i]) || !isfinite(max[i])) return 0;
		min[i] -= 1;
		max[i] += 1;
		if(date_type == DATE_KERBAL && i == PA_DUR) {
			min[i] /= 4;
			max[i] /= 4;
		}
	}

	for(int i = 0; i < 6; i++) {
		if(min[i] > max[i]) {
			double temp = min[i];
			min[i] = max[i];
			max[i] = temp;
		}
	}
	return 1;
}

int apply_filter(void) {
	if(pa_porkchop_points == NULL) return 0;
	double min[6], max[6];
	if(!read_pa_filter_bounds(min, max) || !are_any_porkchop_points_in_filter(min, max)) return 0;

	// show only groups inside filter in gui (setting visible below)
	for(int group_idx = 0; group_idx < pa_num_groups; group_idx++) pa_groups[group_idx].has_itin_inside_filter = 0;

	struct PorkchopPoint pp;
	for(int i = 0; i < pa_num_itins; i++) {
		pa_porkchop_points[i].inside_filter = 1;
		pp = pa_porkchop_points[i].data;

		double dv_sat = pp.dv_dsm;
		if(pa_last_transfer_type == TF_CAPTURE)	dv_sat += pp.dv_arr_cap;
		if(pa_last_transfer_type == TF_CIRC)	dv_sat += pp.dv_arr_circ;

		if(	pp.dep_date 		< min[PA_DEP] 	|| pp.dep_date 			> max[PA_DEP] ||
			pp.dep_date+pp.dur 	< min[PA_ARR] 	|| pp.dep_date+pp.dur 	> max[PA_ARR] ||
			pp.dur 				< min[PA_DUR] 	|| pp.dur 				> max[PA_DUR] ||
			pp.dv_dep + dv_sat + pp.dv_flyby	< min[PA_TOTDV] || pp.dv_dep + dv_sat + pp.dv_flyby	> max[PA_TOTDV] ||
			pp.dv_dep 			< min[PA_DEPDV] || pp.dv_dep 			> max[PA_DEPDV] ||
			dv_sat + pp.dv_flyby 	< min[PA_SATDV] || dv_sat + pp.dv_flyby 	> max[PA_SATDV]) pa_porkchop_points[i].inside_filter = 0;
		else pa_porkchop_points[i].group->has_itin_inside_filter = 1;
	}
	return 1;
}

G_MODULE_EXPORT void on_pa_filter_entry_changed(GtkWidget* widget, gpointer data) {
	if(pa_porkchop_points == NULL || pa_filter_entry_update_depth > 0) return;
	if(!apply_filter()) return;
	reset_pa_porkchop_view();
	update_pa();
}

G_MODULE_EXPORT void on_reset_porkchop(GtkWidget* widget, gpointer data) {
	if(pa_porkchop_points == NULL) return;
	reset_pa_porkchop_view();
	for(int i = 0; i < pa_num_itins; i++) pa_porkchop_points[i].inside_filter = 1;
	for(int i = 0; i < pa_num_groups; i++) {pa_groups[i].show_group = 1; pa_groups[i].has_itin_inside_filter = 1;}
	reset_min_max_feedback(1);
	update_pa();
}

void update_pa_selected_itinerary(void) {
	if(pa_porkchop_points == NULL || pa_num_itins <= 0) return;
	int best_show_ind = -1;
	for(int i = 0; i < pa_num_itins; i++) {
		if(pa_porkchop_points[i].inside_filter && pa_porkchop_points[i].group != NULL &&
			pa_porkchop_points[i].group->show_group &&
			pa_porkchop_points[i].data.arrival == pa_selected_arrival) {
			best_show_ind = i;
			break;
		}
	}
	if(best_show_ind < 0)
		best_show_ind = find_first_visible_porkchop_point(pa_porkchop_points, pa_num_itins);
	if(best_show_ind < 0 || best_show_ind >= pa_num_itins) {
		if(curr_transfer_pa != NULL) free_itinerary(get_first(curr_transfer_pa));
		curr_transfer_pa = NULL;
		pa_selected_arrival = NULL;
		return;
	}
	pa_selected_arrival = pa_porkchop_points[best_show_ind].data.arrival;
	if(curr_transfer_pa != NULL) free_itinerary(get_first(curr_transfer_pa));
	curr_transfer_pa = create_itin_copy_from_arrival(pa_porkchop_points[best_show_ind].data.arrival);
	if(curr_transfer_pa == NULL) return;
	struct ItinStep *last_step = get_last(curr_transfer_pa);
	if(last_step == NULL) return;
	current_date_pa = last_step->date;
	pa_update_body_show_status();
	camera_zoom_to_fit_itinerary(pa_itin_preview_camera, curr_transfer_pa);
}

void update_best_itin() {
	update_pa_selected_itinerary();
}

void analyze_departure_itins() {
	gint64 collection_start = g_get_monotonic_time();
	int num_itins = 0, tot_num_itins = 0;
	struct ItinStep **arrivals = NULL;
	if(!collect_itineraries_from_departures(pa_departures, pa_num_deps, &arrivals, &num_itins, &tot_num_itins)) {
		fprintf(stderr, "Could not collect itineraries for the Porkchop Analyzer\n");
		return;
	}
	gint64 collection_end = g_get_monotonic_time();

	printf("\n%d itineraries found!\nNumber of Nodes: %d\n", num_itins, tot_num_itins);

	pa_porkchop_points = num_itins > 0 ?
			malloc((size_t)num_itins * sizeof(*pa_porkchop_points)) : NULL;
	if(num_itins > 0 && pa_porkchop_points == NULL) {
		fprintf(stderr, "Could not allocate Porkchop Analyzer data for %d itineraries\n", num_itins);
		free(arrivals);
		return;
	}
	set_porkchop_exact_flybys(1);
	for(int i = 0; i < num_itins; i++) {
		pa_porkchop_points[i].data = create_porkchop_point(arrivals[i],
				get_first(arrivals[0])->body->atmo_alt + pa_dep_periapsis,
				arrivals[0]->body->atmo_alt + pa_arr_periapsis);
		pa_porkchop_points[i].inside_filter = 1;
		pa_porkchop_points[i].group = NULL;
	}
	set_porkchop_exact_flybys(0);
	free(arrivals);
	gint64 metrics_end = g_get_monotonic_time();

	pa_num_itins = num_itins;
	initialize_itinerary_groups();
	if(pa_num_itins > 1) sort_porkchop(pa_porkchop_points, pa_num_itins, pa_last_transfer_type);
	update_best_itin();
	reset_min_max_feedback(1);
	gint64 analysis_end = g_get_monotonic_time();
	printf("Porkchop analysis timing: tree collection %.3f s, route metrics %.3f s, grouping/selection %.3f s\n",
			(collection_end - collection_start) / 1e6,
			(metrics_end - collection_end) / 1e6,
			(analysis_end - metrics_end) / 1e6);
}
