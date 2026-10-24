#ifndef KMAT_DATE_PICKER_H
#define KMAT_DATE_PICKER_H

#include <gtk/gtk.h>

void gui_date_picker_open(GtkEntry *entry);
void gui_date_picker_close(void);
void gui_date_picker_on_entry_icon_release(
		GtkEntry *entry, GtkEntryIconPosition icon_pos, gpointer data);

#endif
