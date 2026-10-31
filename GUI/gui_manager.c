#include "gui_manager.h"
#include "info_win_manager.h"
#include <gtk/gtk.h>
#include <locale.h>
#include <stdlib.h>
#include "css_loader.h"
#include "settings.h"
#include "transfer_app/transfer_planner.h"
#include "transfer_app/porkchop_analyzer.h"
#include "transfer_app/sequence_calculator.h"
#include "transfer_app/itinerary_calculator.h"
#include "transfer_app/starting_orbit_editor.h"
#include "launch_app/launch_analyzer.h"
#include "launch_app/capability_analyzer.h"
#include "launch_app/launch_parameter_analyzer.h"
#include "database/lv_database.h"
#include "launch_calculator/lv_profile.h"
#include "gui/database_app/mission_db.h"
#include "tools/file_io.h"
#include "tools/version_tool.h"
#include "ksp/ksp_session.h"


#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>

void resolve_win_relative_path(const char *relative_path, char *absolute_path) {
	char full_path[MAX_PATH];
	if (_fullpath(full_path, relative_path, MAX_PATH)) {
		strcpy(absolute_path, full_path);
	} else {
		printf("Error resolving relative path.\n");
		absolute_path[0] = '\0'; // In case of error
	}
}
#endif

struct LV *all_launcher;
int *launcher_ids;
int num_launcher;

char itins_dir[] = "../Itineraries";

void activate_app(GtkApplication *app, gpointer gui_filepath);

