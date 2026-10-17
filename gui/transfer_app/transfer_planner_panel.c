#include "transfer_planner_internal.h"
#include "gui/settings.h"
#include "gui/info_win_manager.h"
#include "gui/gui_manager.h"
#include "orbit_calculator/flyby_policy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void update_date_label() {
	char time_string[20];
	char date_string[20];
	char clock_string[20];
	date_to_string(convert_JD_date(current_date_tp, get_settings_datetime_type()), date_string, 0);
	clocktime_to_string(convert_JD_date(current_date_tp, get_settings_datetime_type()), clock_string, 0);
	sprintf(time_string, "%s %s", date_string, clock_string);
	gtk_label_set_text(GTK_LABEL(lb_tp_date), time_string);
}

void update_transfer_panel() {
	int previous_restriction = set_flyby_unrestricted_periapsis(1);
	if(curr_transfer_tp == NULL) {
		gtk_button_set_label(GTK_BUTTON(tb_tp_tfdate), get_settings_datetime_type() == DATE_ISO ? "0000-00-00" : "0000-000");
		update_body_dropdown(GTK_COMBO_BOX(cb_tp_tfbody), NULL);
		fix_tfbody = TRUE;
		gtk_label_set_text(GTK_LABEL(lb_tp_dv_warning), "");
	} else {
		struct Datetime date = convert_JD_date(curr_transfer_tp->date, get_settings_datetime_type());
		char date_string[10];
		date_to_string(date, date_string, 0);
		gtk_button_set_label(GTK_BUTTON(tb_tp_tfdate), date_string);
		if(gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_tfbody)) < 0) {
			update_body_dropdown(GTK_COMBO_BOX(cb_tp_tfbody), tp_system);
			append_combobox_entry(GTK_COMBO_BOX(cb_tp_tfbody), "Deep-Space Maneuver");
		}
		fix_tfbody = TRUE;
		int body_id = curr_transfer_tp->body != NULL ? get_body_system_id(curr_transfer_tp->body, tp_system) : tp_system->num_bodies;
		gtk_combo_box_set_active(GTK_COMBO_BOX(cb_tp_tfbody), body_id);
		fix_tfbody = FALSE;
		char s_dv[64];
		sprintf(s_dv, "%6.0f m/s", calc_step_dv(curr_transfer_tp));
		gtk_label_set_label(GTK_LABEL(lb_tp_transfer_dv), s_dv);
		sprintf(s_dv, "%6.0f m/s", calc_total_dv());
		gtk_label_set_label(GTK_LABEL(lb_tp_total_dv), s_dv);
		// Route warnings go on their own red, bold lines under the total.
		char s_warning[96];
		int below_safe = count_flybys_below_min_safe(curr_transfer_tp) > 0;
		g_snprintf(s_warning, sizeof(s_warning), "%s%s%s", tp_route_unconverged ? "Approximate: the flyby solution did not settle" : "",
			tp_route_unconverged && below_safe ? "\n" : "", below_safe ? "Flyby hits planet" : "");
		gtk_label_set_text(GTK_LABEL(lb_tp_dv_warning), s_warning);
		if(curr_transfer_tp->prev != NULL || curr_transfer_tp->num_next_nodes > 0) {
			char s_tfprop_labels[ITIN_STEP_TEXT_SIZE], s_tfprop_values[ITIN_STEP_TEXT_SIZE];
			itinerary_step_parameters_to_string(s_tfprop_labels, s_tfprop_values, get_settings_datetime_type(), tp_dep_periapsis, tp_arr_periapsis, curr_transfer_tp);
			gtk_label_set_label(GTK_LABEL(lb_tp_param_labels), s_tfprop_labels);
			gtk_label_set_label(GTK_LABEL(lb_tp_param_values), s_tfprop_values);
		} else {
			gtk_label_set_label(GTK_LABEL(lb_tp_param_labels), "");
			gtk_label_set_label(GTK_LABEL(lb_tp_param_values), "");
		}
	}
	set_flyby_unrestricted_periapsis(previous_restriction);
}

