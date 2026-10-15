#ifndef KMAT_POWERED_SEED_SELECTION_H
#define KMAT_POWERED_SEED_SELECTION_H

#include "orbitlib.h"
#include <math.h>

#define POWERED_SEED_BUCKETS 4
#define POWERED_SEEDS_PER_BUCKET 3

typedef struct PoweredFlybySeed {
	double dt, diff_vinf, estimated_burn, actual_burn;
	OSV target_state;
	Lambert3 transfer;
} PoweredFlybySeed;

typedef struct PoweredSeedBucket {
	PoweredFlybySeed seeds[POWERED_SEEDS_PER_BUCKET];
	int count;
} PoweredSeedBucket;

/* Keep distinct dates; a cheap estimate orders work, never establishes feasibility. */
static inline void retain_powered_seed(PoweredSeedBucket *bucket, PoweredFlybySeed seed) {
	for(int i = 0; i < bucket->count; i++)
		if(fabs(bucket->seeds[i].dt - seed.dt) < 1e-6) return;
	int index = bucket->count;
	if(index == POWERED_SEEDS_PER_BUCKET) {
		if(seed.estimated_burn >= bucket->seeds[index - 1].estimated_burn) return;
		index--;
	} else bucket->count++;
	while(index > 0 && seed.estimated_burn < bucket->seeds[index - 1].estimated_burn) {
		bucket->seeds[index] = bucket->seeds[index - 1];
		index--;
	}
	bucket->seeds[index] = seed;
}

typedef double (*PoweredSeedCost)(const PoweredFlybySeed *, void *);

/* At most three full evaluations per bucket, falling back after infeasibility. */
static inline int select_powered_seed(const PoweredSeedBucket *bucket,
		PoweredSeedCost evaluate, void *context, PoweredFlybySeed *selected,
		double *cost, unsigned long long *evaluations) {
	for(int i = 0; i < bucket->count; i++) {
		(*evaluations)++;
		double value = evaluate(&bucket->seeds[i], context);
		if(!isfinite(value)) continue;
		*selected = bucket->seeds[i];
		*cost = value;
		return 1;
	}
	return 0;
}
#endif
