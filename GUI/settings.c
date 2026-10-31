#include "settings.h"
#include "orbitlib.h"
#include "orbit_calculator/flyby_search.h"
#include "gui_manager.h"
#include "ksp/ksp_session.h"
#include "tools/starting_orbit.h"
#include "transfer_app/itinerary_calculator.h"
#include "transfer_app/sequence_calculator.h"
#include "transfer_app/starting_orbit_editor.h"
#include "transfer_app/transfer_planner.h"
#include "tools/celestial_systems.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


GObject *cb_settings_default_system, *cb_settings_seed_budget;
GObject *bt_settings_ksp_connect, *lb_settings_ksp_status;

struct GlobalSettings {
	enum DateType date_type;
	char default_system_name[100];
	char ksp_host[128];
	uint16_t ksp_port;
	int powered_seed_budget;
	} global_settings = {.date_type = DATE_ISO, .default_system_name = "Solar System (Ephemeris)",
		.ksp_host = "127.0.0.1", .ksp_port = 50000, .powered_seed_budget = POWERED_SEEDS_PER_DEPARTURE};

static gchar *get_settings_file_path() {
	return g_build_filename(g_get_user_config_dir(), "kmat", "settings.ini", NULL);
}

static void load_persisted_settings() {
	gchar *path = get_settings_file_path();
	GKeyFile *key_file = g_key_file_new();
	GError *error = NULL;
	if(g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
		gchar *system_name = g_key_file_get_string(key_file, "Settings", "default-celestial-system", NULL);
		if(system_name != NULL && system_name[0] != '\0')
			g_strlcpy(global_settings.default_system_name, system_name, sizeof(global_settings.default_system_name));
		g_free(system_name);
		gchar *ksp_host = g_key_file_get_string(key_file, "Settings", "ksp-host", NULL);
		if(ksp_host != NULL && ksp_host[0] != 0)
			g_strlcpy(global_settings.ksp_host, ksp_host, sizeof(global_settings.ksp_host));
		g_free(ksp_host);
		gint ksp_port = g_key_file_get_integer(key_file, "Settings", "ksp-port", NULL);
		if(ksp_port > 0 && ksp_port <= UINT16_MAX) global_settings.ksp_port = (uint16_t)ksp_port;
		gint seed_budget = g_key_file_get_integer(key_file, "Settings", "powered-seed-budget", NULL);
		if(find_powered_seed_budget_preset(seed_budget) >= 0) global_settings.powered_seed_budget = seed_budget;
	} else {
		g_clear_error(&error);
	}
	g_key_file_unref(key_file);
	g_free(path);
}

