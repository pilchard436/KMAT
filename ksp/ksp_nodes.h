#ifndef KMAT_KSP_NODES_H
#define KMAT_KSP_NODES_H

#include "orbit_calculator/itin_tool.h"
#include <stddef.h>

int ksp_project_delta_v_to_node(Vector3 position, Vector3 velocity, Vector3 delta_v, Vector3 *components);
int ksp_add_next_maneuver_node(struct ItinStep *route, double dep_periapsis, double arr_periapsis,
		enum LastTransferType transfer_type, char *message, size_t message_size);

#endif
