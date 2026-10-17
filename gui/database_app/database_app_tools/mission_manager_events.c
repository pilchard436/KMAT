#include "mission_manager_internal.h"
#include "gui/css_loader.h"

void update_mman_event_box() {
	int num_cols = 9;

	// Remove grid if exists
	if (grid_mman_events != NULL && GTK_WIDGET(vp_mman_events) == gtk_widget_get_parent(grid_mman_events)) {
		gtk_container_remove(GTK_CONTAINER(vp_mman_events), grid_mman_events);
	}

	grid_mman_events = gtk_grid_new();
	GtkWidget *separator;


	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_grid_attach(GTK_GRID(grid_mman_events), separator, 0, 0, num_cols*2+1, 1);
	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_grid_attach(GTK_GRID(grid_mman_events), separator, 0, 1, 1, 1);

	for (int col = 0; col < num_cols; col++) {
		char label_text[20];
		int req_width = -1;
		switch(col) {
			case 0: sprintf(label_text, "Type"); req_width = 75; break;
			case 1: sprintf(label_text, "Epoch / T+"); req_width = 260; break;
			case 7: sprintf(label_text, "Event"); req_width = 535; break;
			default: sprintf(label_text, ""); req_width = 50; break;
		}
		// Create a GtkLabel
		GtkWidget *label = gtk_label_new(label_text);
		// width request
		gtk_widget_set_size_request(GTK_WIDGET(label), req_width, -1);
		// set css class
		set_css_class_for_widget(GTK_WIDGET(label), "missiondb-header");

		// Set the label in the grid at the specified row and column
		if(col != 1)
			gtk_grid_attach(GTK_GRID(grid_mman_events), label, col * 2 + 1, 1, 1, 1);
		else {
			gtk_grid_attach(GTK_GRID(grid_mman_events), label, col * 2 + 1, 1, 10, 1);
			col = num_cols-3;
		}

		// Create a horizontal separator line (optional)
		separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
		gtk_grid_attach(GTK_GRID(grid_mman_events), separator, col * 2 + 2, 1, 1, 1);
	}

	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_grid_attach(GTK_GRID(grid_mman_events), separator, 0, 2, num_cols*2+1, 1);

	// Create labels and buttons and add them to the grid
	for (int i = 0; i < num_events; i++) {
		int row = i*2+3;
		struct Datetime date = {};
		if(event_list[i].event_type != TIMESINCE_EVENT) {
			date = convert_JD_date(events[i].epoch, DATE_ISO);
		} else {
			date = get_date_difference_from_epochs(event_list[initial_event_id].event->epoch, event_list[i].event->epoch, DATE_ISO);
		}


		for (int j = 0; j < num_cols; j++) {
			int col = j*2+1;
			char widget_text[500];
			GtkWidget *widget;
			switch(j) {
				case 0: // Type Combo box
					widget = gtk_combo_box_text_new();
					// Add options to the combo box
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), NULL, "INIT");
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), NULL, "EPOCH");
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), NULL, "T+");
					gtk_combo_box_set_active(GTK_COMBO_BOX(widget), event_list[i].event_type);
					g_signal_connect(widget, "changed", G_CALLBACK(on_mman_change_event_time_type), &(event_list[i]));
					event_list[i].cb_type = widget;
					break;
				case 1: // Year Entry box
					if(event_list[i].event_type == TIMESINCE_EVENT) {
						event_list[i].tf_year = NULL;
						event_list[i].tf_month = NULL;
						j+=2; goto case_3;
					}
					widget = gtk_entry_new();
					sprintf(widget_text, "%d", date.y);
					gtk_entry_set_text(GTK_ENTRY(widget), widget_text);
					gtk_entry_set_width_chars(GTK_ENTRY(widget), 4);
					event_list[i].tf_year = widget;
					break;
				case 2: // Month Entry box
					widget = gtk_entry_new();
					sprintf(widget_text, "%02d", date.m);
					gtk_entry_set_text(GTK_ENTRY(widget), widget_text);
					gtk_entry_set_width_chars(GTK_ENTRY(widget), 2);
					event_list[i].tf_month = widget;
					break;
				case 3: // Day Entry box
				case_3:
					widget = gtk_entry_new();
					if(event_list[i].event_type != TIMESINCE_EVENT) {
						sprintf(widget_text, "%02d", date.d);
						gtk_entry_set_width_chars(GTK_ENTRY(widget), 2);
					} else {
						sprintf(widget_text, "%d", date.d);
						gtk_entry_set_width_chars(GTK_ENTRY(widget), 5);
					}
					gtk_entry_set_text(GTK_ENTRY(widget), widget_text);
					event_list[i].tf_day = widget;
					break;
				case 4: // Hour Entry box
					widget = gtk_entry_new();
					sprintf(widget_text, "%02d", date.h);
					gtk_entry_set_text(GTK_ENTRY(widget), widget_text);
					gtk_entry_set_width_chars(GTK_ENTRY(widget), 3);
					event_list[i].tf_hour = widget;
					break;
				case 5: // Minute Entry box
					widget = gtk_entry_new();
					sprintf(widget_text, "%02d", date.min);
					gtk_entry_set_text(GTK_ENTRY(widget), widget_text);
					gtk_entry_set_width_chars(GTK_ENTRY(widget), 3);
					event_list[i].tf_min = widget;
					break;
				case 6: // Second Entry box
					widget = gtk_entry_new();
					sprintf(widget_text, "%02.0f", date.s);
					gtk_entry_set_text(GTK_ENTRY(widget), widget_text);
					gtk_entry_set_width_chars(GTK_ENTRY(widget), 3);
					event_list[i].tf_sec = widget;
					break;
				case 7:
					widget = gtk_entry_new();
					sprintf(widget_text, "%s", events[i].event);
					gtk_entry_set_text(GTK_ENTRY(widget), widget_text);
					event_list[i].tf_event = widget;
					break;
				case 8:
					widget = gtk_button_new_with_label("-");
					g_signal_connect(widget, "clicked", G_CALLBACK(on_mman_remove_event), &(event_list[i]));
					break;
				default:
					sprintf(widget_text, "");
					widget = gtk_label_new(widget_text);
					break;
			}

			if(j > 0 && j < 7)
				gtk_entry_set_alignment(GTK_ENTRY(widget), (gfloat) 1.0);

			if(j>0) {
				if(events[i].id == 0) set_css_class_for_widget(widget, "mman-added");
				if(events[i].id <  0) set_css_class_for_widget(widget, "mman-removed");
				if(events[i].id >  0) set_css_class_for_widget(widget, "mman-norm");
			}


			// Set the label in the grid at the specified row and column
			if(j == 3 && event_list[i].event_type == TIMESINCE_EVENT) {
				gtk_grid_attach(GTK_GRID(grid_mman_events), widget, col, row, 5, 1);
				col = j*2+1;
			} else gtk_grid_attach(GTK_GRID(grid_mman_events), widget, col, row, 1, 1);


			// Create a horizontal separator line (optional)
			if(j == 0 || j >= 6) {
				separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
				gtk_grid_attach(GTK_GRID(grid_mman_events), separator, col + 1, row, 1, 1);
			} else {
				switch(j) {
					case 1: // y-m
					case 2: // m-d
						widget = gtk_label_new("-"); break;
					case 3: // dTh
						widget = gtk_label_new("T"); break;
					case 4: // h:min
					case 5: // min:sec
						widget = gtk_label_new(":"); break;
					default:
						widget = gtk_label_new("");
						break;
				}
				if(events[i].id == 0) set_css_class_for_widget(widget, "mman-added");
				if(events[i].id <  0) set_css_class_for_widget(widget, "mman-removed");
				if(events[i].id >  0) set_css_class_for_widget(widget, "mman-norm");
				gtk_grid_attach(GTK_GRID(grid_mman_events), widget, col + 1, row, 1, 1);
			}
		}
		separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
		gtk_grid_attach(GTK_GRID(grid_mman_events), separator, 0, row+1, num_cols*2+1, 1);
	}
	int row = num_events*2+3;

	GtkWidget *add_event_button = gtk_button_new_with_label("+");
	g_signal_connect(add_event_button, "clicked", G_CALLBACK(on_mman_add_event), NULL);
	// Set the label in the grid at the specified row and column
	gtk_grid_attach(GTK_GRID(grid_mman_events), add_event_button, 0, row, num_cols*2+1, 1);


	gtk_container_add (GTK_CONTAINER (vp_mman_events),
					   grid_mman_events);
	gtk_widget_show_all(GTK_WIDGET(vp_mman_events));
}

