#ifndef KMAT_LEG_VARIANTS_H
#define KMAT_LEG_VARIANTS_H

#include "itin_tool.h"

// A leg is solved as variant 0 (no revolutions) or as 1..LEG_VARIANT_COUNT-1: revolutions (v+1)/2, branch (v-1)%2.
#define LEG_VARIANT_MAX_REVS 4
#define LEG_VARIANT_COUNT (1 + 2 * LEG_VARIANT_MAX_REVS)
Lambert3 calc_leg_variant_lambert(Body *cb, Vector3 r0, Vector3 r1, double dt, int variant,
		LambertCancelCheck cancel_check, void *cancel_context);
// Solves variants 0..count-1 into legs, sharing each revolution count's geometry between its two branches.
void calc_leg_variant_lamberts(Body *cb, Vector3 r0, Vector3 r1, double dt, int count, Lambert3 *legs,
		LambertCancelCheck cancel_check, void *cancel_context);
// Picks the variant whose departure velocity is closest to v_ref; returns 0 if none exists.
int nearest_leg_variant_lambert(Body *cb, Vector3 r0, Vector3 r1, double dt, Vector3 v_ref, Lambert3 *leg, int *variant);
// 1 if the Lambert solution converged and has finite velocities.
int is_usable_lambert(Lambert3 leg);

#endif