void tp_change_date_type(enum DateType old_date_type, enum DateType new_date_type) {
	change_label_date_type(lb_tp_date, old_date_type, new_date_type);
	if(curr_transfer_tp != NULL) change_button_date_type(tb_tp_tfdate, old_date_type, new_date_type);
	current_date_tp = convert_date_JD(change_date_type(convert_JD_date(current_date_tp, old_date_type), new_date_type));
	
	if(strcmp(gtk_widget_get_name(GTK_WIDGET(bt_tp_10y6hp)), "+10Y") == 0) {
		gtk_button_set_label(GTK_BUTTON(bt_tp_1m30d10minp), new_date_type == DATE_ISO ? "+1M" : "+30D");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1m30d10minp), new_date_type == DATE_ISO ? "+1M" : "+30D");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1m30d10minm), new_date_type == DATE_ISO ? "-1M" : "-30D");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1m30d10minm), new_date_type == DATE_ISO ? "-1M" : "-30D");
	}
	update();
}

// A maneuver node slides along its incoming arc when its date changes.
static void tp_set_current_step_date(double date) {
	struct ItinStep *first = get_first(curr_transfer_tp);
	int count = 0;
	for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) count++;
	double *dates = malloc(count * sizeof(*dates));
	Vector3 *positions = malloc(count * sizeof(*positions));
	if(dates == NULL || positions == NULL) { free(dates); free(positions); return; }
	int index = 0;
	for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL, index++) {
		dates[index] = node->date;
		positions[index] = node->r;
	}

	set_itinerary_step_date(curr_transfer_tp, tp_system, date);
	if(!tp_resolve_route()) {
		index = 0;
		for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL, index++) {
			node->date = dates[index];
			node->r = positions[index];
		}
		update_itin_body_osvs(first, tp_system);
		show_msg_window("This route cannot be re-solved at that date, so the edit was ignored.");
	}
	free(dates);
	free(positions);
	update();
}

G_MODULE_EXPORT void on_change_date(GtkWidget* widget, gpointer data) {
	const char *name = gtk_widget_get_name(widget);
	// Date
	if		(strcmp(name, "+10Y") == 0) current_date_tp = jd_change_date(current_date_tp, 10, 0, 0, get_settings_datetime_type());
	else if	(strcmp(name,  "+1Y") == 0) current_date_tp = jd_change_date(current_date_tp, 1, 0, 0, get_settings_datetime_type());
	else if	(strcmp(name,  "+1M") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 1, 0, get_settings_datetime_type());
	else if	(strcmp(name,  "+30D") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0, 30, get_settings_datetime_type());
	else if	(strcmp(name,  "+1D") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0, 1, get_settings_datetime_type());
	else if	(strcmp(name, "-10Y") == 0) current_date_tp = jd_change_date(current_date_tp, -10, 0, 0, get_settings_datetime_type());
	else if	(strcmp(name,  "-1Y") == 0) current_date_tp = jd_change_date(current_date_tp, -1, 0, 0, get_settings_datetime_type());
	else if	(strcmp(name,  "-1M") == 0) current_date_tp = jd_change_date(current_date_tp, 0, -1, 0, get_settings_datetime_type());
	else if	(strcmp(name, "-30D") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0, -30, get_settings_datetime_type());
	else if	(strcmp(name,  "-1D") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0, -1, get_settings_datetime_type());
	// Clocktime
	else if	(strcmp(name,  "+6h") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0,  6.0/24, get_settings_datetime_type());
	else if	(strcmp(name,  "-6h") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0, -6.0/24, get_settings_datetime_type());
	else if	(strcmp(name,  "+1h") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0,  1.0/24, get_settings_datetime_type());
	else if	(strcmp(name,  "-1h") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0, -1.0/24, get_settings_datetime_type());
	else if	(strcmp(name, "+10m") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0,  10.0/(24*60), get_settings_datetime_type());
	else if	(strcmp(name, "-10m") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0, -10.0/(24*60), get_settings_datetime_type());
	else if	(strcmp(name,  "+1m") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0,  1.0/(24*60), get_settings_datetime_type());
	else if	(strcmp(name,  "-1m") == 0) current_date_tp = jd_change_date(current_date_tp, 0, 0, -1.0/(24*60), get_settings_datetime_type());

	if(curr_transfer_tp != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate))) {
		if(curr_transfer_tp->prev != NULL && current_date_tp <= curr_transfer_tp->prev->date) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate), 0);
			return;
		}
		tp_set_current_step_date(current_date_tp);
		return;
	}

	// Only the clock moved, so the route and its delta-v stay as they are.
	update();
}

