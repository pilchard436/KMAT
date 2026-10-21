#include "tools/itinerary_date_picker_state.h"

void itinerary_date_picker_close(ItineraryDatePickerState *state) {
	if(state == 0 || state->dialog == 0) return;
	void *dialog = state->dialog;
	ItineraryDatePickerDestroy destroy_dialog = state->destroy_dialog;
	state->dialog = 0;
	state->destroy_dialog = 0;
	if(destroy_dialog != 0) destroy_dialog(dialog);
}

void itinerary_date_picker_track(
	ItineraryDatePickerState *state,
	void *dialog,
	ItineraryDatePickerDestroy destroy_dialog) {
	if(state == 0 || dialog == 0 || destroy_dialog == 0) return;
	if(state->dialog == dialog) return;
	itinerary_date_picker_close(state);
	state->dialog = dialog;
	state->destroy_dialog = destroy_dialog;
}

void itinerary_date_picker_closed(ItineraryDatePickerState *state, void *dialog) {
	if(state != 0 && state->dialog == dialog) {
		state->dialog = 0;
		state->destroy_dialog = 0;
	}
}