void update_mman_event_list() {
	for(int i = 0; i < num_events; i++) {
		struct MissionEvent_DB *p_event = event_list[i].event;
		p_event->epoch = get_event_epoch(&(event_list[i]));
		sprintf(p_event->event, "%s", gtk_entry_get_text(GTK_ENTRY(event_list[i].tf_event)));
	}
}

G_MODULE_EXPORT void on_mman_add_event(GtkWidget *button, gpointer data) {
	update_mman_event_list();
	num_events++;

	// increase arrays by 1
	struct MissionEventList *new_event_list = realloc(event_list, num_events*sizeof(struct MissionEventList));
	if(new_event_list == NULL) { printf("Memory reallocation failed!\n"); exit(1); }
	event_list = new_event_list;
	struct MissionEvent_DB *new_events = realloc(events, num_events*sizeof(struct MissionEvent_DB));
	if(new_events == NULL) { printf("Memory reallocation failed!\n"); exit(1); }
	events = new_events;
	// Initialize the new objective
	struct Datetime date = {1950, 01, 01};
	events[num_events-1].id = 0;
	if(initial_event_id < 0)
		events[num_events-1].epoch = convert_date_JD(date);
	else
		events[num_events-1].epoch = events[initial_event_id].epoch;
	events[num_events-1].mission_id = mission.id;
	sprintf(events[num_events-1].event, "<EVENT>");
	event_list[num_events-1].event_type = EPOCH_EVENT;
	update_lists_with_id_and_references();
	update_mman_event_box();
}

