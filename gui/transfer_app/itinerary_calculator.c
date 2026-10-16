#include "itinerary_calculator.h"
#include "orbit_calculator/transfer_calc.h"
#include "gui/gui_manager.h"
#include "gui/css_loader.h"
#include "gui/settings.h"
#include "gui/info_win_manager.h"
#include "tools/file_io.h"
#include "tools/itinerary_date_limits.h"
#include "tools/itinerary_reference_defaults.h"
#include "tools/thread_pool.h"
#include <math.h>


GObject *tf_ic_window;
GObject *cb_ic_system;
GObject *cb_ic_central_body;
GObject *lb_ic_central_body;
GObject *cb_ic_depbody;
GObject *cb_ic_arrbody;
GObject *tf_ic_mindepdate;
GObject *tf_ic_maxdepdate;
GObject *tf_ic_maxarrdate;
GObject *tf_ic_maxdur;
static GObject *rb_ic_maxarrdate;
static GObject *rb_ic_maxdur;
GObject *cb_ic_transfertype;
static GtkWidget *cb_ic_allow_dsm;
static GtkWidget *cb_ic_allow_resonant;
GObject *tf_ic_depdv;
GObject *tf_ic_satdv;
GObject *vp_ic_fbbodies;
GtkWidget *grid_ic_fbbodies;
static GtkWidget *ic_latest_dep_minus_month_button;
static GtkWidget *ic_latest_dep_plus_month_button;
static GtkWidget *ic_latest_arr_minus_month_button;
static GtkWidget *ic_latest_arr_plus_month_button;
static gboolean ic_changing_date_type;
static gboolean ic_syncing_time_limits;

CelestSystem *ic_system;

double ic_dep_periapsis = 50e3;
double ic_arr_periapsis = 50e3;

GtkWidget * ic_update_seq_body_grid(GObject *viewport, GtkWidget *grid);

typedef struct {
	GtkEntry *entry;
	GtkCalendar *calendar;
	GtkStack *stack;
	GtkWidget *month_button;
	GtkWidget *year_button;
	GtkWidget *year_range_label;
	GtkWidget *year_grid;
	guint year_page_start;
} DatePickerDialogData;

static void on_date_picker_response(GtkDialog *dialog, gint response_id, gpointer user_data);
static void update_date_picker_header(DatePickerDialogData *data);
static void populate_date_picker_year_grid(DatePickerDialogData *data);
static void on_ic_latest_date_adjust(GtkButton *button, gpointer user_data);
static void on_ic_earliest_dep_changed(GtkEditable *editable, gpointer user_data);
static void on_ic_latest_arrival_changed(GtkEditable *editable, gpointer user_data);
static void on_ic_max_duration_changed(GtkEditable *editable, gpointer user_data);
static gboolean update_ic_reference_defaults(void);
static void on_ic_reference_input_changed(GtkWidget *widget, gpointer user_data);

static double adjust_date_by_interval(double jd, const char *interval, int direction, enum DateType date_type) {
	if(g_strcmp0(interval, "week") == 0)
		return jd_change_date(jd, 0, 0, direction * 7, date_type);
	if(g_strcmp0(interval, "month") == 0)
		return date_type == DATE_ISO ? jd_change_date(jd, 0, direction, 0, date_type) : jd_change_date(jd, 0, 0, direction * 30, date_type);
	return jd_change_date(jd, direction, 0, 0, date_type);
}

static void set_entry_date_from_jd(GtkEntry *entry, double jd, enum DateType date_type) {
	Datetime date = convert_JD_date(jd, date_type);
	char date_text[32];
	date_to_string(date, date_text, 0);
	gtk_entry_set_text(entry, date_text);
}

static gboolean get_earliest_departure_jd(double *jd, enum DateType date_type) {
	const char *date_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	if(!is_string_valid_date_format(date_text, date_type)) return FALSE;
	*jd = convert_date_JD(date_from_string((char *)date_text, date_type));
	return TRUE;
}

