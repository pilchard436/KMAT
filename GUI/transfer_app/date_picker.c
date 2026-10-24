#include "date_picker.h"

#include "gui/settings.h"
#include "tools/itinerary_date_input.h"
#include "tools/itinerary_date_picker_state.h"

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
static gboolean on_date_picker_calendar_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void on_ordinal_date_day_wrapped(GtkSpinButton *spin_button, gpointer user_data);
static void update_date_picker_header(DatePickerDialogData *data);
static void populate_date_picker_year_grid(DatePickerDialogData *data);
static void show_ordinal_date_picker(GtkEntry *entry, enum DateType date_type);

static void free_date_picker_dialog_data(gpointer data, GClosure *closure) {
	(void)closure;
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

void gui_date_picker_close(void) {
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
	(void)button;
	DatePickerDialogData *data = user_data;
	gtk_stack_set_visible_child_name(data->stack, "months");
}

static void on_date_picker_year_button(GtkButton *button, gpointer user_data) {
	(void)button;
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
	(void)calendar;
	update_date_picker_header(user_data);
}

static gboolean on_date_picker_calendar_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
	(void)widget;
	DatePickerDialogData *data = user_data;
	int day_delta;
	switch(event->keyval) {
		case GDK_KEY_Left: day_delta = -1; break;
		case GDK_KEY_Right: day_delta = 1; break;
		case GDK_KEY_Up: day_delta = -7; break;
		case GDK_KEY_Down: day_delta = 7; break;
		default: return FALSE;
	}

	guint year, month, day;
	gtk_calendar_get_date(data->calendar, &year, &month, &day);
	Datetime selected_date = {
		.y = year, .m = month + 1, .d = day, .date_type = DATE_ISO
	};
	if(!itinerary_date_picker_step_date(&selected_date, day_delta)) return TRUE;
	gtk_calendar_select_month(data->calendar, selected_date.m - 1, selected_date.y);
	gtk_calendar_select_day(data->calendar, selected_date.d);
	return TRUE;
}

static void on_ordinal_date_day_wrapped(GtkSpinButton *spin_button, gpointer user_data) {
	DatePickerDialogData *data = user_data;
	int day_limit = itinerary_date_picker_day_limit(data->date_type);
	int wrapped_day = gtk_spin_button_get_value_as_int(spin_button);
	int day_delta;
	if(wrapped_day == 1) day_delta = 1;
	else if(wrapped_day == day_limit) day_delta = -1;
	else return;

	Datetime selected_date = {
		.y = gtk_spin_button_get_value_as_int(data->year_input),
		.d = day_delta > 0 ? day_limit : 1,
		.date_type = data->date_type
	};
	if(!itinerary_date_picker_step_date(&selected_date, day_delta)) {
		gtk_spin_button_set_value(data->day_input, selected_date.d);
		return;
	}

	gtk_spin_button_set_value(data->year_input, selected_date.y);
	gtk_spin_button_set_value(data->day_input, selected_date.d);
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

static void show_iso_date_picker(GtkEntry *entry) {
	enum DateType date_type = get_settings_datetime_type();
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
	if(initial_date.y < 1 || initial_date.y > 9999 ||
		!g_date_valid_dmy(initial_date.d, initial_date.m, initial_date.y))
		initial_date = (Datetime) {.y = 2000, .m = 1, .d = 1, .date_type = DATE_ISO};

	GtkWidget *dialog = gtk_dialog_new_with_buttons(
		"Select Date", GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(entry))), GTK_DIALOG_DESTROY_WITH_PARENT,
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
	gtk_widget_set_can_focus(calendar, TRUE);
	gtk_calendar_select_month(GTK_CALENDAR(calendar), initial_date.m - 1, initial_date.y);
	gtk_calendar_select_day(GTK_CALENDAR(calendar), initial_date.d);
	gtk_calendar_set_display_options(GTK_CALENDAR(calendar), GTK_CALENDAR_SHOW_DAY_NAMES);
	g_signal_connect(calendar, "key-press-event", G_CALLBACK(on_date_picker_calendar_key_press), dialog_data);
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
	gtk_widget_grab_focus(calendar);
}

static void show_ordinal_date_picker(GtkEntry *entry, enum DateType date_type) {
	Datetime selected_date = {.y = 1, .d = 1, .date_type = date_type};
	const char *entry_text = gtk_entry_get_text(entry);
	if(is_string_valid_date_format(entry_text, date_type))
		selected_date = date_from_string((char *)entry_text, date_type);

	GtkWidget *dialog = gtk_dialog_new_with_buttons(
		"Select Date", GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(entry))), GTK_DIALOG_DESTROY_WITH_PARENT,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
	track_date_picker_dialog(dialog);
	GtkWidget *picker = gtk_grid_new();
	GtkWidget *year_label = gtk_label_new("Year:");
	GtkWidget *day_label = gtk_label_new("Day:");
	GtkWidget *year_input = gtk_spin_button_new_with_range(1, 9999, 1);
	GtkWidget *day_input = gtk_spin_button_new_with_range(
		1, itinerary_date_picker_day_limit(date_type), 1);
	DatePickerDialogData *dialog_data = g_new0(DatePickerDialogData, 1);
	dialog_data->entry = entry;
	dialog_data->year_input = GTK_SPIN_BUTTON(year_input);
	dialog_data->day_input = GTK_SPIN_BUTTON(day_input);
	dialog_data->date_type = date_type;

	gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(year_input), TRUE);
	gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(day_input), TRUE);
	gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(day_input), TRUE);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(year_input), selected_date.y);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(day_input), selected_date.d);
	g_signal_connect(day_input, "wrapped", G_CALLBACK(on_ordinal_date_day_wrapped), dialog_data);
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

void gui_date_picker_open(GtkEntry *entry) {
	enum DateType date_type = get_settings_datetime_type();
	if(date_type != DATE_ISO) {
		show_ordinal_date_picker(entry, date_type);
		return;
	}
	show_iso_date_picker(entry);
}

void gui_date_picker_on_entry_icon_release(
		GtkEntry *entry, GtkEntryIconPosition icon_pos, gpointer data) {
	(void)data;
	if(icon_pos == GTK_ENTRY_ICON_SECONDARY)
		gui_date_picker_open(entry);
}
