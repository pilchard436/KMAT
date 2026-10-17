#include "mission_manager_internal.h"
#include "gui/css_loader.h"

GtkWidget *grid_mman_objectives;
GtkWidget *grid_mman_events;

GObject *vp_mman_objectives;
GObject *vp_mman_events;

GObject *lb_mman_updatenew;
GObject *tf_mman_name;
GObject *cb_mman_program;
GObject *cb_mman_missionstatus;
GObject *cb_mman_launcher;
GObject *bt_mman_updatenew;
GObject *tf_mman_newprogram;
GObject *stack_newprogram;


struct MissionObjective_DB *objectives;
struct MissionObjectiveList *obj_list;
struct MissionEvent_DB *events;
struct MissionEventList *event_list;
int num_objectives, num_events, initial_event_id;

struct Mission_DB mission;


void init_mission_manager(GtkBuilder *builder) {
	vp_mman_objectives = gtk_builder_get_object(builder, "vp_mman_objectives");
	vp_mman_events = gtk_builder_get_object(builder, "vp_mman_events");
	lb_mman_updatenew = gtk_builder_get_object(builder, "lb_mman_updatenew");
	tf_mman_name = gtk_builder_get_object(builder, "tf_mman_name");
	cb_mman_program = gtk_builder_get_object(builder, "cb_mman_program");
	cb_mman_missionstatus = gtk_builder_get_object(builder, "cb_mman_missionstatus");
	cb_mman_launcher = gtk_builder_get_object(builder, "cb_mman_launcher");
	bt_mman_updatenew = gtk_builder_get_object(builder, "bt_mman_updatenew");
	tf_mman_newprogram = gtk_builder_get_object(builder, "tf_mman_newprogram");
	stack_newprogram = gtk_builder_get_object(builder, "stack_newprogram");

	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(cb_mman_program), renderer, TRUE);
	gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(cb_mman_program), renderer, "text", 0, NULL);

	renderer = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(cb_mman_launcher), renderer, TRUE);
	gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(cb_mman_launcher), renderer, "text", 0, NULL);
}


void switch_to_mission_manager(struct Mission_DB m) {
	mission = m;
	// Retrieve Objectives of mission
	num_objectives = db_get_objectives_from_mission_id(&objectives, mission.id);
	obj_list = malloc(num_objectives * sizeof(struct MissionObjectiveList));
	update_mission_launcher_dropdown(GTK_COMBO_BOX(cb_mman_launcher));
	update_program_dropdown(GTK_COMBO_BOX(cb_mman_program), 0);
	// Retrieve Events of Mission
	num_events = db_get_events_from_mission_id(&events, mission.id);
	event_list = malloc(num_events * sizeof(struct MissionEventList));
	update_mission_launcher_dropdown(GTK_COMBO_BOX(cb_mman_launcher));
	update_program_dropdown(GTK_COMBO_BOX(cb_mman_program), 0);

	update_lists_with_id_and_references();
	initial_event_id = -1;

	for(int i = 0; i < num_events; i++) event_list[i].event_type = EPOCH_EVENT;


	if(mission.id < 0) {
		gtk_label_set_label(GTK_LABEL(lb_mman_updatenew), "NEW MISSION:");
		gtk_entry_set_text(GTK_ENTRY(tf_mman_name), "");
		gtk_button_set_label(GTK_BUTTON(bt_mman_updatenew), "Add Mission");
		set_active_combobox_id(GTK_COMBO_BOX(cb_mman_program), -1);
		set_active_combobox_id(GTK_COMBO_BOX(cb_mman_launcher), -1);
		gtk_combo_box_set_active(GTK_COMBO_BOX(cb_mman_missionstatus), 0);
	} else {
		gtk_label_set_label(GTK_LABEL(lb_mman_updatenew), "EDIT MISSION:");
		gtk_entry_set_text(GTK_ENTRY(tf_mman_name), mission.name);
		gtk_button_set_label(GTK_BUTTON(bt_mman_updatenew), "Update Mission");
		set_active_combobox_id(GTK_COMBO_BOX(cb_mman_program), mission.program_id);
		set_active_combobox_id(GTK_COMBO_BOX(cb_mman_launcher), mission.launcher_id);
		gtk_combo_box_set_active(GTK_COMBO_BOX(cb_mman_missionstatus), mission.status);
	}


	update_mman_objective_box();
	update_mman_event_box();
}


G_MODULE_EXPORT void on_enter_new_program() {
	gtk_stack_set_visible_child_name(GTK_STACK(stack_newprogram), "page1");
}

G_MODULE_EXPORT void on_add_new_program() {
	char *program_name = (char*) gtk_entry_get_text(GTK_ENTRY(tf_mman_newprogram));
	if(program_name[0] != '\0') db_new_program(program_name, "");
	gtk_stack_set_visible_child_name(GTK_STACK(stack_newprogram), "page0");
	update_program_dropdown(GTK_COMBO_BOX(cb_mman_program), 0);
}

G_MODULE_EXPORT void on_mman_back() {
	reset_mission_manager();
	update_db_box();
	switch_to_mission_database_page();
}

G_MODULE_EXPORT void on_newupdate_mission() {
	char *mission_name = (char*) gtk_entry_get_text(GTK_ENTRY(tf_mman_name));
	int program_id = get_active_combobox_id(GTK_COMBO_BOX(cb_mman_program));
	int launcher_id = get_active_combobox_id(GTK_COMBO_BOX(cb_mman_launcher));
	int status = gtk_combo_box_get_active(GTK_COMBO_BOX(cb_mman_missionstatus));
	if(mission.id < 0 && mission_name[0] != '\0') {
		db_new_mission(mission_name, program_id, launcher_id, status);
		mission.id = db_get_last_inserted_id();
	} else if(mission_name[0] != '\0') {
		db_update_mission(mission.id, mission_name, program_id, launcher_id, status);
	}
	update_mman_objectives_list();
	for(int i = 0; i < num_objectives; i++) {
		struct MissionObjective_DB obj = objectives[i];
		if(obj.id  > 0) db_update_objective(obj.id, mission.id, obj.status, (int) obj.rank+1, obj.objective);
		if(obj.id == 0) db_new_objective(mission.id, obj.status, (int) obj.rank+1, obj.objective);
		if(obj.id  < 0) db_remove_objective(-obj.id);
	}
	update_mman_event_list();
	for(int i = 0; i < num_events; i++) {
		struct MissionEvent_DB event = events[i];
		double epoch = get_event_epoch(&(event_list[i]));
		if(event.id  > 0) db_update_event(event.id, mission.id, epoch, event.event);
		if(event.id == 0) db_new_event(mission.id, epoch, event.event);;
		if(event.id  < 0) db_remove_event(-event.id);
	}
	reset_mission_manager();
	update_db_box();
	switch_to_mission_database_page();
}

void reset_mission_manager() {
	free(objectives);
	free(obj_list);
	free(events);
	free(event_list);
	num_objectives = 0;
	num_events = 0;
}

void update_lists_with_id_and_references() {
	for(int i = 0; i < num_objectives; i++) {
		obj_list[i].objective = &(objectives[i]);
		obj_list[i].list_id = i;
	}
	for(int i = 0; i < num_events; i++) {
		event_list[i].event = &(events[i]);
		event_list[i].list_id = i;
		if(event_list[i].event_type == INITIAL_EVENT) initial_event_id = i;
	}
}


/* ----------------------------
 * 		OBJECTIVE BOX
 * ----------------------------
 */


/* ----------------------------
 * 		EVENT BOX
 * ----------------------------
 */