static void enforce_latest_arrival_after_earliest() {
	if(ic_changing_date_type) return;
	enum DateType date_type = get_settings_datetime_type();
	double earliest_jd;
	if(!get_earliest_departure_jd(&earliest_jd, date_type)) return;
	const char *arrival_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_maxarrdate));
	if(is_string_valid_date_format(arrival_text, date_type) &&
		convert_date_JD(date_from_string((char *)arrival_text, date_type)) > earliest_jd)
		return;
	set_entry_date_from_jd(GTK_ENTRY(tf_ic_maxarrdate),
		jd_change_date(earliest_jd, 0, 0, 1, date_type), date_type);
}

static void connect_date_adjust_buttons(GtkBuilder *builder, const char *prefix, GtkEntry *target) {
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

static void update_month_adjust_button_labels(enum DateType date_type) {
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
	double earliest_jd;
	if(get_earliest_departure_jd(&earliest_jd, date_type)) {
		if(target == GTK_ENTRY(tf_ic_maxarrdate) && adjusted_jd <= earliest_jd)
			adjusted_jd = jd_change_date(earliest_jd, 0, 0, 1, date_type);
		else if(target == GTK_ENTRY(tf_ic_maxdepdate) && adjusted_jd < earliest_jd)
			adjusted_jd = earliest_jd;
	}
	set_entry_date_from_jd(target, adjusted_jd, date_type);
}

static void on_ic_earliest_dep_changed(GtkEditable *editable, gpointer user_data) {
	if(ic_changing_date_type) return;
	const char *date_text = gtk_entry_get_text(GTK_ENTRY(editable));
	if(is_string_valid_date_format(date_text, get_settings_datetime_type())) {
		ic_syncing_time_limits = TRUE;
		gtk_entry_set_text(GTK_ENTRY(tf_ic_maxdepdate), date_text);
		enforce_latest_arrival_after_earliest();
		ic_syncing_time_limits = FALSE;
	}
}

static void on_ic_latest_arrival_changed(GtkEditable *editable, gpointer user_data) {
	if(ic_syncing_time_limits) return;
	enforce_latest_arrival_after_earliest();
	const char *earliest_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	const char *arrival_text = gtk_entry_get_text(GTK_ENTRY(tf_ic_maxarrdate));
	enum DateType date_type = get_settings_datetime_type();
	if(!is_string_valid_date_format(earliest_text, date_type) ||
		!is_string_valid_date_format(arrival_text, date_type)) return;
	double earliest_jd = convert_date_JD(date_from_string((char *)earliest_text, date_type));
	double arrival_jd = convert_date_JD(date_from_string((char *)arrival_text, date_type));
	ItineraryDateLimits limits = calculate_itinerary_date_limits(
		TRUE, earliest_jd, 0, arrival_jd, 0, date_type);
	double user_duration = limits.max_duration_days * (date_type == DATE_KERBAL ? 4.0 : 1.0);
	char duration_text[32];
	g_snprintf(duration_text, sizeof(duration_text), "%.0f", user_duration);
	ic_syncing_time_limits = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate), TRUE);
	gtk_entry_set_text(GTK_ENTRY(tf_ic_maxdur), duration_text);
	ic_syncing_time_limits = FALSE;
}

static void on_ic_max_duration_changed(GtkEditable *editable, gpointer user_data) {
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
	double min_dep_jd = convert_date_JD(date_from_string((char *)min_dep_text, date_type));
	double max_dep_jd = convert_date_JD(date_from_string((char *)max_dep_text, date_type));
	ItineraryDateLimits limits = calculate_itinerary_date_limits(
		FALSE, min_dep_jd, max_dep_jd, 0, user_duration, date_type);
	ic_syncing_time_limits = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb_ic_maxdur), TRUE);
	set_entry_date_from_jd(GTK_ENTRY(tf_ic_maxarrdate), limits.max_arrival_jd, date_type);
	ic_syncing_time_limits = FALSE;
}

