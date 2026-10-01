#include "settings.h"
#include "orbitlib.h"
#include "gui_manager.h"
#include <string.h>


GObject *cb_settings_datetime_type;
GObject *cb_settings_default_system;
GObject *cb_settings_default_departure_date;

struct GlobalSettings {
	enum DateType date_type;
	char default_system_name[100];
	enum DefaultDepartureDate default_departure_date;
} global_settings = {DATE_ISO, "Solar System (Ephemeris)", DEFAULT_DEPARTURE_DATE_1950};

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
		gchar *departure_date = g_key_file_get_string(key_file, "Settings", "default-departure-date", NULL);
		if(g_strcmp0(departure_date, "today") == 0)
			global_settings.default_departure_date = DEFAULT_DEPARTURE_DATE_TODAY;
		g_free(departure_date);
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
	g_key_file_set_string(key_file, "Settings", "default-departure-date",
		global_settings.default_departure_date == DEFAULT_DEPARTURE_DATE_TODAY ? "today" : "1950-01-01");
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
	cb_settings_datetime_type = gtk_builder_get_object(builder, "cb_settings_datetime_type");
	cb_settings_default_system = gtk_builder_get_object(builder, "cb_settings_default_system");
	cb_settings_default_departure_date = gtk_builder_get_object(builder, "cb_settings_default_departure_date");
	load_persisted_settings();
	g_signal_handlers_block_by_func(cb_settings_default_departure_date, G_CALLBACK(on_change_default_departure_date), NULL);
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(cb_settings_default_departure_date),
		global_settings.default_departure_date == DEFAULT_DEPARTURE_DATE_TODAY ? "today" : "1950");
	g_signal_handlers_unblock_by_func(cb_settings_default_departure_date, G_CALLBACK(on_change_default_departure_date), NULL);
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

G_MODULE_EXPORT void on_change_datetime_type() {
	enum DateType old_type = global_settings.date_type;
	global_settings.date_type = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_settings_datetime_type));
	change_gui_date_type(old_type, global_settings.date_type);
}

enum DateType get_settings_datetime_type() {
	return global_settings.date_type;
}

enum DefaultDepartureDate get_settings_default_departure_date() {
	return global_settings.default_departure_date;
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

G_MODULE_EXPORT void on_change_default_departure_date() {
	const char *active_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(cb_settings_default_departure_date));
	if(active_id == NULL) return;
	global_settings.default_departure_date = g_strcmp0(active_id, "today") == 0 ?
		DEFAULT_DEPARTURE_DATE_TODAY : DEFAULT_DEPARTURE_DATE_1950;
	save_settings();
}