void start_gui(const char* gui_filepath) {
	// init launcher from db for launch calc gui
	num_launcher = get_all_launch_vehicles_from_database(&all_launcher, &launcher_ids);
	setlocale(LC_NUMERIC, "C");	// Glade somehow uses commas instead of points for decimals...

	// init app
	GtkApplication *app = gtk_application_new ("org.gtk.example", G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect (app, "activate", G_CALLBACK (activate_app), (gpointer) gui_filepath);
	g_application_run (G_APPLICATION (app), 0, NULL);
	ksp_session_disconnect();
	g_object_unref (app);

	// gui runs...

	// reset transfer gui
	end_starting_orbit_editor();
	reset_ic();
	reset_sc();
	end_porkchop_analyzer();
	end_transfer_planner();
	// reset launch gui
	close_launch_analyzer();
	close_capability_analyzer();
	close_launch_parameter_analyzer();
	// reset db gui
	close_mission_db();
	for(int i = 0; i < num_launcher; i++) free(all_launcher[i].stages);
	free(all_launcher);
	free(launcher_ids);
	all_launcher = NULL;
	launcher_ids = NULL;
	num_launcher = 0;
}

void activate_app(GtkApplication *app, gpointer gui_filepath) {
	/* Construct a GtkBuilder instance and load our UI description */
	setlocale(LC_NUMERIC, "C");	// Glade somehow uses commas instead of points for decimals...
	GtkBuilder *builder = gtk_builder_new ();
	gtk_builder_add_from_file(builder, gui_filepath, NULL);

	gtk_builder_connect_signals(builder, NULL);

	/* Connect signal handlers to the constructed widgets. */
	GObject *window = gtk_builder_get_object (builder, "window");
	gtk_window_set_application(GTK_WINDOW (window), app);
	char version_string[16];
	get_current_version_string_incl_tool_name(version_string);
	gtk_window_set_title(GTK_WINDOW(window), version_string);
	gtk_widget_set_visible(GTK_WIDGET (window), TRUE);

	#ifdef _WIN32
	char css_path[MAX_PATH] = {0};
	char cache_path[MAX_PATH] = {0};

	resolve_win_relative_path("../GUI/theme/breeze-dark-win.css", css_path);
	resolve_win_relative_path("../lib/gdk-pixbuf-2.0/2.10.0/loaders.cache", cache_path);

	g_setenv("GDK_PIXBUF_MODULE_FILE", cache_path, TRUE);

	// Load CSS manually if needed
	set_window_style_css(css_path);
	#endif
	load_css("../GUI/theme/style.css");

	// init settings page
	init_global_settings(builder);
	// init transfer planner page
	init_itinerary_calculator(builder);
	init_starting_orbit_editor(builder);
	init_sequence_calculator(builder);
	init_porkchop_analyzer(builder);
	init_transfer_planner(builder);
	// init launch calc page
	init_launch_analyzer(builder);
	init_capability_analyzer(builder);
	init_launch_parameter_analyzer(builder);
	// init db page
	init_mission_db(builder);
	// init progress window
	init_info_windows(builder);
	ic_initialize_date_defaults();
	ksp_auto_connect();

	if(!is_latest_version()) {
		char msg[100];
		char current_version_string[16];
		char latest_release_version_string[16];
		get_current_version_string(current_version_string);
		get_latest_release_version_string(latest_release_version_string);
		sprintf(msg, "A new version of KMAT is available!\n"
				"Current version: %s\n"
				"Latest version: %s", current_version_string, latest_release_version_string);
		show_msg_window(msg);
	}

	/* We do not need the builder anymore */
	g_object_unref(builder);
}

int get_path_from_file_chooser(char *filepath, char *extension, GtkFileChooserAction action, char *initial_name) {
	create_directory_if_not_exists(get_itins_directory());
	#ifdef _WIN32
		OPENFILENAME ofn;
		char initialDir[MAX_PATH] = {0};
		char szFile[MAX_PATH] = {0};
		char filter[100];

		ZeroMemory(&ofn, sizeof(ofn));
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = NULL;  // If using a window, set the handle here


		// Convert relative directory path to an absolute path
		resolve_win_relative_path(get_itins_directory(), initialDir);

		// Set file filter dynamically based on the extension
		snprintf(filter, sizeof(filter), "%s Files%c*%s%cAll Files (*.*)%c*.*%c",
				 extension, '\0', extension, '\0', '\0', '\0');
		ofn.lpstrFilter = filter;
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrInitialDir = initialDir;
		ofn.lpstrDefExt = extension;

		if (action == GTK_FILE_CHOOSER_ACTION_SAVE) {
			ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
			if (GetSaveFileNameA(&ofn)) {
				strcpy(filepath, szFile);
			} else {
				filepath[0] = '\0';
			}
		} else {  // Open file dialog
			ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST;
			if (GetOpenFileNameA(&ofn)) {
				strcpy(filepath, szFile);
			} else {
				filepath[0] = '\0';
			}
		}
	#else
		GtkWidget *dialog;
		gint res;

		if(action == GTK_FILE_CHOOSER_ACTION_SAVE) {
			dialog = gtk_file_chooser_dialog_new("Save File", NULL, action,
												 "_Cancel", GTK_RESPONSE_CANCEL,
												 "_Save", GTK_RESPONSE_ACCEPT,
												 NULL);
			gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), initial_name);
		} else {
			dialog = gtk_file_chooser_dialog_new("Open File", NULL, action,
												 "_Cancel", GTK_RESPONSE_CANCEL,
												 "_Open", GTK_RESPONSE_ACCEPT,
												 NULL);
		}

		// Set initial folder
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), get_itins_directory());

		// Create a filter for files with the extension .itin
		GtkFileFilter *filter = gtk_file_filter_new();

		char extension_filter[20];
		sprintf(extension_filter, "*%s", extension);
		gtk_file_filter_add_pattern(filter, extension_filter);
		gtk_file_filter_set_name(filter, extension);
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

		// Run the dialog
		res = gtk_dialog_run(GTK_DIALOG(dialog));
		if (res == GTK_RESPONSE_ACCEPT) {
			char *filepath_temp;
			GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
			filepath_temp = gtk_file_chooser_get_filename(chooser);

			sprintf(filepath, "%s", filepath_temp);
			g_free(filepath_temp);
		}
		// Destroy the dialog
		gtk_widget_destroy(dialog);
	#endif

	// check if file or path was selected
	if(filepath[0] == '\0') return 0;
	else return 1;
}

void create_combobox_dropdown_text_renderer(GObject *combo_box, GtkAlign align) {
	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo_box), renderer, TRUE);
	gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo_box), renderer, "text", 0, NULL);
	if(align == GTK_ALIGN_CENTER) g_object_set(renderer, "xalign", 0.5, NULL);
}

typedef struct StableTooltip {
	GtkWidget *popover;
	GtkWidget *label;
	guint timer_id;
	gboolean entry_icon_only;
	GtkEntryIconPosition icon_position;
} StableTooltip;

static void hide_stable_tooltip(StableTooltip *tip) {
	if(tip->timer_id != 0) {
		g_source_remove(tip->timer_id);
		tip->timer_id = 0;
	}
	gtk_widget_hide(tip->popover);
}

static gboolean show_stable_tooltip(gpointer data) {
	StableTooltip *tip = data;
	tip->timer_id = 0;
	gtk_widget_show_all(tip->popover);
	return G_SOURCE_REMOVE;
}

