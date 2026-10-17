#include "porkchop_analyzer_internal.h"
#include "tools/file_io.h"
#include <stdlib.h>
#include <string.h>

G_MODULE_EXPORT void on_load_itineraries(GtkWidget* widget, gpointer data) {
	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itins", GTK_FILE_CHOOSER_ACTION_OPEN, "")) return;

	free_all_porkchop_analyzer_itins();
	struct ItinsLoadFileResults load_results = load_itineraries_from_bfile(filepath);
	char param_string[1000];
	print_header_data_to_string(load_results.header, param_string, sizeof(param_string), get_settings_datetime_type());
	printf("%s\n", param_string);
	if(load_results.departures == NULL) return;
	
	if(pa_analysis_params.num_deps > 0 && pa_analysis_params.file_type > 2) {
		if(pa_analysis_params.calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) free(pa_analysis_params.calc_data.seq_info.to_target.flyby_bodies);
		if(pa_analysis_params.calc_data.seq_info.spec_seq.type == ITIN_SEQ_INFO_SPEC_SEQ) free(pa_analysis_params.calc_data.seq_info.spec_seq.bodies);
	}
	pa_analysis_params = load_results.header;
	pa_num_deps = load_results.header.num_deps;
	pa_departures = load_results.departures;
	pa_system = load_results.header.system;
	update_camera_to_celestial_system(pa_itin_preview_camera, pa_system, deg2rad(90), 0);
	if(body_show_status_pa != NULL) free(body_show_status_pa);
	body_show_status_pa = (int*) calloc(pa_system->num_bodies, sizeof(int));
	analyze_departure_itins();

	update_pa_porkchop_diagram();
	update_pa_itinerary_preview();
}

G_MODULE_EXPORT void on_save_best_itinerary(GtkWidget* widget, gpointer data) {
	struct ItinStep *first = get_first(curr_transfer_pa);
	if(first == NULL) return;

	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itin", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	store_single_itinerary_in_bfile(first, pa_system, filepath);
}

G_MODULE_EXPORT void show_pa_analysis_parameters() {
	if(pa_analysis_params.num_deps == 0) return;
	char param_string[1000];
	print_header_data_to_string(pa_analysis_params, param_string, sizeof(param_string), get_settings_datetime_type());
	show_msg_window(param_string);
}
