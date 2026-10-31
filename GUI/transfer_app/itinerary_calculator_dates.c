#include "itinerary_calculator_internal.h"
#include "date_picker.h"
#include "gui/css_loader.h"
#include "tools/itinerary_date_limits.h"
#include "tools/itinerary_date_input.h"
#include "tools/itinerary_reference_defaults.h"
#include <math.h>
static void on_ic_latest_date_adjust(GtkButton *button, gpointer user_data);
void on_ic_earliest_dep_changed(GtkEditable *editable, gpointer user_data);
void on_ic_latest_arrival_changed(GtkEditable *editable, gpointer user_data);
void on_ic_max_duration_changed(GtkEditable *editable, gpointer user_data);
gboolean update_ic_reference_defaults(void);
void on_ic_reference_input_changed(GtkWidget *widget, gpointer user_data);

static double adjust_date_by_interval(double jd, const char *interval, int direction, enum DateType date_type) {
	if(g_strcmp0(interval, "week") == 0)
		return jd_change_date(jd, 0, 0, direction * 7, date_type);
	if(g_strcmp0(interval, "month") == 0)
		return date_type == DATE_ISO ? jd_change_date(jd, 0, direction, 0, date_type) : jd_change_date(jd, 0, 0, direction * 30, date_type);
	return jd_change_date(jd, direction, 0, 0, date_type);
}

static void on_date_entry_insert_text(
	GtkEditable *editable,
	const gchar *new_text,
	gint new_text_length,
	gint *position,
	gpointer user_data) {
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
	gboolean valid = itinerary_date_entry_is_valid_prefix(candidate->str, get_settings_datetime_type());
	g_string_free(candidate, TRUE);
	if(!valid) g_signal_stop_emission_by_name(editable, "insert-text");
}

static void on_date_entry_changed(GtkEditable *editable, gpointer user_data) {
	(void)user_data;
	GtkWidget *entry = GTK_WIDGET(editable);
	const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
	enum DateType date_type = get_settings_datetime_type();
	GtkStyleContext *style = gtk_widget_get_style_context(entry);
	if(itinerary_date_entry_is_complete(text, date_type))
		g_object_set_data_full(G_OBJECT(entry), "last-valid-date", g_strdup(text), g_free);
	if(itinerary_date_entry_is_valid_prefix(text, date_type))
		gtk_style_context_remove_class(style, "error");
	else
		gtk_style_context_add_class(style, "error");
}

static gboolean on_date_entry_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data) {
	(void)event;
	(void)user_data;
	const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
	char normalized_text[32];
	if(itinerary_date_entry_normalize(text, get_settings_datetime_type(),
		normalized_text, sizeof(normalized_text))) {
		if(g_strcmp0(text, normalized_text) != 0)
			gtk_entry_set_text(GTK_ENTRY(widget), normalized_text);
	} else {
		const char *last_valid = g_object_get_data(G_OBJECT(widget), "last-valid-date");
		gtk_entry_set_text(GTK_ENTRY(widget), last_valid != NULL ? last_valid : "");
	}
	return FALSE;
}

static void on_ic_date_entry_activate(GtkEntry *entry, gpointer user_data) {
	(void)user_data;
	on_date_entry_focus_out(GTK_WIDGET(entry), NULL, NULL);

	GtkWidget *next_entry = entry == GTK_ENTRY(tf_ic_mindepdate) ? GTK_WIDGET(tf_ic_maxdepdate) :
		entry == GTK_ENTRY(tf_ic_maxdepdate) ? GTK_WIDGET(tf_ic_maxarrdate) :
		entry == GTK_ENTRY(tf_ic_maxarrdate) ? GTK_WIDGET(tf_ic_maxdur) : NULL;
	if(next_entry != NULL) {
		gtk_widget_grab_focus(next_entry);
		gtk_editable_select_region(GTK_EDITABLE(next_entry), 0, -1);
	}
}

void connect_itinerary_date_entry_validation(GtkEntry *entry) {
	g_signal_connect(entry, "insert-text", G_CALLBACK(on_date_entry_insert_text), NULL);
	g_signal_connect(entry, "changed", G_CALLBACK(on_date_entry_changed), NULL);
	g_signal_connect(entry, "focus-out-event", G_CALLBACK(on_date_entry_focus_out), NULL);
	gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_SECONDARY, "x-office-calendar");
	set_stable_tooltip_for_entry_icon(entry, GTK_ENTRY_ICON_SECONDARY, "Select date from calendar");
	g_signal_connect(entry, "icon-release", G_CALLBACK(gui_date_picker_on_entry_icon_release), NULL);
	on_date_entry_changed(GTK_EDITABLE(entry), NULL);
}