static void schedule_stable_tooltip(StableTooltip *tip) {
	if(tip->timer_id == 0 && !gtk_widget_get_visible(tip->popover))
		tip->timer_id = g_timeout_add(500, show_stable_tooltip, tip);
}

// Event coordinates over an entry icon are relative to the icon's own window, so query the pointer instead.
static gboolean stable_tooltip_pointer_over_entry_icon(GtkWidget *widget, StableTooltip *tip) {
	if(!tip->entry_icon_only || !GTK_IS_ENTRY(widget)) return FALSE;
	GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
	GdkWindow *top_window = gtk_widget_get_window(toplevel);
	GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(widget));
	if(top_window == NULL || seat == NULL) return FALSE;
	gint px, py, ex, ey;
	gdk_window_get_device_position(top_window, gdk_seat_get_pointer(seat), &px, &py, NULL);
	if(!gtk_widget_translate_coordinates(toplevel, widget, px, py, &ex, &ey)) return FALSE;
	return gtk_entry_get_icon_at_pos(GTK_ENTRY(widget), ex, ey) == (gint)tip->icon_position;
}

static gboolean on_stable_tooltip_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer data) {
	StableTooltip *tip = data;
	// Grab-related crossings follow a click and must not reopen the tooltip.
	if(event->mode == GDK_CROSSING_NORMAL &&
		(!tip->entry_icon_only || stable_tooltip_pointer_over_entry_icon(widget, tip)))
		schedule_stable_tooltip(tip);
	return FALSE;
}

static gboolean on_stable_tooltip_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data) {
	StableTooltip *tip = data;
	if(!tip->entry_icon_only) return FALSE;
	if(stable_tooltip_pointer_over_entry_icon(widget, tip))
		schedule_stable_tooltip(tip);
	else
		hide_stable_tooltip(tip);
	return FALSE;
}

static gboolean on_stable_tooltip_hide(GtkWidget *widget, GdkEvent *event, gpointer data) {
	StableTooltip *tip = data;
	// Moving between an entry's text area and its icon also emits leave events.
	if(event->type == GDK_LEAVE_NOTIFY && stable_tooltip_pointer_over_entry_icon(widget, tip)) return FALSE;
	hide_stable_tooltip(tip);
	return FALSE;
}

static void on_stable_tooltip_unmap(GtkWidget *widget, gpointer data) {
	hide_stable_tooltip(data);
}

static void on_stable_tooltip_widget_destroy(GtkWidget *widget, gpointer data) {
	StableTooltip *tip = data;
	if(tip->timer_id != 0) g_source_remove(tip->timer_id);
	gtk_widget_destroy(tip->popover);
	g_free(tip);
}

