#ifndef KMAT_PORKCHOP_ANALYZER_MODEL_H
#define KMAT_PORKCHOP_ANALYZER_MODEL_H

#include "gui/transfer_app/porkchop_analyzer_tools.h"

int find_first_visible_porkchop_point(const struct PorkchopAnalyzerPoint *points, int num_points);
// Shows or hides every group that has an itinerary inside the filter; other groups keep their state.
// Returns how many groups changed.
int set_porkchop_groups_visibility(struct PorkchopGroup *groups, int num_groups, int show);
void mark_itinerary_bodies_visible(int *visible, size_t num_bodies, const struct ItinStep *selected_step, const CelestSystem *system);

#endif