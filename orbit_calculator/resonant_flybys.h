#ifndef KMAT_RESONANT_FLYBYS_H
#define KMAT_RESONANT_FLYBYS_H

#include "itin_tool.h"

// Samples of (duration, variant) for a first leg that returns to the departure body, around 1 and 2 of its periods.
int resonant_departure_sample_count(void);
void resonant_departure_sample(int index, double period_days, double *duration_days, int *variant);
// Adds next steps that return to the body of tf after revolutions with (almost) unchanged excess speed.
void find_resonant_flybys(struct ItinStep *tf, CelestSystem *system, double min_dt, double max_dt);

#endif