static void set_stable_tooltip_content(GtkWidget *widget, const char *text, gboolean use_markup) {
	StableTooltip *tip = g_object_get_data(G_OBJECT(widget), "stable-tooltip");
	if(tip == NULL) {
		tip = g_new0(StableTooltip, 1);
		tip->popover = gtk_popover_new(widget);
		gtk_popover_set_modal(GTK_POPOVER(tip->popover), FALSE);
		gtk_popover_set_position(GTK_POPOVER(tip->popover), GTK_POS_BOTTOM);
		gtk_container_set_border_width(GTK_CONTAINER(tip->popover), 6);
		tip->label = gtk_label_new(NULL);
		gtk_label_set_line_wrap(GTK_LABEL(tip->label), TRUE);
		gtk_label_set_line_wrap_mode(GTK_LABEL(tip->label), PANGO_WRAP_WORD_CHAR);
		gtk_label_set_max_width_chars(GTK_LABEL(tip->label), 50);
		gtk_label_set_xalign(GTK_LABEL(tip->label), 0.0f);
		gtk_container_add(GTK_CONTAINER(tip->popover), tip->label);
		gtk_widget_add_events(widget, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
			GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
		g_signal_connect(widget, "enter-notify-event", G_CALLBACK(on_stable_tooltip_enter), tip);
		g_signal_connect(widget, "motion-notify-event", G_CALLBACK(on_stable_tooltip_motion), tip);
		g_signal_connect(widget, "leave-notify-event", G_CALLBACK(on_stable_tooltip_hide), tip);
		g_signal_connect(widget, "button-press-event", G_CALLBACK(on_stable_tooltip_hide), tip);
		g_signal_connect(widget, "unmap", G_CALLBACK(on_stable_tooltip_unmap), tip);
		g_signal_connect(widget, "destroy", G_CALLBACK(on_stable_tooltip_widget_destroy), tip);
		g_object_set_data(G_OBJECT(widget), "stable-tooltip", tip);
	}
	if(use_markup)
		gtk_label_set_markup(GTK_LABEL(tip->label), text);
	else
		gtk_label_set_text(GTK_LABEL(tip->label), text);
}

void set_stable_tooltip(GtkWidget *widget, const char *text) {
	set_stable_tooltip_content(widget, text, FALSE);
}

void set_stable_tooltip_at(GtkWidget *widget, const char *text, double x, double y) {
	set_stable_tooltip_content(widget, text, FALSE);
	StableTooltip *tip = g_object_get_data(G_OBJECT(widget), "stable-tooltip");
	GdkRectangle pointing_to = {(gint)x, (gint)y, 1, 1};
	gtk_popover_set_pointing_to(GTK_POPOVER(tip->popover), &pointing_to);
	schedule_stable_tooltip(tip);
}

void set_stable_tooltip_markup(GtkWidget *widget, const char *markup) {
	set_stable_tooltip_content(widget, markup, TRUE);
}

void set_stable_tooltip_for_entry_icon(GtkEntry *entry, GtkEntryIconPosition icon_position, const char *text) {
	set_stable_tooltip_content(GTK_WIDGET(entry), text, FALSE);
	StableTooltip *tip = g_object_get_data(G_OBJECT(entry), "stable-tooltip");
	tip->entry_icon_only = TRUE;
	tip->icon_position = icon_position;
}

void append_combobox_entry(GtkComboBox *combo_box, char *new_entry) {
	GtkListStore *store = GTK_LIST_STORE(gtk_combo_box_get_model(combo_box));
	GtkTreeIter iter;
	gtk_list_store_append(store, &iter);  // Appends to the end
	gtk_list_store_set(store, &iter, 0, new_entry, -1);
	gtk_combo_box_set_active_iter(combo_box, &iter);
}

void remove_combobox_last_entry(GtkComboBox *combo_box) {
	GtkListStore *store = GTK_LIST_STORE(gtk_combo_box_get_model(combo_box));
	
	GtkTreeIter iter, last_iter;
	gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
	
	if (!valid) {
		// List is empty, nothing to remove
		return;
	}
	
	last_iter = iter;  // In case there's only one row
	while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter)) {
		last_iter = iter;  // Keep updating to track the last one
	}
	
	gtk_list_store_remove(store, &last_iter);
}

void change_text_field_date_type(GObject *text_field, enum DateType old_date_type, enum DateType new_date_type) {
	char *old_string, new_string[32];
	old_string = (char*) gtk_entry_get_text(GTK_ENTRY(text_field));
	if(!is_string_valid_date_format(old_string, old_date_type)) return;
	struct Datetime date = date_from_string(old_string, old_date_type);
	date = change_date_type(date, new_date_type);
	date_to_string(date, new_string, 0);
	gtk_entry_set_text(GTK_ENTRY(text_field), new_string);
}

void change_label_date_type(GObject *label, enum DateType old_date_type, enum DateType new_date_type) {
	char *old_string, new_string[32];
	old_string = (char*) gtk_label_get_text(GTK_LABEL(label));
	if(!is_string_valid_date_format(old_string, old_date_type)) return;
	struct Datetime date = date_from_string(old_string, old_date_type);
	date = change_date_type(date, new_date_type);
	date_to_string(date, new_string, 0);
	gtk_label_set_text(GTK_LABEL(label), new_string);
}

void change_button_date_type(GObject *button, enum DateType old_date_type, enum DateType new_date_type) {
	char *old_string, new_string[32];
	old_string = (char*) gtk_button_get_label(GTK_BUTTON(button));
	if(!is_string_valid_date_format(old_string, old_date_type)) return;
	struct Datetime date = date_from_string(old_string, old_date_type);
	date = change_date_type(date, new_date_type);
	date_to_string(date, new_string, 0);
	gtk_button_set_label(GTK_BUTTON(button), new_string);
}

// settings stuff -------------------------------------------------------------------------
void change_gui_date_type(enum DateType old_date_type, enum DateType new_date_type) {
	ic_change_date_type(old_date_type, new_date_type);
	sc_change_date_type(old_date_type, new_date_type);
	pa_change_date_type(old_date_type, new_date_type);
	tp_change_date_type(old_date_type, new_date_type);
}


// transfer calc gui stuff ----------------------------------------------------------------
char * get_itins_directory() {
	return itins_dir;
}

