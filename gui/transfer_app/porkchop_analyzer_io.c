#include "porkchop_analyzer_internal.h"
#include "tools/file_io.h"
#include "orbit_calculator/flyby_policy.h"
#include "tools/starting_orbit.h"
#include <stdlib.h>
#include <string.h>

static void apply_pa_search_options(ItinSearchOptions options, CelestSystem *system) {
	set_moon_assist_enabled(options.known && options.moon_assist_arrival);
	set_moon_aware_flybys_enabled(options.known && options.moon_aware_flybys);
	if(options.known && options.use_custom_starting_orbit && system != NULL &&
		options.starting_orbit_body_id >= 0 && options.starting_orbit_body_id < system->num_bodies)
		starting_orbit_set_active(&options.starting_orbit, system->bodies[options.starting_orbit_body_id]);
	else
		starting_orbit_set_active(NULL, NULL);
}

G_MODULE_EXPORT void on_load_itineraries(GtkWidget* widget, gpointer data) {
	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itins", GTK_FILE_CHOOSER_ACTION_OPEN, "")) return;

	gint64 load_started = g_get_monotonic_time();
	reset_pa_porkchop_view();
	free_all_porkchop_analyzer_itins();
	pa_set_last_transfer_type(TF_FLYBY, 0);
	struct ItinsLoadFileResults load_results = load_itineraries_from_bfile(filepath);
	gint64 file_read_finished = g_get_monotonic_time();
	char param_string[1000];
	print_header_data_to_string(load_results.header, param_string, sizeof(param_string), get_settings_datetime_type());
	printf("%s\n", param_string);
	if(load_results.departures == NULL) return;
	
	if(pa_analysis_params.num_deps > 0 && pa_analysis_params.file_type > 2) {
		if(pa_analysis_params.calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) free(pa_analysis_params.calc_data.seq_info.to_target.flyby_bodies);
		if(pa_analysis_params.calc_data.seq_info.spec_seq.type == ITIN_SEQ_INFO_SPEC_SEQ) free(pa_analysis_params.calc_data.seq_info.spec_seq.bodies);
	}
	pa_analysis_params = load_results.header;
	apply_pa_search_options(pa_analysis_params.calc_data.search_options, load_results.header.system);
	enum LastTransferType loaded_type = (enum LastTransferType)load_results.header.calc_data.dv_filter.last_transfer_type;
	pa_has_both_modes = load_results.header.file_type == 7 || load_results.header.file_type == 8;
	if(pa_has_both_modes) {
		pa_dual_flyby_departures = load_results.departures;
		pa_dual_flyby_num_deps = load_results.header.num_deps;
		pa_dual_capture_departures = load_results.capture_departures;
		pa_dual_capture_num_deps = load_results.capture_num_deps;
		pa_departures = pa_dual_flyby_departures;
		pa_num_deps = pa_dual_flyby_num_deps;
		pa_set_last_transfer_type(TF_FLYBY, 0);
	} else {
		pa_departures = load_results.departures;
		pa_num_deps = load_results.header.num_deps;
		int has_calculation_mode = load_results.header.file_type > 2 &&
		loaded_type >= TF_FLYBY && loaded_type <= TF_CIRC;
		pa_set_last_transfer_type(has_calculation_mode ? loaded_type : TF_FLYBY, has_calculation_mode);
	}
	pa_system = load_results.header.system;
	update_camera_to_celestial_system(pa_itin_preview_camera, pa_system, deg2rad(90), 0);
	pa_itin_preview_camera->min_pos_dist = 1e6;
	if(body_show_status_pa != NULL) free(body_show_status_pa);
	body_show_status_pa = (int*) calloc(pa_system->num_bodies, sizeof(int));
	analyze_departure_itins();
	gint64 analysis_finished = g_get_monotonic_time();

	update_pa_porkchop_diagram();
	update_pa_itinerary_preview();
	gint64 display_setup_finished = g_get_monotonic_time();
	printf("Itinerary load timing: file decode %.3f s, analyzer %.3f s, initial display setup %.3f s\n",
			(file_read_finished - load_started) / 1e6,
			(analysis_finished - file_read_finished) / 1e6,
			(display_setup_finished - analysis_finished) / 1e6);
}

G_MODULE_EXPORT void on_save_best_itinerary(GtkWidget* widget, gpointer data) {
	struct ItinStep *first = get_first(curr_transfer_pa);
	if(first == NULL) return;

	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itin", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	ItinSearchOptions options = pa_analysis_params.calc_data.search_options;
	if(options.known) options.last_transfer_type = pa_last_transfer_type;
	store_single_itinerary_in_bfile_with_options(first, pa_system, filepath, options);
}

G_MODULE_EXPORT void show_pa_analysis_parameters() {
	if(pa_analysis_params.num_deps == 0) return;
	char param_string[1000];
	print_header_data_to_string(pa_analysis_params, param_string, sizeof(param_string), get_settings_datetime_type());
	show_msg_window(param_string);
}
