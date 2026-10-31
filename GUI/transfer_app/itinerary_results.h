#ifndef KMAT_ITINERARY_RESULTS_H
#define KMAT_ITINERARY_RESULTS_H

#include <gtk/gtk.h>
#include "orbit_calculator/transfer_calc.h"

/* A finished search's results, kept until they are saved or shown in the Porkchop Analyzer. */
typedef struct PendingItineraries {
	Itin_Calc_Results results;
	Itin_Calc_Data calc_data;
	CelestSystem *system;
} PendingItineraries;

// Takes over the results and the calculation's body list; returns 0 (and frees them) when there is nothing to show.
int pending_itineraries_keep(PendingItineraries *pending, Itin_Calc_Results results, Itin_Calc_Data calc_data,
	CelestSystem *system);
int pending_itineraries_available(const PendingItineraries *pending);
void pending_itineraries_free(PendingItineraries *pending);
void pending_itineraries_save(PendingItineraries *pending);
// Hands the results to the Porkchop Analyzer and switches to it; nothing is left pending afterwards.
void pending_itineraries_show_in_porkchop_analyzer(PendingItineraries *pending);
// Enabled while results are pending.
void pending_itineraries_update_buttons(const PendingItineraries *pending, GtkWidget *show_button, GtkWidget *save_button);

#endif //KMAT_ITINERARY_RESULTS_H