void update_system_dropdown(GtkComboBox *cb_sel_system) {
	if(cb_sel_system == NULL) return;
	GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
	GtkTreeIter iter;
	for(int i = 0; i < get_num_available_systems(); i++) {
		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter, 0, get_available_systems()[i]->name, 1, i, -1);
	}
	gtk_combo_box_set_model(cb_sel_system, GTK_TREE_MODEL(store));
	gtk_combo_box_set_active(cb_sel_system, 0);
	g_object_unref(store);
}

void update_central_body_dropdown(GtkComboBox *cb_sel_central_body, CelestSystem *system) {
	GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
	GtkTreeIter iter;
	// Add items to the list store
	for(int i = 0; i <= get_number_of_subsystems(system); i++) {
		gtk_list_store_append(store, &iter);
		char entry[50];
		sprintf(entry, "%s", get_subsystem_from_system_and_id(system, i)->cb->name);
		gtk_list_store_set(store, &iter, 0, entry, 1, i, -1);
	}
	
	gtk_combo_box_set_model(cb_sel_central_body, GTK_TREE_MODEL(store));
	gtk_combo_box_set_active(cb_sel_central_body, 0);
	
	g_object_unref(store);
}


void update_body_dropdown(GtkComboBox *cb_sel_body, CelestSystem *system) {
	GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
	if(system != NULL) {
		GtkTreeIter iter;
		// Add items to the list store
		for(int i = 0; i < system->num_bodies; i++) {
			gtk_list_store_append(store, &iter);
			char entry[32];
			sprintf(entry, "%s", system->bodies[i]->name);
			gtk_list_store_set(store, &iter, 0, entry, 1, i, -1);
		}
	}

	gtk_combo_box_set_model(cb_sel_body, GTK_TREE_MODEL(store));
	if(system && system->home_body) gtk_combo_box_set_active(cb_sel_body, get_body_system_id(system->home_body, system));
	else gtk_combo_box_set_active(cb_sel_body, 0);

	g_object_unref(store);
}


// launch calc gui stuff ----------------------------------------------------------------
void update_launcher_dropdown(GtkComboBox *cb_sel_launcher) {
	GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
	GtkTreeIter iter;
	// Add items to the list store
	for(int i = 0; i < num_launcher; i++) {
		gtk_list_store_append(store, &iter);
		char entry[32];
		sprintf(entry, "%s", all_launcher[i].name);
		gtk_list_store_set(store, &iter, 0, entry, 1, i, -1);
	}

	gtk_combo_box_set_model(cb_sel_launcher, GTK_TREE_MODEL(store));
	gtk_combo_box_set_active(cb_sel_launcher, 0);

	g_object_unref(store);
}

void update_profile_dropdown(GtkComboBox *cb_sel_launcher, GtkComboBox *cb_sel_profile) {
	int id = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_sel_launcher));
	GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
	GtkTreeIter iter;
	if(id >= 0 && id < num_launcher) {
		struct LaunchProfiles_DB profiles = db_get_launch_profiles_from_lv_id(launcher_ids[id]);
		for(int i = 0; i < profiles.num_profiles; i++) {
			gtk_list_store_append(store, &iter);
			char entry[50];
			switch(profiles.profile[i].profiletype) {
				case 1: sprintf(entry, "p = %.0f", profiles.profile[i].lp_params[0]);
					break;
				case 2: sprintf(entry, "p = 90*exp(-%f*h)", profiles.profile[i].lp_params[0]);
					break;
				case 3: sprintf(entry, "p = (90-%.0f)*exp(-%f*h) + %.0f",
								profiles.profile[i].lp_params[1],
								profiles.profile[i].lp_params[0],
								profiles.profile[i].lp_params[1]);
					break;
				case 4: sprintf(entry, "p4(%f, %f, %f)",
								profiles.profile[i].lp_params[0],
								profiles.profile[i].lp_params[2],
								profiles.profile[i].lp_params[1]);
					break;
				default: sprintf(entry, "Unknown profile");
			}
			gtk_list_store_set(store, &iter, 0, entry, 1, i, -1);
		}
	}

	gtk_combo_box_set_model(GTK_COMBO_BOX(cb_sel_profile), GTK_TREE_MODEL(store));
	gtk_combo_box_set_active(GTK_COMBO_BOX(cb_sel_profile), 0);

	g_object_unref(store);
}

struct LV * get_all_launcher() {
	return all_launcher;
}

int * get_launcher_ids() {
	return launcher_ids;
}