void connect_ic_date_entry_validation(GtkEntry *entry) {
	connect_itinerary_date_entry_validation(entry);
	g_signal_connect(entry, "changed", G_CALLBACK(on_ic_date_range_changed), NULL);
	g_signal_connect(entry, "activate", G_CALLBACK(on_ic_date_entry_activate), NULL);
}

static void set_entry_date_from_jd(GtkEntry *entry, double jd, enum DateType date_type) {
	Datetime date = convert_JD_date(jd, date_type);
	char date_text[32];
	date_to_string(date, date_text, 0);
	gtk_entry_set_text(entry, date_text);
}

void connect_date_adjust_buttons(GtkBuilder *builder, const char *prefix, GtkEntry *target) {
	static const char *suffixes[] = {
		"minus_year", "minus_month", "minus_week", "plus_week", "plus_month", "plus_year"
	};
	static const char *intervals[] = {"year", "month", "week", "week", "month", "year"};
	static const char *tooltips[] = {
		"Subtract one year", "Subtract one month", "Subtract one week",
		"Add one week", "Add one month", "Add one year"
	};
	for(guint i = 0; i < G_N_ELEMENTS(suffixes); i++) {
		char object_id[64];
		g_snprintf(object_id, sizeof(object_id), "btn_%s_%s", prefix, suffixes[i]);
		GtkWidget *button = GTK_WIDGET(gtk_builder_get_object(builder, object_id));
		int direction = i < 3 ? -1 : 1;
		g_object_set_data(G_OBJECT(button), "interval", (gpointer)intervals[i]);
		g_object_set_data(G_OBJECT(button), "direction", GINT_TO_POINTER(direction));
		set_stable_tooltip(button, tooltips[i]);
		gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
		gtk_widget_set_size_request(button, 18, 16);
		set_css_class_for_widget(button, "date-adjust-button");
		g_signal_connect(button, "clicked", G_CALLBACK(on_ic_latest_date_adjust), target);
		if(g_strcmp0(prefix, "ic_mindep") == 0 && g_strcmp0(intervals[i], "month") == 0) {
			if(direction < 0) ic_earliest_dep_minus_month_button = button;
			else ic_earliest_dep_plus_month_button = button;
		} else if(g_strcmp0(prefix, "ic_maxdep") == 0 && g_strcmp0(intervals[i], "month") == 0) {
			if(direction < 0) ic_latest_dep_minus_month_button = button;
			else ic_latest_dep_plus_month_button = button;
		} else if(g_strcmp0(intervals[i], "month") == 0) {
			if(direction < 0) ic_latest_arr_minus_month_button = button;
			else ic_latest_arr_plus_month_button = button;
		}
	}
}

void update_month_adjust_button_labels(enum DateType date_type) {
	const char *minus_label = date_type == DATE_ISO ? "-m" : "-30d";
	const char *plus_label = date_type == DATE_ISO ? "+m" : "+30d";
	GtkWidget *minus_buttons[] = {ic_earliest_dep_minus_month_button, ic_latest_dep_minus_month_button, ic_latest_arr_minus_month_button};
	GtkWidget *plus_buttons[] = {ic_earliest_dep_plus_month_button, ic_latest_dep_plus_month_button, ic_latest_arr_plus_month_button};
	for(guint i = 0; i < G_N_ELEMENTS(minus_buttons); i++) {
		if(minus_buttons[i] != NULL) {
			gtk_button_set_label(GTK_BUTTON(minus_buttons[i]), minus_label);
			set_stable_tooltip(minus_buttons[i], date_type == DATE_ISO ? "Subtract one month" : "Subtract 30 days");
		}
		if(plus_buttons[i] != NULL) {
			gtk_button_set_label(GTK_BUTTON(plus_buttons[i]), plus_label);
			set_stable_tooltip(plus_buttons[i], date_type == DATE_ISO ? "Add one month" : "Add 30 days");
		}
	}
}

void ic_close_date_picker(void) {
	gui_date_picker_close();
}

