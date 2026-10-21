#ifndef KMAT_SOI_ENCOUNTER_H
#define KMAT_SOI_ENCOUNTER_H

#include "orbitlib.h"

/* Returns non-zero when the coast enters an SOI other than those of its endpoint bodies.
 * Invalid or uncheckable trajectories are rejected conservatively. */
int coast_leg_crosses_unmodeled_soi(CelestSystem *system, Body *departure_body, Body *arrival_body,
		OSV departure_state, double departure_jd, double arrival_jd);

#endif