static void save_settings() {
	gchar *path = get_settings_file_path();
	gchar *directory = g_path_get_dirname(path);
	GKeyFile *key_file = g_key_file_new();
	GError *error = NULL;
	gsize data_length;

	if(!g_key_file_load_from_file(key_file, path, G_KEY_FILE_KEEP_COMMENTS, &error))
		g_clear_error(&error);
	g_key_file_set_string(key_file, "Settings", "default-celestial-system", global_settings.default_system_name);
	g_key_file_remove_key(key_file, "Settings", "default-departure-date", NULL);
	g_key_file_set_string(key_file, "Settings", "ksp-host", global_settings.ksp_host);
	g_key_file_set_integer(key_file, "Settings", "ksp-port", global_settings.ksp_port);
	g_key_file_set_integer(key_file, "Settings", "powered-seed-budget", global_settings.powered_seed_budget);
	gchar *data = g_key_file_to_data(key_file, &data_length, NULL);
	if(g_mkdir_with_parents(directory, 0700) != 0) {
		g_warning("Failed to create KMAT settings directory: %s", directory);
	} else if(!g_file_set_contents(path, data, data_length, &error)) {
		g_warning("Failed to save KMAT settings: %s", error->message);
		g_clear_error(&error);
	}

	g_free(data);
	g_key_file_unref(key_file);
	g_free(directory);
	g_free(path);
}
void init_global_settings(GtkBuilder *builder) {
	cb_settings_default_system = gtk_builder_get_object(builder, "cb_settings_default_system");
	bt_settings_ksp_connect = gtk_builder_get_object(builder, "bt_settings_ksp_connect");
	lb_settings_ksp_status = gtk_builder_get_object(builder, "lb_settings_ksp_status");
	cb_settings_seed_budget = gtk_builder_get_object(builder, "cb_settings_seed_budget");
	GtkWidget *seed_budget_info = GTK_WIDGET(gtk_builder_get_object(builder, "eb_settings_seed_budget_info"));
	setup_ic_info_icon_hover(seed_budget_info);
	set_stable_tooltip(seed_budget_info, "How thoroughly the itinerary search looks for routes. Higher values find more routes but take longer.");
	load_persisted_settings();
	set_powered_seed_budget(global_settings.powered_seed_budget);
	g_signal_handlers_block_by_func(cb_settings_seed_budget, G_CALLBACK(on_change_seed_budget), NULL);
	gtk_combo_box_set_active(GTK_COMBO_BOX(cb_settings_seed_budget),
		find_powered_seed_budget_preset(global_settings.powered_seed_budget));
	g_signal_handlers_unblock_by_func(cb_settings_seed_budget, G_CALLBACK(on_change_seed_budget), NULL);
	gtk_button_set_label(GTK_BUTTON(bt_settings_ksp_connect), ksp_session_is_connected() ? "Disconnect from KSP" : "Connect to KSP");
	gtk_label_set_text(GTK_LABEL(lb_settings_ksp_status), ksp_session_is_connected() ? "Connected to KSP" : "Not connected");
	char default_system_name[sizeof(global_settings.default_system_name)];
	strcpy(default_system_name, global_settings.default_system_name);
	create_combobox_dropdown_text_renderer(cb_settings_default_system, GTK_ALIGN_CENTER);
	g_signal_handlers_block_by_func(cb_settings_default_system, G_CALLBACK(on_change_default_system), NULL);
	update_system_dropdown(GTK_COMBO_BOX(cb_settings_default_system));
	g_signal_handlers_unblock_by_func(cb_settings_default_system, G_CALLBACK(on_change_default_system), NULL);

	int default_system_index = -1;
	for(int i = 0; i < get_num_available_systems(); i++) {
		if(strcmp(get_available_systems()[i]->name, default_system_name) == 0) {
			default_system_index = i;
			break;
		}
	}
	if(default_system_index < 0 && get_num_available_systems() > 0) default_system_index = 0;
	if(default_system_index >= 0) {
		gtk_combo_box_set_active(GTK_COMBO_BOX(cb_settings_default_system), default_system_index);
		on_change_default_system();
	}
}

enum DateType get_settings_datetime_type() {
	return global_settings.date_type;
}

void set_settings_datetime_type(enum DateType date_type) {
	if(global_settings.date_type == date_type) return;
	enum DateType old_type = global_settings.date_type;
	global_settings.date_type = date_type;
	change_gui_date_type(old_type, date_type);
}

char *get_settings_default_system() {
	return global_settings.default_system_name;
}

G_MODULE_EXPORT void on_change_default_system() {
	int active = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_settings_default_system));
	if(active < 0 || active >= get_num_available_systems()) return;
	strcpy(global_settings.default_system_name, get_available_systems()[active]->name);
	save_settings();
}

G_MODULE_EXPORT void on_change_seed_budget() {
	int active = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_settings_seed_budget));
	if(active < 0 || active >= POWERED_SEED_BUDGET_PRESET_COUNT) return;
	global_settings.powered_seed_budget = get_powered_seed_budget_preset(active);
	set_powered_seed_budget(global_settings.powered_seed_budget);
	save_settings();
}

typedef struct KspSettingsConnectTask {
	GtkWidget *button;
	GtkWidget *status;
	char *host;
	uint16_t port;
	int success;
	int matched_index;
	char error[256];
} KspSettingsConnectTask;

