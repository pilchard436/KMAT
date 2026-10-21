#include "itinerary_calculator_internal.h"
#include "gui/css_loader.h"
#include "tools/itinerary_date_limits.h"
#include "tools/itinerary_date_input.h"
#include "tools/itinerary_date_picker_state.h"
#include "tools/itinerary_reference_defaults.h"
#include <math.h>

typedef struct {
	GtkEntry *entry;
	GtkCalendar *calendar;
	GtkStack *stack;
	GtkWidget *month_button;
	GtkWidget *year_button;
	GtkWidget *year_range_label;
	GtkWidget *year_grid;
	GtkSpinButton *year_input;
	GtkSpinButton *day_input;
	enum DateType date_type;
	guint year_page_start;
} DatePickerDialogData;

static ItineraryDatePickerState active_date_picker_dialog;

static void on_date_picker_response(GtkDialog *dialog, gint response_id, gpointer user_data);
static void on_date_picker_dialog_destroy(GtkWidget *dialog, gpointer user_data);
static void track_date_picker_dialog(GtkWidget *dialog);
static void update_date_picker_header(DatePickerDialogData *data);
static void populate_date_picker_year_grid(DatePickerDialogData *data);
static void on_ic_latest_date_adjust(GtkButton *button, gpointer user_data);
static void show_ordinal_date_picker(GtkEntry *entry, enum DateType date_type);
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

static void on_ic_date_entry_insert_text(
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

static void on_ic_date_entry_changed(GtkEditable *editable, gpointer user_data) {
	(void)user_data;
	GtkWidget *entry = GTK_WIDGET(editable);
	const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
	enum DateType date_type = get_settings_datetime_type();
	GtkStyleContext *style = gtk_widget_get_style_context(entry);
	if(itinerary_date_entry_is_complete(text, date_type))
		g_object_set_data_full(G_OBJECT(entry), "ic-last-valid-date", g_strdup(text), g_free);
	if(itinerary_date_entry_is_valid_prefix(text, date_type))
		gtk_style_context_remove_class(style, "error");
	else
		gtk_style_context_add_class(style, "error");
}

static gboolean on_ic_date_entry_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data) {
	(void)event;
	(void)user_data;
	const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
	char normalized_text[32];
	if(itinerary_date_entry_normalize(text, get_settings_datetime_type(),
		normalized_text, sizeof(normalized_text))) {
		if(g_strcmp0(text, normalized_text) != 0)
			gtk_entry_set_text(GTK_ENTRY(widget), normalized_text);
	} else {
		const char *last_valid = g_object_get_data(G_OBJECT(widget), "ic-last-valid-date");
		gtk_entry_set_text(GTK_ENTRY(widget), last_valid != NULL ? last_valid : "");
	}
	return FALSE;
}

static void on_ic_date_entry_activate(GtkEntry *entry, gpointer user_data) {
	(void)user_data;
	on_ic_date_entry_focus_out(GTK_WIDGET(entry), NULL, NULL);

	GtkWidget *next_entry = entry == GTK_ENTRY(tf_ic_mindepdate) ? GTK_WIDGET(tf_ic_maxdepdate) :
		entry == GTK_ENTRY(tf_ic_maxdepdate) ? GTK_WIDGET(tf_ic_maxarrdate) :
		entry == GTK_ENTRY(tf_ic_maxarrdate) ? GTK_WIDGET(tf_ic_maxdur) : NULL;
	if(next_entry != NULL) {
		gtk_widget_grab_focus(next_entry);
		gtk_editable_select_region(GTK_EDITABLE(next_entry), 0, -1);
	}
}

