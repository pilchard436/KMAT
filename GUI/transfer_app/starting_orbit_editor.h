#ifndef KSP_STARTING_ORBIT_EDITOR_H
#define KSP_STARTING_ORBIT_EDITOR_H

#include <gtk/gtk.h>
#include "orbitlib.h"

void init_starting_orbit_editor(GtkBuilder *builder);
void end_starting_orbit_editor(void);

/* Activates the custom starting orbit for a search from dep_body, or clears it when disabled.
 * Returns 0 with a message in `error` if it is enabled but cannot be used. */
int starting_orbit_editor_prepare_search(Body *dep_body, char *error, size_t error_size);
void starting_orbit_editor_finish_search(void);

G_MODULE_EXPORT void on_open_starting_orbit_editor(void);
G_MODULE_EXPORT void on_starting_orbit_reset(void);
G_MODULE_EXPORT void on_starting_orbit_changed(void);
G_MODULE_EXPORT gboolean on_starting_orbit_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data);

#endif
