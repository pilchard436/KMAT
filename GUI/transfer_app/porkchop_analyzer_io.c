#include "porkchop_analyzer_internal.h"
#include "tools/file_io.h"
#include "orbit_calculator/flyby_policy.h"
#include "tools/starting_orbit.h"
#include "transfer_planner.h"
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

static int pa_capture_num_nodes, pa_capture_num_itins;

void pa_show_itineraries(struct ItinsLoadFileResults load_results) {
	gint64 analysis_started = g_get_monotonic_time();
	reset_pa_porkchop_view();
	free_all_porkchop_analyzer_itins();
	pa_set_moon_assist_dv_toggle_available(0);
	pa_set_last_transfer_type(TF_FLYBY, 0);

	pa_analysis_params = load_results.header;
	apply_pa_search_options(pa_analysis_params.calc_data.search_options, load_results.header.system);
	pa_has_both_modes = 1;
	pa_dual_flyby_departures = load_results.departures;
	pa_dual_flyby_num_deps = load_results.header.num_deps;
	pa_dual_capture_departures = load_results.capture_departures;
	pa_dual_capture_num_deps = load_results.capture_num_deps;
	pa_capture_num_nodes = load_results.capture_num_nodes;
	pa_capture_num_itins = load_results.capture_num_itins;
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
	int moon_assist_dvs_available = pa_num_itins > 0 && pa_porkchop_points != NULL &&
			pa_analysis_params.calc_data.search_options.known &&
			pa_analysis_params.calc_data.search_options.moon_assist_arrival;
	for(int i = 0; moon_assist_dvs_available && i < pa_num_itins; i++) {
		struct ItinStep *arrival = pa_porkchop_points[i].data.arrival;
		if(arrival == NULL || !arrival->arrival_dv_cache.moon_assist_dvs_valid)
			moon_assist_dvs_available = 0;
	}
	pa_set_moon_assist_dv_toggle_available(moon_assist_dvs_available);
	gint64 analysis_finished = g_get_monotonic_time();

	update_pa_porkchop_diagram();
	update_pa_itinerary_preview();
	gint64 display_setup_finished = g_get_monotonic_time();
	printf("Itinerary display timing: analyzer %.3f s, initial display setup %.3f s\n",
			(analysis_finished - analysis_started) / 1e6,
			(display_setup_finished - analysis_finished) / 1e6);
}

G_MODULE_EXPORT void on_load_itineraries(GtkWidget* widget, gpointer data) {
	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itins", GTK_FILE_CHOOSER_ACTION_OPEN, "")) return;

	// The current results are released first so that two large sets are never held at once.
	reset_pa_porkchop_view();
	free_all_porkchop_analyzer_itins();
	gint64 load_started = g_get_monotonic_time();
	struct ItinsLoadFileResults load_results = load_itineraries_from_bfile(filepath);
	char param_string[1000];
	print_header_data_to_string(load_results.header, param_string, sizeof(param_string), get_settings_datetime_type());
	printf("%s\n", param_string);
	printf("Itinerary file decode: %.3f s\n", (g_get_monotonic_time() - load_started) / 1e6);
	if(load_results.departures == NULL) return;
	pa_show_itineraries(load_results);
}

G_MODULE_EXPORT void on_pa_save_itineraries(GtkWidget* widget, gpointer data) {
	if(!pa_has_both_modes || pa_system == NULL) return;
	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itins", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	Itin_Calc_Results results = {
		.departures = pa_dual_flyby_departures,
		.num_deps = pa_dual_flyby_num_deps,
		.num_nodes = pa_analysis_params.num_nodes,
		.num_itins = pa_analysis_params.num_itins,
		.capture_departures = pa_dual_capture_departures,
		.capture_num_deps = pa_dual_capture_num_deps,
		.capture_num_nodes = pa_capture_num_nodes,
		.capture_num_itins = pa_capture_num_itins
	};
	store_itineraries_in_bfile_both_modes(results, pa_analysis_params.calc_data, pa_system, filepath);
}

// The selected route with the search options and periapses it was calculated with; returns NULL without one.
static struct ItinStep *get_selected_route_pa(ItinSearchOptions *options, double *dep_periapsis, double *arr_periapsis) {
	struct ItinStep *first = get_first(curr_transfer_pa);
	if(first == NULL) return NULL;
	struct ItinStep *arrival = get_last(first);
	if(first->body == NULL || arrival == NULL || arrival->body == NULL) return NULL;
	*options = pa_analysis_params.calc_data.search_options;
	if(options->known) options->last_transfer_type = pa_last_transfer_type;
	*dep_periapsis = first->body->atmo_alt + pa_dep_periapsis;
	*arr_periapsis = arrival->body->atmo_alt + pa_arr_periapsis;
	return first;
}

G_MODULE_EXPORT void on_save_selected_itinerary(GtkWidget* widget, gpointer data) {
	ItinSearchOptions options;
	double dep_periapsis, arr_periapsis;
	struct ItinStep *first = get_selected_route_pa(&options, &dep_periapsis, &arr_periapsis);
	if(first == NULL) return;

	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itin", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	store_single_itinerary_in_bfile_with_options(first, pa_system, filepath, options, dep_periapsis, arr_periapsis);
}

G_MODULE_EXPORT void on_pa_open_in_transfer_planner(GtkWidget* widget, gpointer data) {
	ItinSearchOptions options;
	double dep_periapsis, arr_periapsis;
	struct ItinStep *first = get_selected_route_pa(&options, &dep_periapsis, &arr_periapsis);
	if(first == NULL) return;
	tp_open_itinerary(first, pa_system, options, dep_periapsis, arr_periapsis);
}