G_MODULE_EXPORT void on_tp_reset_clocktime(GtkWidget* widget, gpointer data) {
	Datetime datetime = convert_JD_date(current_date_tp, get_settings_datetime_type());
	current_date_tp = convert_date_JD((Datetime){datetime.y, datetime.m, datetime.d});
	if(curr_transfer_tp != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate))) {
		if(curr_transfer_tp->prev != NULL && current_date_tp <= curr_transfer_tp->prev->date) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate), 0);
			return;
		}
		tp_set_current_step_date(current_date_tp);
		return;
	}
	
	update();
}

G_MODULE_EXPORT void on_tp_switch_clocktime_date(GtkWidget* widget, gpointer data) {
	if(strcmp(gtk_widget_get_name(GTK_WIDGET(bt_tp_10y6hp)), "+10Y") == 0) {
		gtk_button_set_label(GTK_BUTTON(bt_tp_10y6hp), "+6h");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_10y6hp), "+6h");
		gtk_button_set_label(GTK_BUTTON(bt_tp_10y6hm), "-6h");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_10y6hm), "-6h");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1y1hp), "+1h");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1y1hp), "+1h");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1y1hm), "-1h");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1y1hm), "-1h");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1m30d10minp), "+10m");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1m30d10minp), "+10m");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1m30d10minm), "-10m");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1m30d10minm), "-10m");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1d1minp), "+1m");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1d1minp), "+1m");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1d1minm), "-1m");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1d1minm), "-1m");
	} else {
		gtk_button_set_label(GTK_BUTTON(bt_tp_10y6hp), "+10Y");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_10y6hp), "+10Y");
		gtk_button_set_label(GTK_BUTTON(bt_tp_10y6hm), "-10Y");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_10y6hm), "-10Y");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1y1hp), "+1Y");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1y1hp), "+1Y");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1y1hm), "-1Y");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1y1hm), "-1Y");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1m30d10minp), get_settings_datetime_type() == DATE_ISO ? "+1M" : "+30D");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1m30d10minp), get_settings_datetime_type() == DATE_ISO ? "+1M" : "+30D");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1m30d10minm), get_settings_datetime_type() == DATE_ISO ? "-1M" : "-30D");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1m30d10minm), get_settings_datetime_type() == DATE_ISO ? "-1M" : "-30D");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1d1minp), "+1D");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1d1minp), "+1D");
		gtk_button_set_label(GTK_BUTTON(bt_tp_1d1minm), "-1D");
		gtk_widget_set_name(GTK_WIDGET(bt_tp_1d1minm), "-1D");
		
	}
}

G_MODULE_EXPORT void on_prev_transfer(GtkWidget* widget, gpointer data) {
	if(curr_transfer_tp == NULL) return;
	if(curr_transfer_tp->prev != NULL) curr_transfer_tp = curr_transfer_tp->prev;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate), 0);
	current_date_tp = curr_transfer_tp->date;
	update();
}

G_MODULE_EXPORT void on_next_transfer(GtkWidget* widget, gpointer data) {
	if(curr_transfer_tp == NULL) return;
	if(curr_transfer_tp->next != NULL) curr_transfer_tp = curr_transfer_tp->next[0];
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate), 0);
	current_date_tp = curr_transfer_tp->date;
	update();
}