void connect_ic_date_entry_validation(GtkEntry *entry) {
	g_signal_connect(entry, "insert-text", G_CALLBACK(on_ic_date_entry_insert_text), NULL);
	g_signal_connect(entry, "changed", G_CALLBACK(on_ic_date_entry_changed), NULL);
	g_signal_connect(entry, "changed", G_CALLBACK(on_ic_date_range_changed), NULL);
	g_signal_connect(entry, "focus-out-event", G_CALLBACK(on_ic_date_entry_focus_out), NULL);
	g_signal_connect(entry, "activate", G_CALLBACK(on_ic_date_entry_activate), NULL);
	on_ic_date_entry_changed(GTK_EDITABLE(entry), NULL);
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
		if(g_strcmp0(prefix, "ic_maxdep") == 0 && g_strcmp0(intervals[i], "month") == 0) {
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
	GtkWidget *minus_buttons[] = {ic_latest_dep_minus_month_button, ic_latest_arr_minus_month_button};
	GtkWidget *plus_buttons[] = {ic_latest_dep_plus_month_button, ic_latest_arr_plus_month_button};
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

static void free_date_picker_dialog_data(gpointer data, GClosure *closure) {
	g_free(data);
}

static void on_date_picker_dialog_destroy(GtkWidget *dialog, gpointer user_data) {
	(void)user_data;
	itinerary_date_picker_closed(&active_date_picker_dialog, dialog);
}

static void destroy_date_picker_dialog(void *dialog) {
	gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void track_date_picker_dialog(GtkWidget *dialog) {
	itinerary_date_picker_track(&active_date_picker_dialog, dialog, destroy_date_picker_dialog);
	g_signal_connect(dialog, "destroy", G_CALLBACK(on_date_picker_dialog_destroy), NULL);
}

void ic_close_date_picker(void) {
	itinerary_date_picker_close(&active_date_picker_dialog);
}

static void on_date_picker_month_selected(GtkButton *button, gpointer user_data) {
	DatePickerDialogData *data = user_data;
	guint year, month, day;
	gtk_calendar_get_date(data->calendar, &year, &month, &day);
	month = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "month"));
	gtk_calendar_select_month(data->calendar, month, year);
	update_date_picker_header(data);
	gtk_stack_set_visible_child_name(data->stack, "days");
}

static void on_date_picker_year_selected(GtkButton *button, gpointer user_data) {
	DatePickerDialogData *data = user_data;
	guint year, month, day;
	gtk_calendar_get_date(data->calendar, &year, &month, &day);
	year = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "year"));
	gtk_calendar_select_month(data->calendar, month, year);
	update_date_picker_header(data);
	gtk_stack_set_visible_child_name(data->stack, "months");
}

static void on_date_picker_month_button(GtkButton *button, gpointer user_data) {
	DatePickerDialogData *data = user_data;
	gtk_stack_set_visible_child_name(data->stack, "months");
}

static void on_date_picker_year_button(GtkButton *button, gpointer user_data) {
	DatePickerDialogData *data = user_data;
	guint year, month, day;
	gtk_calendar_get_date(data->calendar, &year, &month, &day);
	data->year_page_start = ((year - 1) / 10) * 10 + 1;
	populate_date_picker_year_grid(data);
	gtk_stack_set_visible_child_name(data->stack, "years");
}

static void on_date_picker_navigate(GtkButton *button, gpointer user_data) {
	DatePickerDialogData *data = user_data;
	int direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
	const char *page = gtk_stack_get_visible_child_name(data->stack);
	if(g_strcmp0(page, "years") == 0) {
		if((direction < 0 && data->year_page_start > 10) || (direction > 0 && data->year_page_start <= 9989))
			data->year_page_start += direction * 10;
		populate_date_picker_year_grid(data);
	} else {
		guint year, month, day;
		gtk_calendar_get_date(data->calendar, &year, &month, &day);
		if(g_strcmp0(page, "months") == 0) {
			if((direction < 0 && year > 1) || (direction > 0 && year < 9999)) {
				year += direction;
				gtk_calendar_select_month(data->calendar, month, year);
			}
		} else {
			if(direction < 0 && month == 0 && year > 1) {
				month = 11;
				year--;
			} else if(direction > 0 && month == 11 && year < 9999) {
				month = 0;
				year++;
			} else if((direction < 0 && month > 0) || (direction > 0 && month < 11)) {
				month += direction;
			}
			gtk_calendar_select_month(data->calendar, month, year);
		}
		update_date_picker_header(data);
	}
}

static void on_date_picker_calendar_month_changed(GtkCalendar *calendar, gpointer user_data) {
	update_date_picker_header(user_data);
}

static void update_date_picker_header(DatePickerDialogData *data) {
	static const char *month_names[] = {
		"January", "February", "March", "April", "May", "June",
		"July", "August", "September", "October", "November", "December"
	};
	guint year, month, day;
	char year_text[8];
	gtk_calendar_get_date(data->calendar, &year, &month, &day);
	gtk_button_set_label(GTK_BUTTON(data->month_button), month_names[month]);
	g_snprintf(year_text, sizeof(year_text), "%u", year);
	gtk_button_set_label(GTK_BUTTON(data->year_button), year_text);
}

