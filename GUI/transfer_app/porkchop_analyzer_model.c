#include "gui/transfer_app/porkchop_analyzer_model.h"

#include <string.h>

int find_first_visible_porkchop_point(const struct PorkchopAnalyzerPoint *points, int num_points) {
	if(points == NULL || num_points <= 0) return -1;
	for(int i = 0; i < num_points; i++) {
		if(points[i].inside_filter && points[i].group != NULL && points[i].group->show_group)
			return i;
	}
	return -1;
}

void mark_itinerary_bodies_visible(int *visible, size_t num_bodies, const struct ItinStep *selected_step, const CelestSystem *system) {
	if(visible == NULL || system == NULL) return;
	memset(visible, 0, num_bodies * sizeof(*visible));
	if(system->num_bodies <= 0 || system->bodies == NULL) return;

	while(selected_step != NULL) {
		if(selected_step->body != NULL) {
			for(size_t body_index = 0; body_index < num_bodies && body_index < (size_t)system->num_bodies; body_index++) {
				if(system->bodies[body_index] == selected_step->body)
					visible[body_index] = 1;
			}
		}
		selected_step = selected_step->prev;
	}
}

int set_porkchop_groups_visibility(struct PorkchopGroup *groups, int num_groups, int show) {
        if(groups == NULL) return 0;
        int changed = 0;
        for(int group_idx = 0; group_idx < num_groups; group_idx++) {
                if(!groups[group_idx].has_itin_inside_filter) continue;
                changed += groups[group_idx].show_group != show;
                groups[group_idx].show_group = show;
        }
        return changed;
}
