#include "porkchop_analyzer_internal.h"
#include "tools/file_io.h"
#include "orbit_calculator/flyby_policy.h"
#include "tools/starting_orbit.h"
#include <math.h>
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
	pa_has_both_modes = 1;
	pa_dual_flyby_departures = load_results.departures;
	pa_dual_flyby_num_deps = load_results.header.num_deps;
	pa_dual_capture_departures = load_results.capture_departures;
	pa_dual_capture_num_deps = load_results.capture_num_deps;
	pa_departures = pa_dual_flyby_departures;
	pa_num_deps = pa_dual_flyby_num_deps;
	pa_set_last_transfer_type(TF_FLYBY, 0);
	pa_system = load_results.header.system;
	if(pa_analysis_params.calc_data.search_options.known && pa_num_deps > 0 &&
		pa_departures != NULL && pa_departures[0] != NULL) {
		struct ItinStep *arrival = get_last(pa_departures[0]);
		if(arrival != NULL && arrival->body != NULL) {
			double saved_arrival_periapsis = pa_analysis_params.calc_data.dv_filter.arr_periapsis;
			double arrival_periapsis_altitude = saved_arrival_periapsis - arrival->body->atmo_alt;
			if(isfinite(arrival_periapsis_altitude) && arrival_periapsis_altitude >= 0)
				pa_arr_periapsis = arrival_periapsis_altitude;
		}
	}
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
	struct ItinStep *arrival = get_last(first);
	if(first->body == NULL || arrival == NULL || arrival->body == NULL) return;

	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itin", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	ItinSearchOptions options = pa_analysis_params.calc_data.search_options;
	if(options.known) options.last_transfer_type = pa_last_transfer_type;
	store_single_itinerary_in_bfile_with_options(first, pa_system, filepath, options,
		first->body->atmo_alt + pa_dep_periapsis,
		arrival->body->atmo_alt + pa_arr_periapsis);
}

G_MODULE_EXPORT void show_pa_analysis_parameters() {
	if(pa_analysis_params.num_deps == 0) return;
	char param_string[1000];
	print_header_data_to_string(pa_analysis_params, param_string, sizeof(param_string), get_settings_datetime_type());
	show_msg_window(param_string);
}