static void populate_date_picker_year_grid(DatePickerDialogData *data) {
	GList *children = gtk_container_get_children(GTK_CONTAINER(data->year_grid));
	for(GList *child = children; child != NULL; child = child->next)
		gtk_widget_destroy(GTK_WIDGET(child->data));
	g_list_free(children);

	char range_text[32];
	g_snprintf(range_text, sizeof(range_text), "%u - %u", data->year_page_start,
		MIN(data->year_page_start + 9, 9999));
	gtk_label_set_text(GTK_LABEL(data->year_range_label), range_text);
	for(guint offset = 0; offset < 12; offset++) {
		gint year = (gint)data->year_page_start + (gint)offset - 1;
		if(year < 1 || year > 9999) continue;
		char year_text[8];
		g_snprintf(year_text, sizeof(year_text), "%d", year);
		GtkWidget *year_button = gtk_button_new_with_label(year_text);
		g_object_set_data(G_OBJECT(year_button), "year", GUINT_TO_POINTER((guint)year));
		gtk_widget_set_size_request(year_button, 64, 36);
		gtk_button_set_relief(GTK_BUTTON(year_button), GTK_RELIEF_NONE);
		g_signal_connect(year_button, "clicked", G_CALLBACK(on_date_picker_year_selected), data);
		gtk_grid_attach(GTK_GRID(data->year_grid), year_button, offset % 4, offset / 4, 1, 1);
	}
	gtk_widget_show_all(data->year_grid);
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
	const char *transfer_type_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(cb_ic_transfertype));
	if(transfer_type_id == NULL) return FALSE;
	enum LastTransferType transfer_type = (enum LastTransferType)strtol(transfer_type_id, NULL, 10);
	if(transfer_type != TF_FLYBY && transfer_type != TF_CAPTURE && transfer_type != TF_CIRC) return FALSE;

	ItineraryHohmannReference reference;
	if(!calculate_itinerary_hohmann_reference(departure_body, arrival_body, ic_system->cb,
		ic_dep_periapsis, ic_arr_periapsis, &reference)) return FALSE;
	const char *earliest_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	enum DateType date_type = get_settings_datetime_type();
	if(!is_string_valid_date_format(earliest_text, date_type)) return FALSE;
	double earliest_jd = convert_date_JD(date_from_string((char *)earliest_text, date_type));
	ItineraryHohmannDefaults defaults;
	if(!calculate_itinerary_hohmann_defaults(&reference, transfer_type, date_type, earliest_jd, &defaults))
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
	if(widget == GTK_WIDGET(cb_ic_depbody) || widget == GTK_WIDGET(cb_ic_arrbody))
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
	update_ic_reference_defaults();
}