static gboolean update_ic_reference_defaults(void) {
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

static void on_ic_reference_input_changed(GtkWidget *widget, gpointer user_data) {
	if(widget == GTK_WIDGET(cb_ic_depbody) || widget == GTK_WIDGET(cb_ic_arrbody))
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
	update_ic_reference_defaults();
}

void init_itinerary_calculator(GtkBuilder *builder) {
	tf_ic_window = gtk_builder_get_object(builder, "window");
	cb_ic_system = gtk_builder_get_object(builder, "cb_ic_system");
	cb_ic_central_body = gtk_builder_get_object(builder, "cb_ic_central_body");
	lb_ic_central_body = gtk_builder_get_object(builder, "lb_ic_central_body");
	cb_ic_depbody = gtk_builder_get_object(builder, "cb_ic_depbody");
	cb_ic_arrbody = gtk_builder_get_object(builder, "cb_ic_arrbody");
	tf_ic_mindepdate = gtk_builder_get_object(builder, "tf_ic_mindepdate");
	tf_ic_maxdepdate = gtk_builder_get_object(builder, "tf_ic_maxdepdate");
	tf_ic_maxarrdate = gtk_builder_get_object(builder, "tf_ic_maxarrdate");
	tf_ic_maxdur = gtk_builder_get_object(builder, "tf_ic_maxdur");
	rb_ic_maxarrdate = gtk_builder_get_object(builder, "rb_ic_maxarrdate");
	rb_ic_maxdur = gtk_builder_get_object(builder, "rb_ic_maxdur");
	g_signal_connect(rb_ic_maxarrdate, "toggled", G_CALLBACK(on_ic_time_limit_toggled), NULL);
	g_signal_connect(rb_ic_maxdur, "toggled", G_CALLBACK(on_ic_time_limit_toggled), NULL);
	on_ic_time_limit_toggled(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate), NULL);
	g_signal_connect(tf_ic_mindepdate, "changed", G_CALLBACK(on_ic_earliest_dep_changed), NULL);
	g_signal_connect(tf_ic_maxarrdate, "changed", G_CALLBACK(on_ic_latest_arrival_changed), NULL);
	g_signal_connect(tf_ic_maxdur, "changed", G_CALLBACK(on_ic_max_duration_changed), NULL);
	connect_date_adjust_buttons(builder, "ic_maxdep", GTK_ENTRY(tf_ic_maxdepdate));
	connect_date_adjust_buttons(builder, "ic_maxarr", GTK_ENTRY(tf_ic_maxarrdate));
	update_month_adjust_button_labels(get_settings_datetime_type());
	if(get_settings_default_departure_date() == DEFAULT_DEPARTURE_DATE_TODAY) {
		GDateTime *now = g_date_time_new_now_local();
		Datetime today = {
			.y = g_date_time_get_year(now),
			.m = g_date_time_get_month(now),
			.d = g_date_time_get_day_of_month(now),
			.date_type = DATE_ISO
		};
		g_date_time_unref(now);
		if(get_settings_datetime_type() != DATE_ISO)
			today = change_date_type(today, get_settings_datetime_type());
		char date_text[32];
		date_to_string(today, date_text, 0);
		gtk_entry_set_text(GTK_ENTRY(tf_ic_mindepdate), date_text);
	}
	cb_ic_transfertype = gtk_builder_get_object(builder, "cb_ic_transfertype");
	cb_ic_allow_dsm = GTK_WIDGET(gtk_builder_get_object(builder, "cb_ic_allow_dsm"));
	if(cb_ic_allow_dsm != NULL)
		set_stable_tooltip(cb_ic_allow_dsm, "Tries one deep-space maneuver per leg on the best routes. Its delta-v counts toward the satellite delta-v limit, so raise that limit above 0 for maneuvers to appear.");
	cb_ic_allow_resonant = GTK_WIDGET(gtk_builder_get_object(builder, "cb_ic_allow_resonant"));
	if(cb_ic_allow_resonant != NULL)
		set_stable_tooltip(cb_ic_allow_resonant, "Also searches flybys that return to the same body after one or more revolutions (for example Earth-Earth). Makes the search several times slower.");
	tf_ic_depdv = gtk_builder_get_object(builder, "tf_ic_depdv");
	tf_ic_satdv = gtk_builder_get_object(builder, "tf_ic_satdv");
	vp_ic_fbbodies = gtk_builder_get_object(builder, "vp_ic_fbbodies");

	ic_system = NULL;

	create_combobox_dropdown_text_renderer(cb_ic_system, GTK_ALIGN_CENTER);
	create_combobox_dropdown_text_renderer(cb_ic_central_body, GTK_ALIGN_CENTER);
	create_combobox_dropdown_text_renderer(cb_ic_depbody, GTK_ALIGN_CENTER);
	create_combobox_dropdown_text_renderer(cb_ic_arrbody, GTK_ALIGN_CENTER);
	update_system_dropdown(GTK_COMBO_BOX(cb_ic_system));
	set_ic_default_system(get_settings_default_system());
	if(get_num_available_systems() > 0) {
		ic_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))];
		update_central_body_dropdown(GTK_COMBO_BOX(cb_ic_central_body), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_depbody), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_arrbody), ic_system);
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
	}
	g_signal_connect(cb_ic_depbody, "changed", G_CALLBACK(on_ic_reference_input_changed), NULL);
	g_signal_connect(cb_ic_arrbody, "changed", G_CALLBACK(on_ic_reference_input_changed), NULL);
	g_signal_connect(cb_ic_transfertype, "changed", G_CALLBACK(on_ic_reference_input_changed), NULL);
	update_ic_reference_defaults();
}

