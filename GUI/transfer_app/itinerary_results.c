#include "itinerary_results.h"
#include "porkchop_analyzer.h"
#include "gui/gui_manager.h"
#include "tools/file_io.h"

#include <stdlib.h>

static void free_calc_data_bodies(Itin_Calc_Data *calc_data) {
	if(calc_data->seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
		free(calc_data->seq_info.to_target.flyby_bodies);
		calc_data->seq_info.to_target.flyby_bodies = NULL;
	} else {
		free(calc_data->seq_info.spec_seq.bodies);
		calc_data->seq_info.spec_seq.bodies = NULL;
	}
}

static void free_results(Itin_Calc_Results *results) {
	for(int i = 0; i < results->num_deps; i++) free_itinerary(results->departures[i]);
	free(results->departures);
	for(int i = 0; i < results->capture_num_deps; i++) free_itinerary(results->capture_departures[i]);
	free(results->capture_departures);
	*results = (Itin_Calc_Results){0};
}

int pending_itineraries_available(const PendingItineraries *pending) {
	return pending->system != NULL;
}

void pending_itineraries_free(PendingItineraries *pending) {
	if(!pending_itineraries_available(pending)) return;
	free_results(&pending->results);
	free_calc_data_bodies(&pending->calc_data);
	pending->system = NULL;
}

int pending_itineraries_keep(PendingItineraries *pending, Itin_Calc_Results results, Itin_Calc_Data calc_data,
		CelestSystem *system) {
	pending_itineraries_free(pending);
	pending->results = results;
	pending->calc_data = calc_data;
	pending->system = system;
	// The analyzer expects both Fly-By and Capture results.
	if(results.capture_calculation_failed || (results.num_itins == 0 && results.capture_num_itins == 0)) {
		pending_itineraries_free(pending);
		return 0;
	}
	return 1;
}

void pending_itineraries_save(PendingItineraries *pending) {
	if(!pending_itineraries_available(pending)) return;
	char filepath[255];
	if(!get_path_from_file_chooser(filepath, ".itins", GTK_FILE_CHOOSER_ACTION_SAVE, "")) return;
	store_itineraries_in_bfile_both_modes(pending->results, pending->calc_data, pending->system, filepath);
}

void pending_itineraries_show_in_porkchop_analyzer(PendingItineraries *pending) {
	if(!pending_itineraries_available(pending)) return;
	pa_show_itineraries(prepare_itineraries_for_analysis(pending->results, pending->calc_data, pending->system));
	// The analyzer now owns the routes and its own copy of the body list.
	free_calc_data_bodies(&pending->calc_data);
	pending->results = (Itin_Calc_Results){0};
	pending->system = NULL;
	pa_show_page();
}

void pending_itineraries_update_buttons(const PendingItineraries *pending, GtkWidget *show_button, GtkWidget *save_button) {
	gboolean available = pending_itineraries_available(pending);
	if(show_button != NULL) gtk_widget_set_sensitive(show_button, available);
	if(save_button != NULL) gtk_widget_set_sensitive(save_button, available);
}