static void show_date_picker(GtkEntry *entry) {
	enum DateType date_type = get_settings_datetime_type();
	if(date_type != DATE_ISO) {
		show_ordinal_date_picker(entry, date_type);
		return;
	}
	const char *entry_text = gtk_entry_get_text(entry);
	Datetime initial_date;
	if(is_string_valid_date_format(entry_text, date_type)) {
		initial_date = date_from_string((char *)entry_text, date_type);
	} else {
		GDateTime *now = g_date_time_new_now_local();
		initial_date = (Datetime) {
			.y = g_date_time_get_year(now),
			.m = g_date_time_get_month(now),
			.d = g_date_time_get_day_of_month(now),
			.date_type = DATE_ISO
		};
		g_date_time_unref(now);
	}
	if(initial_date.y < 1 || initial_date.y > 9999 || !g_date_valid_dmy(initial_date.d, initial_date.m, initial_date.y))
		initial_date = (Datetime) {.y = 2000, .m = 1, .d = 1, .date_type = DATE_ISO};

	GtkWidget *dialog = gtk_dialog_new_with_buttons(
			"Select Date", GTK_WINDOW(tf_ic_window), GTK_DIALOG_DESTROY_WITH_PARENT,
			"_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
	track_date_picker_dialog(dialog);
	GtkWidget *calendar = gtk_calendar_new();
	GtkWidget *picker = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *previous = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
	GtkWidget *next = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
	GtkWidget *month_button = gtk_button_new();
	GtkWidget *year_button = gtk_button_new();
	GtkWidget *stack = gtk_stack_new();
	GtkWidget *month_grid = gtk_grid_new();
	GtkWidget *year_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *year_range_label = gtk_label_new(NULL);
	GtkWidget *year_grid = gtk_grid_new();
	static const char *month_names[] = {
		"January", "February", "March", "April", "May", "June",
		"July", "August", "September", "October", "November", "December"
	};
	DatePickerDialogData *dialog_data = g_new0(DatePickerDialogData, 1);
	dialog_data->entry = entry;
	dialog_data->calendar = GTK_CALENDAR(calendar);
	dialog_data->date_type = date_type;
	dialog_data->stack = GTK_STACK(stack);
	dialog_data->month_button = month_button;
	dialog_data->year_button = year_button;
	dialog_data->year_range_label = year_range_label;
	dialog_data->year_grid = year_grid;
	gtk_button_set_relief(GTK_BUTTON(previous), GTK_RELIEF_NONE);
	gtk_button_set_relief(GTK_BUTTON(next), GTK_RELIEF_NONE);
	gtk_button_set_relief(GTK_BUTTON(month_button), GTK_RELIEF_NONE);
	gtk_button_set_relief(GTK_BUTTON(year_button), GTK_RELIEF_NONE);
	gtk_widget_set_size_request(month_button, 112, 36);
	gtk_widget_set_size_request(year_button, 64, 36);
	gtk_widget_set_halign(year_range_label, GTK_ALIGN_CENTER);
	gtk_grid_set_row_spacing(GTK_GRID(month_grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(month_grid), 4);
	gtk_grid_set_row_spacing(GTK_GRID(year_grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(year_grid), 4);
	gtk_calendar_select_month(GTK_CALENDAR(calendar), initial_date.m - 1, initial_date.y);
	gtk_calendar_select_day(GTK_CALENDAR(calendar), initial_date.d);
	gtk_calendar_set_display_options(GTK_CALENDAR(calendar), GTK_CALENDAR_SHOW_DAY_NAMES);
	for(guint month = 0; month < G_N_ELEMENTS(month_names); month++) {
		GtkWidget *button = gtk_button_new_with_label(month_names[month]);
		g_object_set_data(G_OBJECT(button), "month", GUINT_TO_POINTER(month));
		gtk_widget_set_size_request(button, 88, 36);
		gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
		g_signal_connect(button, "clicked", G_CALLBACK(on_date_picker_month_selected), dialog_data);
		gtk_grid_attach(GTK_GRID(month_grid), button, month % 3, month / 3, 1, 1);
	}
	gtk_box_pack_start(GTK_BOX(navigation), previous, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(navigation), month_button, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(navigation), year_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(navigation), next, FALSE, FALSE, 0);
	gtk_stack_add_named(GTK_STACK(stack), calendar, "days");
	gtk_stack_add_named(GTK_STACK(stack), month_grid, "months");
	gtk_box_pack_start(GTK_BOX(year_page), year_range_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(year_page), year_grid, TRUE, TRUE, 0);
	gtk_stack_add_named(GTK_STACK(stack), year_page, "years");
	g_object_set_data(G_OBJECT(previous), "direction", GINT_TO_POINTER(-1));
	g_object_set_data(G_OBJECT(next), "direction", GINT_TO_POINTER(1));
	g_signal_connect(previous, "clicked", G_CALLBACK(on_date_picker_navigate), dialog_data);
	g_signal_connect(next, "clicked", G_CALLBACK(on_date_picker_navigate), dialog_data);
	g_signal_connect(month_button, "clicked", G_CALLBACK(on_date_picker_month_button), dialog_data);
	g_signal_connect(year_button, "clicked", G_CALLBACK(on_date_picker_year_button), dialog_data);
	g_signal_connect(calendar, "month-changed", G_CALLBACK(on_date_picker_calendar_month_changed), dialog_data);
	update_date_picker_header(dialog_data);
	gtk_stack_set_visible_child_name(GTK_STACK(stack), "days");
	gtk_box_pack_start(GTK_BOX(picker), navigation, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(picker), stack, TRUE, TRUE, 0);
	gtk_container_set_border_width(GTK_CONTAINER(dialog), 8);
	GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_box_pack_start(GTK_BOX(content_area), picker, TRUE, TRUE, 0);
	g_signal_connect_data(dialog, "response", G_CALLBACK(on_date_picker_response), dialog_data,
			free_date_picker_dialog_data, 0);
	gtk_widget_show_all(dialog);
}

static void show_ordinal_date_picker(GtkEntry *entry, enum DateType date_type) {
	Datetime selected_date = {.y = 1, .d = 1, .date_type = date_type};
	const char *entry_text = gtk_entry_get_text(entry);
	if(is_string_valid_date_format(entry_text, date_type))
		selected_date = date_from_string((char *)entry_text, date_type);

	GtkWidget *dialog = gtk_dialog_new_with_buttons(
		"Select Date", GTK_WINDOW(tf_ic_window), GTK_DIALOG_DESTROY_WITH_PARENT,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
	track_date_picker_dialog(dialog);
	GtkWidget *picker = gtk_grid_new();
	GtkWidget *year_label = gtk_label_new("Year:");
	GtkWidget *day_label = gtk_label_new("Day:");
	GtkWidget *year_input = gtk_spin_button_new_with_range(1, 9999, 1);
	GtkWidget *day_input = gtk_spin_button_new_with_range(1, itinerary_date_picker_day_limit(date_type), 1);
	DatePickerDialogData *dialog_data = g_new0(DatePickerDialogData, 1);
	dialog_data->entry = entry;
	dialog_data->year_input = GTK_SPIN_BUTTON(year_input);
	dialog_data->day_input = GTK_SPIN_BUTTON(day_input);
	dialog_data->date_type = date_type;

	gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(year_input), TRUE);
	gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(day_input), TRUE);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(year_input), selected_date.y);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(day_input), selected_date.d);
	gtk_widget_set_halign(year_label, GTK_ALIGN_END);
	gtk_widget_set_halign(day_label, GTK_ALIGN_END);
	gtk_grid_set_row_spacing(GTK_GRID(picker), 8);
	gtk_grid_set_column_spacing(GTK_GRID(picker), 8);
	gtk_grid_attach(GTK_GRID(picker), year_label, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(picker), year_input, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(picker), day_label, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(picker), day_input, 1, 1, 1, 1);

	gtk_container_set_border_width(GTK_CONTAINER(dialog), 8);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), picker);
	g_signal_connect_data(dialog, "response", G_CALLBACK(on_date_picker_response), dialog_data,
		free_date_picker_dialog_data, 0);
	gtk_widget_show_all(dialog);
}