void set_ic_default_system(char *system_name) {
	if(cb_ic_system == NULL || system_name == NULL) return;
	CelestSystem *default_system = get_system_by_name(system_name);
	for(int i = 0; i < get_num_available_systems(); i++) {
		if(get_available_systems()[i] == default_system) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(cb_ic_system), i);
			break;
		}
	}
}

void ic_change_date_type(enum DateType old_date_type, enum DateType new_date_type) {
	ic_changing_date_type = TRUE;
	change_text_field_date_type(tf_ic_mindepdate, old_date_type, new_date_type);
	change_text_field_date_type(tf_ic_maxdepdate, old_date_type, new_date_type);
	change_text_field_date_type(tf_ic_maxarrdate, old_date_type, new_date_type);
	ic_changing_date_type = FALSE;
	update_month_adjust_button_labels(new_date_type);
}

static void show_date_picker(GtkEntry *entry) {
	enum DateType date_type = get_settings_datetime_type();
	const char *entry_text = gtk_entry_get_text(entry);
	Datetime initial_date;
	if(is_string_valid_date_format(entry_text, date_type)) {
		Datetime field_date = date_from_string((char *)entry_text, date_type);
		initial_date = date_type == DATE_ISO ? field_date : convert_JD_date(convert_date_JD(field_date), DATE_ISO);
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

static void on_date_picker_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
	DatePickerDialogData *dialog_data = user_data;
	if(response_id == GTK_RESPONSE_ACCEPT) {
		guint year, month, day;
		gtk_calendar_get_date(dialog_data->calendar, &year, &month, &day);
		Datetime selected_iso = {.y = year, .m = month + 1, .d = day, .date_type = DATE_ISO};
		enum DateType date_type = get_settings_datetime_type();
		Datetime selected_date = date_type == DATE_ISO ? selected_iso : convert_JD_date(convert_date_JD(selected_iso), date_type);
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
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_maxarrdate), TRUE);
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_maxdur), TRUE);
}


GtkWidget * ic_update_seq_body_grid(GObject *viewport, GtkWidget *grid) {
	if (grid != NULL && GTK_WIDGET(viewport) == gtk_widget_get_parent(grid)) {
		gtk_container_remove(GTK_CONTAINER(viewport), grid);
	}

	grid = gtk_grid_new();
	int departure_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody));
	int arrival_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody));
	int first_route_index = MIN(departure_index, arrival_index);
	int last_route_index = MAX(departure_index, arrival_index);

	// Create labels and buttons and add them to the grid
	for (int body_idx = 0; body_idx < ic_system->num_bodies; body_idx++) {
		int row = body_idx;
		GtkWidget *widget;
		// Create a show body check button
		widget = gtk_check_button_new_with_label(ic_system->bodies[body_idx]->name);
		gboolean is_endpoint = body_idx == departure_index || body_idx == arrival_index;
		const char *body_name = ic_system->bodies[body_idx]->name;
		gboolean is_default_excluded = g_ascii_strcasecmp(body_name, "Vesta") == 0 ||
			g_ascii_strcasecmp(body_name, "Ceres") == 0;
		gboolean is_between_endpoints = departure_index >= 0 && arrival_index >= 0 &&
			body_idx >= first_route_index && body_idx <= last_route_index && !is_default_excluded;
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), is_between_endpoints);
		gtk_widget_set_sensitive(widget, !is_endpoint);
		gtk_widget_set_halign(widget, GTK_ALIGN_START);

		// Set the label in the grid at the specified row and column
		gtk_grid_attach(GTK_GRID(grid), widget, 0, row, 1, 1);
	}
	gtk_container_add (GTK_CONTAINER (viewport), grid);
	gtk_widget_show_all(GTK_WIDGET(viewport));

	return grid;
}