G_MODULE_EXPORT void on_mman_remove_event(GtkWidget *button, gpointer data) {
	update_mman_event_list();
	struct MissionEventList *event = (struct MissionEventList *) data;  // Cast data back to event struct
	if(event->event->id != 0) event->event->id *= -1;
	else {
		if(event->event_type == INITIAL_EVENT) {
			initial_event_id = -1;
			for(int i = 0; i < num_events; i++) event_list[i].event_type = EPOCH_EVENT;
		}
		// remove previously added objective from list and objectives
		if(event->list_id < num_events-1){
			for(int i = event->list_id; i < num_events-1; i++) {
				events[i].id = events[i+1].id;
				events[i].epoch = events[i+1].epoch;
				sprintf(events[i].event, "%s", events[i+1].event);
				event_list[i].event_type= event_list[i+1].event_type;
				event_list[i].cb_type 	= event_list[i+1].cb_type;
				event_list[i].tf_year 	= event_list[i+1].tf_year;
				event_list[i].tf_month 	= event_list[i+1].tf_month;
				event_list[i].tf_day 	= event_list[i+1].tf_day;
				event_list[i].tf_hour 	= event_list[i+1].tf_hour;
				event_list[i].tf_min 	= event_list[i+1].tf_min;
				event_list[i].tf_sec 	= event_list[i+1].tf_sec;
				event_list[i].tf_event 	= event_list[i+1].tf_event;
			}
		};
		num_events--;
	}
	update_lists_with_id_and_references();
	update_mman_event_box();
}

G_MODULE_EXPORT void on_mman_change_event_time_type(GtkWidget *combo_box, gpointer data) {
	update_mman_event_list();
	struct MissionEventList *event = (struct MissionEventList *) data;

	int new_event_type = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_box));

	switch(new_event_type) {
		case INITIAL_EVENT:
			if(initial_event_id < 0) {
				initial_event_id = event->list_id;
				event->event_type = INITIAL_EVENT;
			}
			break;
		case EPOCH_EVENT:
			if(event->event_type == INITIAL_EVENT) {
				initial_event_id = -1;
				for(int i = 0; i < num_events; i++) event_list[i].event_type = EPOCH_EVENT;
			}
			event->event_type = EPOCH_EVENT;
			break;
		case TIMESINCE_EVENT:
			if(initial_event_id < 0 || initial_event_id == event->list_id) break;
			event->event_type = TIMESINCE_EVENT;
			break;
		default: break;
	}

	update_mman_event_box();
}

double get_event_epoch(struct MissionEventList *event) {
	char *endptr;
	if(event->event_type != TIMESINCE_EVENT) {
		struct Datetime date = {
				.y   = (int) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_year)), &endptr, 10),
				.m   = (int) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_month)), &endptr, 10),
				.d   = (int) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_day)), &endptr, 10),
				.h   = (int) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_hour)), &endptr, 10),
				.min = (int) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_min)), &endptr, 10),
				.s   = (double) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_sec)), &endptr, 10)
		};
		return convert_date_JD(date);
	} else {
		struct Datetime date_diff = {
				.d   = (int) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_day)), &endptr, 10),
				.h   = (int) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_hour)), &endptr, 10),
				.min = (int) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_min)), &endptr, 10),
				.s   = (double) strtol(gtk_entry_get_text(GTK_ENTRY(event->tf_sec)), &endptr, 10)
		};
		double initial_epoch = event_list[initial_event_id].event->epoch;
		double epoch_diff = (date_diff.d) + ((double)date_diff.h/24) + ((double)date_diff.min/(24*60)) + ((double)date_diff.s/(24*60*60));
		return initial_epoch + epoch_diff;
	}
}
