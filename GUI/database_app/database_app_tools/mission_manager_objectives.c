#include "mission_manager_internal.h"
#include "gui/css_loader.h"

void update_mman_objective_box() {
	int num_cols = 4;

	// Remove grid if exists
	if (grid_mman_objectives != NULL && GTK_WIDGET(vp_mman_objectives) == gtk_widget_get_parent(grid_mman_objectives)) {
		gtk_container_remove(GTK_CONTAINER(vp_mman_objectives), grid_mman_objectives);
	}

	grid_mman_objectives = gtk_grid_new();
	GtkWidget *separator;


	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_grid_attach(GTK_GRID(grid_mman_objectives), separator, 0, 0, num_cols*2+1, 1);
	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_grid_attach(GTK_GRID(grid_mman_objectives), separator, 0, 1, 1, 1);

	for (int col = 0; col < num_cols; col++) {
		char label_text[20];
		int req_width = -1;
		switch(col) {
			case 0: sprintf(label_text, "Rank"); req_width = 50; break;
			case 1: sprintf(label_text, "Status"); req_width = 100; break;
			case 2: sprintf(label_text, "Objective"); req_width = 745; break;
			default:sprintf(label_text, ""); req_width = 50; break;
		}

		// Create a GtkLabel
		GtkWidget *label = gtk_label_new(label_text);
		// width request
		gtk_widget_set_size_request(GTK_WIDGET(label), req_width, -1);
		// set css class
		set_css_class_for_widget(GTK_WIDGET(label), "missiondb-header");

		// Set the label in the grid at the specified row and column
		gtk_grid_attach(GTK_GRID(grid_mman_objectives), label, col*2+1, 1, 1, 1);

		// Create a horizontal separator line (optional)
		separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
		gtk_grid_attach(GTK_GRID(grid_mman_objectives), separator, col*2+2, 1, 1, 1);
	}

	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_grid_attach(GTK_GRID(grid_mman_objectives), separator, 0, 2, num_cols*2+1, 1);

	// Create labels and buttons and add them to the grid
	for (int i = 0; i < num_objectives; i++) {
		int row = i*2+3;

		for (int j = 0; j < num_cols; j++) {
			int col = j*2+1;
			char widget_text[500];
			GtkWidget *widget;
			switch(j) {
				case 0:
					// Create a combo box with text entries
					widget = gtk_combo_box_text_new();
					// Add options to the combo box
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), NULL, "PRIM");
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), NULL, "SEC");
					gtk_combo_box_set_active(GTK_COMBO_BOX(widget), objectives[i].rank == OBJ_PRIMARY ? 0 : 1);
					obj_list[i].cb_rank = widget;
					break;
				case 1:
					// Create a combo box with text entries
					widget = gtk_combo_box_text_new();
					// Add options to the combo box
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), NULL, "TBD");
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), NULL, "SUCCESS");
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), NULL, "FAIL");
					gtk_combo_box_set_active(GTK_COMBO_BOX(widget), objectives[i].status == OBJ_TBD ? 0 : objectives[i].status == OBJ_SUCCESS ? 1:2);
					obj_list[i].cb_status = widget;
					break;
				case 2:
					sprintf(widget_text, "%s", objectives[i].objective);
					widget = gtk_entry_new();
					gtk_entry_set_text(GTK_ENTRY(widget), objectives[i].objective);
					obj_list[i].tf_objective = widget;
					break;
				case 3:
					widget = gtk_button_new_with_label("-");
					g_signal_connect(widget, "clicked", G_CALLBACK(on_mman_remove_objective), &(obj_list[i]));
					break;
				default:
					sprintf(widget_text, "");
					widget = gtk_label_new(widget_text);
					break;
			}

			if(j>1) {
				if(objectives[i].id == 0) set_css_class_for_widget(widget, "mman-added");
				if(objectives[i].id <  0) set_css_class_for_widget(widget, "mman-removed");
				if(objectives[i].id >  0) set_css_class_for_widget(widget, "mman-norm");
			}


			// Set the label in the grid at the specified row and column
			gtk_grid_attach(GTK_GRID(grid_mman_objectives), widget, col, row, 1, 1);

			// Create a horizontal separator line (optional)
			separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
			gtk_grid_attach(GTK_GRID(grid_mman_objectives), separator, col+1, row, 1, 1);
		}
		separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
		gtk_grid_attach(GTK_GRID(grid_mman_objectives), separator, 0, row+1, num_cols*2+1, 1);
	}
	int row = num_objectives*2+3;

	GtkWidget *add_obj_button = gtk_button_new_with_label("+");
	g_signal_connect(add_obj_button, "clicked", G_CALLBACK(on_mman_add_objective), NULL);
	// Set the label in the grid at the specified row and column
	gtk_grid_attach(GTK_GRID(grid_mman_objectives), add_obj_button, 0, row, num_cols*2+1, 1);


	gtk_container_add (GTK_CONTAINER (vp_mman_objectives),
					   grid_mman_objectives);
	gtk_widget_show_all(GTK_WIDGET(vp_mman_objectives));
}