static gboolean finish_ksp_settings_connect(gpointer data) {
	KspSettingsConnectTask *task = data;
	gtk_widget_set_sensitive(task->button, TRUE);
	if(task->success && task->matched_index >= 0 && task->matched_index < get_num_available_systems()) {
		CelestSystem *system = get_available_systems()[task->matched_index];
		g_strlcpy(global_settings.ksp_host, task->host, sizeof(global_settings.ksp_host));
		global_settings.ksp_port = task->port;
		gtk_combo_box_set_active(GTK_COMBO_BOX(cb_settings_default_system), task->matched_index);
		on_change_default_system();
		set_ic_default_system(system->name);
		sc_select_system(task->matched_index);
		tp_select_system(task->matched_index);
		KspVesselStart vessel_start;
		ic_set_ksp_note(NULL);
		char vessel_error[256] = {0};
		if(ksp_session_get_vessel_start(&vessel_start, vessel_error, sizeof(vessel_error))) {
			starting_orbit_set_active(&vessel_start.orbit, vessel_start.orbiting_body);
			if(ic_select_ksp_start(vessel_start.system, vessel_start.orbiting_body, vessel_start.date_jd)) {
				starting_orbit_editor_load(&vessel_start.orbit, vessel_start.orbiting_body);
				char status[256], start_date[40];
				date_to_string(convert_JD_date(vessel_start.date_jd, get_settings_datetime_type()), start_date, 1);
				snprintf(status, sizeof(status), "Connected; %s around %s at %s", vessel_start.vessel_name,
					vessel_start.orbiting_body->name, start_date);
				gtk_label_set_text(GTK_LABEL(task->status), status);
			} else {
				starting_orbit_set_active(NULL, NULL);
				gtk_label_set_text(GTK_LABEL(task->status), "Connected; matched system, but could not select the vessel's departure body");
			}
		} else {
			starting_orbit_set_active(NULL, NULL);
			ic_clear_ksp_start();
			char status[320], note[320];
			snprintf(status, sizeof(status), "Connected; matched %s", system->name);
			snprintf(note, sizeof(note), "KSP: the active vessel is not in a usable starting orbit, so the normal settings are used (%s)", vessel_error);
			ic_set_ksp_note(note);
			gtk_label_set_text(GTK_LABEL(task->status), status);
		}
		save_settings();
		gtk_button_set_label(GTK_BUTTON(task->button), "Disconnect from KSP");
	} else {
		gtk_label_set_text(GTK_LABEL(task->status), task->error[0] != 0 ? task->error : "KSP connection failed");
	}
	g_object_unref(task->button);
	g_object_unref(task->status);
	g_free(task->host);
	g_free(task);
	return G_SOURCE_REMOVE;
}

static gpointer run_ksp_settings_connect(gpointer data) {
	KspSettingsConnectTask *task = data;
	task->success = ksp_session_connect_and_match(task->host, task->port, &task->matched_index,
		task->error, sizeof(task->error));
	g_idle_add(finish_ksp_settings_connect, task);
	return NULL;
}

