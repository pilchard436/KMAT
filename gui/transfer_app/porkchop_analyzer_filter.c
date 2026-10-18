#include "porkchop_analyzer_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

void apply_filter() {
	if(pa_porkchop_points == NULL) return;
	double min[6], max[6];
	char *string;
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_pa_min_feedback[PA_DEP]));
	min[PA_DEP] = convert_date_JD(date_from_string(string, get_settings_datetime_type()))-1;	// rounding imprecision in filter entry field
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_pa_max_feedback[PA_DEP]));
	max[PA_DEP] = convert_date_JD(date_from_string(string, get_settings_datetime_type()))+1;	// rounding imprecision in filter entry field
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_pa_min_feedback[PA_ARR]));
	min[PA_ARR] = convert_date_JD(date_from_string(string, get_settings_datetime_type()))-1;	// rounding imprecision in filter entry field
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_pa_max_feedback[PA_ARR]));
	max[PA_ARR] = convert_date_JD(date_from_string(string, get_settings_datetime_type()))+1;	// rounding imprecision in filter entry field
	for(int i = 2; i < 6; i++) {
		string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_pa_min_feedback[i]));
		min[i] = strtod(string, NULL)-1;	// rounding imprecision in filter entry field
		string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_pa_max_feedback[i]));
		max[i] = strtod(string, NULL)+1;	// rounding imprecision in filter entry field
		if(get_settings_datetime_type() == DATE_KERBAL && i == PA_DUR) {min[i] /= 4; max[i] /= 4;}
	}

	for(int i = 0; i < 6; i++) {
		if(min[i] > max[i]) {
			double temp = min[i];
			min[i] = max[i];
			max[i] = temp;
		}
	}

	if(!are_any_porkchop_points_in_filter(min, max)) return;

	// show only groups inside filter in gui (setting visible below)
	for(int group_idx = 0; group_idx < pa_num_groups; group_idx++) pa_groups[group_idx].has_itin_inside_filter = 0;

	int init_num_itins = 0, rem_num_itins = 0;

	struct PorkchopPoint pp;
	for(int i = 0; i < pa_num_itins; i++) {
		if(pa_porkchop_points[i].inside_filter && pa_porkchop_points[i].group->show_group) init_num_itins++;

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

		if(pa_porkchop_points[i].inside_filter && pa_porkchop_points[i].group->show_group) rem_num_itins++;
	}
}

G_MODULE_EXPORT void on_apply_filter(GtkWidget* widget, gpointer data) {
	if(pa_porkchop_points == NULL) return;
	apply_filter();
	reset_min_max_feedback(0);
	apply_filter();
	update_pa();
}

G_MODULE_EXPORT void on_reset_porkchop(GtkWidget* widget, gpointer data) {
	if(pa_porkchop_points == NULL) return;
	for(int i = 0; i < pa_num_itins; i++) pa_porkchop_points[i].inside_filter = 1;
	for(int i = 0; i < pa_num_groups; i++) {pa_groups[i].show_group = 1; pa_groups[i].has_itin_inside_filter = 1;}
	update_pa();
}

void update_best_itin() {
	if(pa_porkchop_points == NULL || pa_num_itins <= 0) return;
	int best_show_ind = find_first_visible_porkchop_point(pa_porkchop_points, pa_num_itins);
	if(best_show_ind < 0 || best_show_ind >= pa_num_itins) return;
	if(curr_transfer_pa != NULL) free_itinerary(get_first(curr_transfer_pa));
	curr_transfer_pa = create_itin_copy_from_arrival(pa_porkchop_points[best_show_ind].data.arrival);
	current_date_pa = get_last(curr_transfer_pa)->date;
	pa_update_body_show_status();
	camera_zoom_to_fit_itinerary(pa_itin_preview_camera, curr_transfer_pa);
}

void analyze_departure_itins() {
	int num_itins = 0, tot_num_itins = 0;
	for(int i = 0; i < pa_num_deps; i++) num_itins += get_number_of_itineraries(pa_departures[i]);
	for(int i = 0; i < pa_num_deps; i++) tot_num_itins += get_total_number_of_stored_steps(pa_departures[i]);

	printf("\n%d itineraries found!\nNumber of Nodes: %d\n", num_itins, tot_num_itins);

	int index = 0;
	struct ItinStep **arrivals = (struct ItinStep**) malloc(num_itins * sizeof(struct ItinStep*));
	for(int i = 0; i < pa_num_deps; i++) store_itineraries_in_array(pa_departures[i], arrivals, &index);
	pa_porkchop_points = malloc(num_itins * sizeof(struct PorkchopAnalyzerPoint));
	set_porkchop_exact_flybys(1);
	for(int i = 0; i < num_itins; i++) {
		pa_porkchop_points[i].data = create_porkchop_point(arrivals[i], get_first(arrivals[0])->body->atmo_alt + pa_dep_periapsis, arrivals[0]->body->atmo_alt + pa_arr_periapsis);
		pa_porkchop_points[i].inside_filter = 1;
		pa_porkchop_points[i].group = NULL;
	}
	set_porkchop_exact_flybys(0);
	free(arrivals);

	pa_num_itins = num_itins;
	initialize_itinerary_groups();
	sort_porkchop(pa_porkchop_points, pa_num_itins, pa_last_transfer_type);
	update_best_itin();
	reset_min_max_feedback(1);
}