void update_mman_objectives_list() {
	for(int i = 0; i < num_objectives; i++) {
		struct MissionObjective_DB *p_obj = obj_list[i].objective;
		p_obj->rank = gtk_combo_box_get_active(GTK_COMBO_BOX(obj_list[i].cb_rank));
		p_obj->status = gtk_combo_box_get_active(GTK_COMBO_BOX(obj_list[i].cb_status));
		sprintf(p_obj->objective, "%s", gtk_entry_get_text(GTK_ENTRY(obj_list[i].tf_objective)));
	}
}

G_MODULE_EXPORT void on_mman_add_objective(GtkWidget *button, gpointer data) {
	update_mman_objectives_list();
	num_objectives++;

	// increase arrays by 1
	struct MissionObjectiveList *new_obj_list = realloc(obj_list, num_objectives*sizeof(struct MissionObjectiveList));
	if(new_obj_list == NULL) { printf("Memory reallocation failed!\n"); exit(1); }
	obj_list = new_obj_list;
	struct MissionObjective_DB *new_obj = realloc(objectives, num_objectives*sizeof(struct MissionObjective_DB));
	if(new_obj == NULL) { printf("Memory reallocation failed!\n"); exit(1); }
	objectives = new_obj;

	// Initialize the new objective
	objectives[num_objectives-1].id = 0;
	objectives[num_objectives-1].status = OBJ_TBD;
	objectives[num_objectives-1].rank = OBJ_PRIMARY;
	objectives[num_objectives-1].mission_id = mission.id;
	sprintf(objectives[num_objectives-1].objective, "<OBJECTIVE>");
	update_lists_with_id_and_references();
	update_mman_objective_box();
}

G_MODULE_EXPORT void on_mman_remove_objective(GtkWidget *button, gpointer data) {
	update_mman_objectives_list();
	struct MissionObjectiveList *objective = (struct MissionObjectiveList *) data;  // Cast data back to int
	if(objective->objective->id != 0) objective->objective->id *= -1;
	else {
		// remove previously added objective from list and objectives
		if(objective->list_id < num_objectives-1){
			for(int i = objective->list_id; i < num_objectives-1; i++) {
				objectives[i].status = objectives[i+1].status;
				objectives[i].id = objectives[i+1].id;
				objectives[i].rank = objectives[i+1].rank;
				objectives[i].mission_id = objectives[i+1].mission_id;
				sprintf(objectives[i].objective, "%s", objectives[i+1].objective);
				obj_list[i].objective = obj_list[i+1].objective;
				obj_list[i].cb_rank = obj_list[i+1].cb_rank;
				obj_list[i].cb_status = obj_list[i+1].cb_status;
				obj_list[i].tf_objective = obj_list[i+1].tf_objective;
			}
		};
		num_objectives--;
	}
	update_lists_with_id_and_references();
	update_mman_objective_box();
}
