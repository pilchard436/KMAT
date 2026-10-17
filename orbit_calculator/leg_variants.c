#include "leg_variants.h"
#include <math.h>

Lambert3 calc_leg_variant_lambert(Body *cb, Vector3 r0, Vector3 r1, double dt, int variant,
		LambertCancelCheck cancel_check, void *cancel_context) {
	if(variant <= 0) return calc_lambert3_cancellable(r0, r1, dt, cb, cancel_check, cancel_context);
	return calc_lambert3_revs_cancellable(r0, r1, dt, cb, (variant + 1) / 2, (variant - 1) % 2,
		cancel_check, cancel_context);
}

int is_usable_lambert(Lambert3 leg) {
	return leg.success != LAMBERT_FAIL_NAN && leg.success != LAMBERT_FAIL_ECC &&
		leg.success != LAMBERT_MAX_ITERATIONS && isfinite(leg.v0.x + leg.v0.y + leg.v0.z) &&
		isfinite(leg.v1.x + leg.v1.y + leg.v1.z);
}

int nearest_leg_variant_lambert(Body *cb, Vector3 r0, Vector3 r1, double dt, Vector3 v_ref, Lambert3 *leg, int *variant) {
	double best_distance = HUGE_VAL;
	for(int candidate_variant = 0; candidate_variant < LEG_VARIANT_COUNT; candidate_variant++) {
		Lambert3 candidate = calc_leg_variant_lambert(cb, r0, r1, dt, candidate_variant, NULL, NULL);
		if(!is_usable_lambert(candidate)) continue;
		double distance = mag_vec3(subtract_vec3(candidate.v0, v_ref));
		if(distance < best_distance) {
			best_distance = distance;
			*leg = candidate;
			if(variant != NULL) *variant = candidate_variant;
		}
	}
	return best_distance < HUGE_VAL;
}