static void on_ic_latest_date_adjust(GtkButton *button, gpointer user_data) {
	enum DateType date_type = get_settings_datetime_type();
	GtkEntry *target = GTK_ENTRY(user_data);
	const char *date_text = gtk_entry_get_text(target);
	if(!is_string_valid_date_format(date_text, date_type))
		date_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	if(!is_string_valid_date_format(date_text, date_type)) return;

	Datetime date = date_from_string((char *)date_text, date_type);
	double adjusted_jd = convert_date_JD(date);
	const char *interval = g_object_get_data(G_OBJECT(button), "interval");
	int direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
	adjusted_jd = adjust_date_by_interval(adjusted_jd, interval, direction, date_type);
	set_entry_date_from_jd(target, adjusted_jd, date_type);
}

void on_ic_earliest_dep_changed(GtkEditable *editable, gpointer user_data) {
	if(ic_changing_date_type) return;
	if(!ic_setting_ksp_start_date) ic_clear_ksp_start();
	const char *date_text = gtk_entry_get_text(GTK_ENTRY(editable));
	if(is_string_valid_date_format(date_text, get_settings_datetime_type())) {
		ic_syncing_time_limits = TRUE;
		gtk_entry_set_text(GTK_ENTRY(tf_ic_maxdepdate), date_text);
		ic_syncing_time_limits = FALSE;
	}
}

void on_ic_latest_arrival_changed(GtkEditable *editable, gpointer user_data) {
	if(ic_syncing_time_limits) return;
	const char *departure_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdepdate));
	const char *arrival_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_maxarrdate));
	enum DateType date_type = get_settings_datetime_type();
	if(!is_string_valid_date_format(departure_text, date_type) ||
		!is_string_valid_date_format(arrival_text, date_type)) return;
	double latest_departure_jd = convert_date_JD(date_from_string((char *)departure_text, date_type));
	double arrival_jd = convert_date_JD(date_from_string((char *)arrival_text, date_type));
	double duration_days = MAX(arrival_jd - latest_departure_jd, 0.0);
	double user_duration = duration_days * (date_type == DATE_KERBAL ? 4.0 : 1.0);
	char duration_text[32];
	g_snprintf(duration_text, sizeof(duration_text), "%.0f", user_duration);
	ic_syncing_time_limits = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate), TRUE);
	gtk_entry_set_text(GTK_ENTRY(tf_ic_maxdur), duration_text);
	ic_syncing_time_limits = FALSE;
}

gboolean validate_ic_date_range_widgets(void) {
	enum DateType date_type = get_settings_datetime_type();
	const char *earliest_departure = gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	const char *latest_departure = gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdepdate));
	const char *latest_arrival = gtk_entry_get_text(GTK_ENTRY(tf_ic_maxarrdate));
	ItineraryDateRangeStatus status = validate_itinerary_date_range(
		earliest_departure, latest_departure, latest_arrival, date_type,
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate)));
	GString *warning = g_string_new(NULL);
	if(status & ITINERARY_DATE_RANGE_LATEST_DEPARTURE_TOO_EARLY)
		g_string_append(warning, "Latest departure date must be on or after earliest departure date.");
	if(status & ITINERARY_DATE_RANGE_LATEST_ARRIVAL_BEFORE_LATEST_DEPARTURE) {
		if(warning->len > 0) g_string_append_c(warning, '\n');
		g_string_append(warning, "Latest arrival date must be on or after latest departure date.");
	}

	if(warning->len > 0) {
		gtk_label_set_text(GTK_LABEL(lb_ic_date_warning), warning->str);
		gtk_style_context_add_class(gtk_widget_get_style_context(lb_ic_date_warning), "error");
		gtk_widget_show(lb_ic_date_warning);
	} else {
		gtk_label_set_text(GTK_LABEL(lb_ic_date_warning), "");
		gtk_style_context_remove_class(gtk_widget_get_style_context(lb_ic_date_warning), "error");
		gtk_widget_hide(lb_ic_date_warning);
	}
	g_string_free(warning, TRUE);

	gboolean valid = status == ITINERARY_DATE_RANGE_VALID;
	gtk_widget_set_sensitive(bt_ic_calc, valid);
	return valid;
}

void on_ic_date_range_changed(GtkEditable *editable, gpointer user_data) {
	(void)editable;
	(void)user_data;
	validate_ic_date_range_widgets();
}

