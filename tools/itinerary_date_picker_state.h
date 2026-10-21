#ifndef KMAT_ITINERARY_DATE_PICKER_STATE_H
#define KMAT_ITINERARY_DATE_PICKER_STATE_H

typedef void (*ItineraryDatePickerDestroy)(void *dialog);

typedef struct {
	void *dialog;
	ItineraryDatePickerDestroy destroy_dialog;
} ItineraryDatePickerState;

void itinerary_date_picker_track(
	ItineraryDatePickerState *state,
	void *dialog,
	ItineraryDatePickerDestroy destroy_dialog);
void itinerary_date_picker_closed(ItineraryDatePickerState *state, void *dialog);
void itinerary_date_picker_close(ItineraryDatePickerState *state);

#endif