int get_num_selected_bodies_from_grid(GtkWidget *grid) {
	int num_bodies = 0;
	for(int i = 0; i < ic_system->num_bodies; i++) {
		if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gtk_grid_get_child_at(GTK_GRID(grid), 0, i))))
			num_bodies++;
	}
	return num_bodies;
}

Body ** get_bodies_from_grid(GtkWidget *grid, int num_bodies) {
	Body **bodies = malloc(num_bodies * sizeof(Body*));
	int idx = 0;
	for(int i = 0; i < ic_system->num_bodies; i++) {
		if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gtk_grid_get_child_at(GTK_GRID(grid), 0, i)))) {
			bodies[idx] = ic_system->bodies[i]; idx++;
		}
	}

	return bodies;
}



struct Itin_Calc_Data ic_calc_data;
struct Itin_Calc_Results ic_results;

void save_itineraries_ic(struct ItinStep **departures, int num_deps, int num_nodes, int num_itins) {
	if(departures == NULL || num_deps == 0) return;
	char filepath[255];
	if(!get_path_from_file_chooser(filepath,  ".itins", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	store_itineraries_in_bfile(departures, num_nodes, num_deps, num_itins, ic_calc_data, ic_system, filepath, get_current_bin_file_type());
}

static gboolean end_ic_calc_thread(gpointer user_data) {
	(void)user_data;
	end_sc_ic_progress_window();
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_window), 1);

	save_itineraries_ic(ic_results.departures, ic_results.num_deps, ic_results.num_nodes, ic_results.num_itins);
	for(int i = 0; i < ic_results.num_deps; i++) free_itinerary(ic_results.departures[i]);
	free(ic_results.departures);
	free(ic_calc_data.seq_info.to_target.flyby_bodies);
	if(ic_results.num_deps == 0) show_msg_window("No itineraries found!");
	return G_SOURCE_REMOVE;
}

static void prepare_ic_calc_data_from_widgets() {
	char *string;

	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	ic_calc_data.jd_min_dep = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdepdate));
	ic_calc_data.jd_max_dep = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	int use_max_arrival_date = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rb_ic_maxarrdate));
	double max_arrival_jd = 0;
	double user_max_duration = 0;
	if(use_max_arrival_date) {
		string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxarrdate));
		max_arrival_jd = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	} else {
		string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdur));
		user_max_duration = strtod(string, NULL);
	}
	ItineraryDateLimits time_limits = calculate_itinerary_date_limits(
		use_max_arrival_date,
		ic_calc_data.jd_min_dep,
		ic_calc_data.jd_max_dep,
		max_arrival_jd,
		user_max_duration,
		get_settings_datetime_type());
	ic_calc_data.jd_max_arr = time_limits.max_arrival_jd;
	ic_calc_data.max_duration = time_limits.max_duration_days;

	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_depdv));
	ic_calc_data.dv_filter.max_depdv = strtod(string, NULL);
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_satdv));
	ic_calc_data.dv_filter.max_satdv = strtod(string, NULL);
	// Departure burn is capped by its own limit and every later burn by the satellite limit.
	ic_calc_data.dv_filter.max_totdv = ic_calc_data.dv_filter.max_depdv + ic_calc_data.dv_filter.max_satdv;
	string = (char*) gtk_combo_box_get_active_id(GTK_COMBO_BOX(cb_ic_transfertype));
	ic_calc_data.dv_filter.last_transfer_type = (int) strtol(string, NULL, 10);
	ic_calc_data.allow_dsm = cb_ic_allow_dsm != NULL &&
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb_ic_allow_dsm));

	ic_calc_data.dv_filter.dep_periapsis = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody))]->atmo_alt + ic_dep_periapsis;
	ic_calc_data.dv_filter.arr_periapsis = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody))]->atmo_alt + ic_arr_periapsis;
	
	ic_calc_data.calc_acc.lambert = 1;
	ic_calc_data.calc_acc.fb_finder = 1;

	ic_calc_data.num_deps_per_date = 500;
	ic_calc_data.step_dep_date = 1;
	ic_calc_data.max_num_waiting_orbits = 0;

	// Make sure arrival body is a fly-by body
	Body *arr_body = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody))];
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gtk_grid_get_child_at(GTK_GRID(grid_ic_fbbodies), 0, get_body_system_id(arr_body, ic_system))), 1);

	struct ItinSequenceInfoToTarget seq_info = {
			.system = ic_system,
			.dep_body = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody))],
			.arr_body = arr_body,
			.num_flyby_bodies = get_num_selected_bodies_from_grid(grid_ic_fbbodies),
			.allow_resonant = cb_ic_allow_resonant != NULL &&
				gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cb_ic_allow_resonant)),
	};
	seq_info.flyby_bodies = get_bodies_from_grid(grid_ic_fbbodies, seq_info.num_flyby_bodies);
	ic_calc_data.seq_info.to_target = seq_info;

}