void on_ic_max_duration_changed(GtkEditable *editable, gpointer user_data) {
	if(ic_syncing_time_limits) return;
	const char *duration_text = gtk_entry_get_text(GTK_ENTRY(editable));
	char *end = NULL;
	double user_duration = g_ascii_strtod(duration_text, &end);
	while(end != NULL && g_ascii_isspace(*end)) end++;
	if(end == duration_text || end == NULL || *end != '\0' || !isfinite(user_duration) || user_duration < 0)
		return;
	const char *min_dep_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	const char *max_dep_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdepdate));
	enum DateType date_type = get_settings_datetime_type();
	if(!is_string_valid_date_format(min_dep_text, date_type) ||
		!is_string_valid_date_format(max_dep_text, date_type)) return;
	double max_dep_jd = convert_date_JD(date_from_string((char *)max_dep_text, date_type));
	ItineraryDateLimits limits = calculate_itinerary_date_limits(
		FALSE, 0, max_dep_jd, 0, user_duration, date_type);
	ic_syncing_time_limits = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb_ic_maxdur), TRUE);
	set_entry_date_from_jd(GTK_ENTRY(tf_ic_maxarrdate), limits.max_arrival_jd, date_type);
	ic_syncing_time_limits = FALSE;
}

gboolean update_ic_reference_defaults(void) {
	if(ic_system == NULL || tf_ic_mindepdate == NULL || tf_ic_maxarrdate == NULL ||
		tf_ic_maxdur == NULL || tf_ic_depdv == NULL || tf_ic_satdv == NULL) return FALSE;
	int departure_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody));
	int arrival_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody));
	if(departure_index < 0 || departure_index >= ic_system->num_bodies ||
		arrival_index < 0 || arrival_index >= ic_system->num_bodies) return FALSE;
	Body *departure_body = ic_system->bodies[departure_index];
	Body *arrival_body = ic_system->bodies[arrival_index];

	ItineraryHohmannReference reference;
	if(!calculate_itinerary_hohmann_reference(departure_body, arrival_body, ic_system->cb,
		ic_dep_periapsis, ic_arr_periapsis, &reference)) return FALSE;
	const char *earliest_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	enum DateType date_type = get_settings_datetime_type();
	if(!is_string_valid_date_format(earliest_text, date_type)) return FALSE;
	double earliest_jd = convert_date_JD(date_from_string((char *)earliest_text, date_type));
	ItineraryHohmannDefaults defaults;
	if(!calculate_itinerary_hohmann_defaults(&reference, TF_CAPTURE, date_type, earliest_jd, &defaults))
		return FALSE;

	char value_text[32];
	Datetime max_arrival = convert_JD_date(defaults.max_arrival_jd, date_type);
	date_to_string(max_arrival, value_text, 0);
	ic_syncing_time_limits = TRUE;
	gtk_entry_set_text(GTK_ENTRY(tf_ic_maxarrdate), value_text);
	g_snprintf(value_text, sizeof(value_text), "%.0f", defaults.max_duration_user_days);
	gtk_entry_set_text(GTK_ENTRY(tf_ic_maxdur), value_text);
	ic_syncing_time_limits = FALSE;
	g_snprintf(value_text, sizeof(value_text), "%.0f", defaults.max_departure_dv);
	gtk_entry_set_text(GTK_ENTRY(tf_ic_depdv), value_text);
	g_snprintf(value_text, sizeof(value_text), "%.0f", defaults.max_satellite_dv);
	gtk_entry_set_text(GTK_ENTRY(tf_ic_satdv), value_text);
	return TRUE;
}

void on_ic_reference_input_changed(GtkWidget *widget, gpointer user_data) {
	if(widget == GTK_WIDGET(cb_ic_depbody) || widget == GTK_WIDGET(cb_ic_arrbody)) {
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
		ic_update_fixed_sequence_grid();
	}
	update_ic_reference_defaults();
}

G_MODULE_EXPORT void on_ic_date_icon_release(GtkEntry *entry, GtkEntryIconPosition icon_pos, gpointer data) {
	gui_date_picker_on_entry_icon_release(entry, icon_pos, data);
}

G_MODULE_EXPORT void on_ic_time_limit_toggled(GtkToggleButton *button, gpointer data) {
	if(!gtk_toggle_button_get_active(button)) return;
	gboolean use_latest_arrival_date =
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate));
	gtk_widget_set_sensitive(box_ic_maxarrdate, use_latest_arrival_date);
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_maxdur), !use_latest_arrival_date);
	validate_ic_date_range_widgets();
}
