#include "itinerary_calculator.h"
#include "orbit_calculator/transfer_calc.h"
#include "gui/gui_manager.h"
#include "gui/css_loader.h"
#include "gui/settings.h"
#include "gui/info_win_manager.h"
#include "tools/file_io.h"


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
GObject *cb_ic_transfertype;
GObject *tf_ic_totdv;
GObject *tf_ic_depdv;
GObject *tf_ic_satdv;
GObject *vp_ic_fbbodies;
GtkWidget *grid_ic_fbbodies;
static GtkWidget *ic_latest_dep_minus_month_button;
static GtkWidget *ic_latest_dep_plus_month_button;
static GtkWidget *ic_latest_arr_minus_month_button;
static GtkWidget *ic_latest_arr_plus_month_button;
static gboolean ic_changing_date_type;

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
		gtk_widget_set_tooltip_text(button, tooltips[i]);
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
			gtk_widget_set_tooltip_text(minus_buttons[i], date_type == DATE_ISO ? "Subtract one month" : "Subtract 30 days");
		}
		if(plus_buttons[i] != NULL) {
			gtk_button_set_label(GTK_BUTTON(plus_buttons[i]), plus_label);
			gtk_widget_set_tooltip_text(plus_buttons[i], date_type == DATE_ISO ? "Add one month" : "Add 30 days");
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
		gtk_entry_set_text(GTK_ENTRY(tf_ic_maxdepdate), date_text);
		enforce_latest_arrival_after_earliest();
	}
}

static void on_ic_latest_arrival_changed(GtkEditable *editable, gpointer user_data) {
	enforce_latest_arrival_after_earliest();
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
	g_signal_connect(tf_ic_mindepdate, "changed", G_CALLBACK(on_ic_earliest_dep_changed), NULL);
	g_signal_connect(tf_ic_maxarrdate, "changed", G_CALLBACK(on_ic_latest_arrival_changed), NULL);
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
	tf_ic_maxdur = gtk_builder_get_object(builder, "tf_ic_maxdur");
	cb_ic_transfertype = gtk_builder_get_object(builder, "cb_ic_transfertype");
	tf_ic_totdv = gtk_builder_get_object(builder, "tf_ic_totdv");
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


GtkWidget * ic_update_seq_body_grid(GObject *viewport, GtkWidget *grid) {
	if (grid != NULL && GTK_WIDGET(viewport) == gtk_widget_get_parent(grid)) {
		gtk_container_remove(GTK_CONTAINER(viewport), grid);
	}

	grid = gtk_grid_new();

	// Create labels and buttons and add them to the grid
	for (int body_idx = 0; body_idx < ic_system->num_bodies; body_idx++) {
		int row = body_idx;
		GtkWidget *widget;
		// Create a show body check button
		widget = gtk_check_button_new_with_label(ic_system->bodies[body_idx]->name);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), 1);
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

gboolean end_ic_calc_thread() {
	end_sc_ic_progress_window();
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_window), 1);

	save_itineraries_ic(ic_results.departures, ic_results.num_deps, ic_results.num_nodes, ic_results.num_itins);
	for(int i = 0; i < ic_results.num_deps; i++) free_itinerary(ic_results.departures[i]);
	free(ic_results.departures);
	free(ic_calc_data.seq_info.to_target.flyby_bodies);
	if(ic_results.num_deps == 0) show_msg_window("No itineraries found!");
	return G_SOURCE_REMOVE;
}

void ic_calc_thread() {
	char *string;

	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_mindepdate));
	ic_calc_data.jd_min_dep = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdepdate));
	ic_calc_data.jd_max_dep = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxarrdate));
	ic_calc_data.jd_max_arr = convert_date_JD(date_from_string(string, get_settings_datetime_type()));
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_maxdur));
	ic_calc_data.max_duration = strtod(string, NULL);
	if(get_settings_datetime_type() == DATE_KERBAL) ic_calc_data.max_duration /= 4;	// kerbal day is 4 times shorter (24h/6h)

	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_totdv));
	ic_calc_data.dv_filter.max_totdv = strtod(string, NULL);
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_depdv));
	ic_calc_data.dv_filter.max_depdv = strtod(string, NULL);
	string = (char*) gtk_entry_get_text(GTK_ENTRY(tf_ic_satdv));
	ic_calc_data.dv_filter.max_satdv = strtod(string, NULL);
	string = (char*) gtk_combo_box_get_active_id(GTK_COMBO_BOX(cb_ic_transfertype));
	ic_calc_data.dv_filter.last_transfer_type = (int) strtol(string, NULL, 10);

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
	};
	seq_info.flyby_bodies = get_bodies_from_grid(grid_ic_fbbodies, seq_info.num_flyby_bodies);
	ic_calc_data.seq_info.to_target = seq_info;

	ic_results = search_for_itineraries(ic_calc_data);

	// GUI stuff needs to happen in main thread
	g_idle_add((GSourceFunc)end_ic_calc_thread, NULL);
}

G_MODULE_EXPORT void on_calc_ic() {
	if(ic_system == NULL) return;
	gtk_widget_set_sensitive(GTK_WIDGET(tf_ic_window), 0);
	g_thread_new("calc_thread", (GThreadFunc) ic_calc_thread, NULL);
	init_sc_ic_progress_window();
}

G_MODULE_EXPORT void on_ic_system_change() {
	if(get_num_available_systems() > 0) {
		ic_system = get_available_systems()[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_system))];
		update_central_body_dropdown(GTK_COMBO_BOX(cb_ic_central_body), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_depbody), ic_system);
		update_body_dropdown(GTK_COMBO_BOX(cb_ic_arrbody), ic_system);
		grid_ic_fbbodies = ic_update_seq_body_grid(vp_ic_fbbodies, grid_ic_fbbodies);
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
	}
}

G_MODULE_EXPORT void on_get_ic_ref_values() {
	if(ic_system == NULL) return;
	double dv_dep, dv_arr_cap, dv_arr_circ, dur;
	Body *dep_body = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_depbody))];
	Body *arr_body = ic_system->bodies[gtk_combo_box_get_active(GTK_COMBO_BOX(cb_ic_arrbody))];

	if(dep_body == arr_body) return;
	
	Hohmann hohmann = calc_hohmann_transfer(dep_body->orbit.a, arr_body->orbit.a, ic_system->cb);
	
	dur = hohmann.dur;
	dv_dep = dv_circ(dep_body, altatmo2radius(dep_body,ic_dep_periapsis), hohmann.dv_dep);
	dv_arr_circ = dv_circ(arr_body, altatmo2radius(arr_body,ic_arr_periapsis), hohmann.dv_arr);
	dv_arr_cap = dv_capture(arr_body, altatmo2radius(arr_body,ic_arr_periapsis), hohmann.dv_arr);
	
	dur /= get_settings_datetime_type() != DATE_KERBAL ? (24*60*60) : (6*60*60);

	char msg[256];
	sprintf(msg, "Hohmann Transfer from %s to %s \n(from %.3E km to %.3E km circular orbit):\n\n"
				 "Departure dv: %.0f m/s\n"
				 "Arrival Capture dv: %.0f m/s\n"
				 "Arrival Circularization: %.0f m/s\n"
				 "Duration: %.0f days",
			dep_body->name, arr_body->name, dep_body->orbit.a/1e3, arr_body->orbit.a/1e3, dv_dep, dv_arr_cap, dv_arr_circ, dur);
	show_msg_window(msg);
}

void reset_ic() {

}