static gpointer ic_calc_thread(gpointer user_data) {
	(void)user_data;
	ic_results = search_for_itineraries(ic_calc_data);
	g_idle_add(end_ic_calc_thread, NULL);
	return NULL;
}

G_MODULE_EXPORT void on_calc_ic() {
	if(ic_system == NULL) return;
	prepare_ic_calc_data_from_widgets();
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_window), 0);
	prepare_thread_pool_counters();
	init_sc_ic_progress_window();
	GThread *calc_thread = g_thread_new("calc_thread", (GThreadFunc) ic_calc_thread, NULL);
	if(calc_thread != NULL) g_thread_unref(calc_thread);
}

G_MODULE_EXPORT void on_ic_system_change() {
	if(get_num_available_systems() > 0) {
		ic_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))];
		update_central_body_dropdown(GTK_COMBO_BOX(cb_ic_central_body), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_depbody), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_arrbody), ic_system);
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
		update_ic_reference_defaults();
	}
}

G_MODULE_EXPORT void on_ic_central_body_change() {
	if(get_num_available_systems() > 0) {
		if(get_number_of_subsystems(get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))]) == 0) {
			gtk_widget_set_sensitive(GTK_WIDGET(cb_ic_central_body), 0);
			return;
		}
		gtk_widget_set_sensitive(GTK_WIDGET(cb_ic_central_body), 1);
		CelestSystem *ic_og_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))];
		ic_system = get_subsystem_from_system_and_id(ic_og_system, gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_central_body)));
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_depbody), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_arrbody), ic_system);
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
		update_ic_reference_defaults();
	}
}

G_MODULE_EXPORT void on_get_ic_ref_values() {
	if(ic_system == NULL || !update_ic_reference_defaults()) return;
	int departure_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody));
	int arrival_index = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody));
	Body *dep_body = ic_system->bodies[departure_index];
	Body *arr_body = ic_system->bodies[arrival_index];
	ItineraryHohmannReference reference;
	if(!calculate_itinerary_hohmann_reference(dep_body, arr_body, ic_system->cb,
		ic_dep_periapsis, ic_arr_periapsis, &reference)) return;
	double duration = reference.duration_seconds /
		(get_settings_datetime_type() == DATE_KERBAL ? 6 * 60 * 60 : 24 * 60 * 60);

	char msg[256];
	sprintf(msg, "Hohmann Transfer from %s to %s \n(from %.3E km to %.3E km circular orbit):\n\n"
				 "Departure dv: %.0f m/s\n"
				 "Arrival Capture dv: %.0f m/s\n"
				 "Arrival Circularization: %.0f m/s\n"
				 "Duration: %.0f days",
			dep_body->name, arr_body->name, dep_body->orbit.a/1e3, arr_body->orbit.a/1e3,
			reference.departure_dv, reference.arrival_capture_dv, reference.arrival_circularization_dv, duration);
	show_msg_window(msg);
}

void reset_ic() {

}