static void on_date_picker_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
	DatePickerDialogData *dialog_data = user_data;
	if(response_id == GTK_RESPONSE_ACCEPT) {
		Datetime selected_date;
		if(dialog_data->day_input != NULL) {
			selected_date = (Datetime) {
				.y = gtk_spin_button_get_value_as_int(dialog_data->year_input),
				.d = gtk_spin_button_get_value_as_int(dialog_data->day_input),
				.date_type = dialog_data->date_type
			};
		} else {
			guint year, month, day;
			gtk_calendar_get_date(dialog_data->calendar, &year, &month, &day);
			selected_date = (Datetime) {
				.y = year, .m = month + 1, .d = day, .date_type = dialog_data->date_type
			};
		}
		char date_text[32];
		date_to_string(selected_date, date_text, 0);
		gtk_entry_set_text(dialog_data->entry, date_text);
	}
	gtk_widget_destroy(GTK_WIDGET(dialog));
}

G_MODULE_EXPORT void on_ic_date_icon_release(GtkEntry *entry, GtkEntryIconPosition icon_pos, gpointer data) {
	if(icon_pos == GTK_ENTRY_ICON_SECONDARY)
		show_date_picker(entry);
}

G_MODULE_EXPORT void on_ic_time_limit_toggled(GtkToggleButton *button, gpointer data) {
	if(!gtk_toggle_button_get_active(button)) return;
	gboolean use_latest_arrival_date =
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate));
	gtk_widget_set_sensitive(box_ic_maxarrdate, use_latest_arrival_date);
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_maxdur), !use_latest_arrival_date);
	validate_ic_date_range_widgets();
}
