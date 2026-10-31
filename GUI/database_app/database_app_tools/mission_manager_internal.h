#ifndef KMAT_MISSION_MANAGER_INTERNAL_H
#define KMAT_MISSION_MANAGER_INTERNAL_H

/* State shared by the mission manager modules (core, objectives, events). */

#include "mission_manager_tools.h"
#include "gui/database_app/mission_db.h"
#include "mission_db_tools.h"
#include "orbitlib.h"

struct MissionObjectiveList {
	struct MissionObjective_DB *objective;
	int list_id;
	GtkWidget *cb_rank, *cb_status, *tf_objective;
};

enum MissionEventType {INITIAL_EVENT, EPOCH_EVENT, TIMESINCE_EVENT};

struct MissionEventList {
	struct MissionEvent_DB *event;
	enum MissionEventType event_type;
	int list_id;
	GtkWidget *cb_type, *tf_year, *tf_month, *tf_day, *tf_hour, *tf_min, *tf_sec, *tf_event;
};

extern GtkWidget *grid_mman_objectives;
extern GtkWidget *grid_mman_events;
extern GObject *vp_mman_objectives;
extern GObject *vp_mman_events;
extern GObject *lb_mman_updatenew;
extern GObject *tf_mman_name;
extern GObject *cb_mman_program;
extern GObject *cb_mman_missionstatus;
extern GObject *cb_mman_launcher;
extern GObject *bt_mman_updatenew;
extern GObject *tf_mman_newprogram;
extern GObject *stack_newprogram;

extern struct MissionObjective_DB *objectives;
extern struct MissionObjectiveList *obj_list;
extern struct MissionEvent_DB *events;
extern struct MissionEventList *event_list;
extern int num_objectives, num_events, initial_event_id;
extern struct Mission_DB mission;

/* mission_manager_tools.c */
void update_lists_with_id_and_references();
void reset_mission_manager();

/* mission_manager_objectives.c */
void update_mman_objective_box();
void update_mman_objectives_list();

/* mission_manager_events.c */
void update_mman_event_box();
void update_mman_event_list();
double get_event_epoch(struct MissionEventList *event);

#endif