G_MODULE_EXPORT void on_transfer_body_change(GtkWidget* widget, gpointer data) {
	if(curr_transfer_tp == NULL || fix_tfbody) return;
	int body_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_tp_tfbody));
	if(body_index < 0 || body_index >= tp_system->num_bodies) return;
	curr_transfer_tp->body = tp_system->bodies[body_index];
	update_itinerary();
}

G_MODULE_EXPORT void on_last_transfer_type_changed_tp(GtkWidget* widget, gpointer data) {
	if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) return;
	const char *name = gtk_widget_get_name(widget);
	if		(strcmp(name, "fb") == 0) tp_last_transfer_type = TF_FLYBY;
	else if	(strcmp(name, "capture") == 0) tp_last_transfer_type = TF_CAPTURE;
	else if	(strcmp(name, "circ") == 0) tp_last_transfer_type = TF_CIRC;
	update_transfer_panel();
}

G_MODULE_EXPORT void on_toggle_transfer_date_lock(GtkWidget* widget, gpointer data) {
	if(curr_transfer_tp == NULL) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate), 0);
		return;
	}
	if(curr_transfer_tp != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate)))
		current_date_tp = curr_transfer_tp->date;
	// Locking or releasing only changes what the date buttons move; the route stays as it is.
	update();
}

G_MODULE_EXPORT void on_goto_transfer_date(GtkWidget* widget, gpointer data) {
	if(curr_transfer_tp == NULL) return;
	current_date_tp = curr_transfer_tp->date;
	update();
}

int find_closest_transfer(struct ItinStep *step) {
	// A maneuver node has no body to search for, and a maneuver predecessor has no state to search from.
	if(step == NULL || step->body == NULL || step->prev == NULL || step->prev->prev == NULL || step->prev->body == NULL) return 0;
	struct ItinStep *prev = step->prev;
	struct ItinStep *temp = (struct ItinStep*) malloc(sizeof(struct ItinStep));
	copy_step_body_vectors_and_date(prev, temp);
	temp->next = NULL;
	temp->prev = NULL;
	temp->num_next_nodes = 0;
	find_viable_flybys(temp, tp_system, step->body, 86400, 86400*365.25*50);

	if(temp->next != NULL) {
		// Resonant returns come grouped by leg variant, so take the one closest to the current date.
		struct ItinStep *new_step = temp->next[0];
		for(int i = 1; i < temp->num_next_nodes && step->body == prev->body; i++)
			if(fabs(temp->next[i]->date - step->date) < fabs(new_step->date - step->date)) new_step = temp->next[i];
		copy_step_body_vectors_and_date(new_step, step);
		for(int i = 0; i < temp->num_next_nodes; i++) free(temp->next[i]);
		free(temp->next);
		current_date_tp = step->date;
		free(temp);
		sort_transfer_dates(step);
		return 1;
	} else {
		free(temp);
		return 0;
	}
}

G_MODULE_EXPORT void on_find_closest_transfer(GtkWidget* widget, gpointer data) {
	if(tp_system == NULL) return;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb_tp_tfdate), 0);
	if(curr_transfer_tp != NULL && curr_transfer_tp->body == NULL) {
		// On a maneuver node the button re-optimizes the route's maneuvers.
		struct ItinStep *first = get_first(curr_transfer_tp);
		if(optimize_itinerary_maneuvers(tp_system, first, tp_dep_periapsis, tp_arr_periapsis, tp_last_transfer_type)) {
			curr_transfer_tp = first;
			for(struct ItinStep *step = first; step != NULL; step = step->next != NULL ? step->next[0] : NULL)
				if(step->body == NULL) { curr_transfer_tp = step; break; }
			current_date_tp = curr_transfer_tp->date;
			update_itinerary();
		}
		return;
	}
	int success = find_closest_transfer(curr_transfer_tp);
	if(success) update_itinerary();
}