G_MODULE_EXPORT void on_settings_ksp_connect(GtkWidget *widget, gpointer data) {
	(void)data;
	if(ksp_session_is_connected()) {
		ksp_session_disconnect();
		starting_orbit_set_active(NULL, NULL);
		ic_clear_ksp_start();
		ic_set_ksp_note(NULL);
		gtk_button_set_label(GTK_BUTTON(widget), "Connect to KSP");
		gtk_label_set_text(GTK_LABEL(lb_settings_ksp_status), "Not connected");
		return;
	}
	GtkWidget *parent = gtk_widget_get_toplevel(widget);
	GtkWidget *dialog = gtk_dialog_new_with_buttons("Connect to KSP",
		GTK_IS_WINDOW(parent) ? GTK_WINDOW(parent) : NULL, GTK_DIALOG_MODAL,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Connect", GTK_RESPONSE_ACCEPT, NULL);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_widget_set_margin_start(grid, 12);
	gtk_widget_set_margin_end(grid, 12);
	gtk_widget_set_margin_top(grid, 12);
	gtk_widget_set_margin_bottom(grid, 12);
	GtkWidget *host_label = gtk_label_new("Server host");
	GtkWidget *host_entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(host_entry), global_settings.ksp_host);
	GtkWidget *port_label = gtk_label_new("RPC port");
	GtkWidget *port_entry = gtk_entry_new();
	char port_text[8];
	snprintf(port_text, sizeof(port_text), "%u", (unsigned int)global_settings.ksp_port);
	gtk_entry_set_text(GTK_ENTRY(port_entry), port_text);
	gtk_grid_attach(GTK_GRID(grid), host_label, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), host_entry, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), port_label, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), port_entry, 1, 1, 1, 1);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid);
	gtk_widget_show_all(dialog);
	int response = gtk_dialog_run(GTK_DIALOG(dialog));
	char *host = g_strdup(gtk_entry_get_text(GTK_ENTRY(host_entry)));
	char *port_input = g_strdup(gtk_entry_get_text(GTK_ENTRY(port_entry)));
	gtk_widget_destroy(dialog);
	if(response != GTK_RESPONSE_ACCEPT) {
		g_free(host);
		g_free(port_input);
		return;
	}
	char *end = NULL;
	errno = 0;
	unsigned long port = strtoul(port_input, &end, 10);
	g_free(port_input);
	if(host[0] == 0 || errno != 0 || end == NULL || *end != 0 || port == 0 || port > UINT16_MAX) {
		gtk_label_set_text(GTK_LABEL(lb_settings_ksp_status), "Enter a host and port from 1 to 65535");
		g_free(host);
		return;
	}
	KspSettingsConnectTask *task = g_new0(KspSettingsConnectTask, 1);
	task->button = g_object_ref(widget);
	task->status = g_object_ref(GTK_WIDGET(lb_settings_ksp_status));
	task->host = host;
	task->port = (uint16_t)port;
	gtk_widget_set_sensitive(widget, FALSE);
	gtk_label_set_text(GTK_LABEL(lb_settings_ksp_status), "Connecting to KSP and matching system...");
	GThread *thread = g_thread_new("ksp-session-connect", run_ksp_settings_connect, task);
	if(thread == NULL) {
		gtk_widget_set_sensitive(widget, TRUE);
		gtk_label_set_text(GTK_LABEL(lb_settings_ksp_status), "Could not start KSP connection worker");
		g_object_unref(task->button);
		g_object_unref(task->status);
		g_free(task->host);
		g_free(task);
		return;
	}
	g_thread_unref(thread);
}

void ksp_auto_connect(void) {
	if(bt_settings_ksp_connect == NULL || lb_settings_ksp_status == NULL || ksp_session_is_connected()) return;
	KspSettingsConnectTask *task = g_new0(KspSettingsConnectTask, 1);
	task->button = g_object_ref(GTK_WIDGET(bt_settings_ksp_connect));
	task->status = g_object_ref(GTK_WIDGET(lb_settings_ksp_status));
	task->host = g_strdup(global_settings.ksp_host);
	task->port = global_settings.ksp_port;
	gtk_widget_set_sensitive(task->button, FALSE);
	gtk_label_set_text(GTK_LABEL(lb_settings_ksp_status), "Connecting to KSP and matching system...");
	GThread *thread = g_thread_new("ksp-auto-connect", run_ksp_settings_connect, task);
	if(thread == NULL) {
		gtk_widget_set_sensitive(task->button, TRUE);
		gtk_label_set_text(GTK_LABEL(lb_settings_ksp_status), "Not connected");
		g_object_unref(task->button);
		g_object_unref(task->status);
		g_free(task->host);
		g_free(task);
		return;
	}
	g_thread_unref(thread);
}
