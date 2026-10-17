#include "test_support.h"
#include "orbit_calculator/powered_route.h"

#include <stdlib.h>

static Body bodies_fixture[3] = {{.name = "EARTH"}, {.name = "VENUS"}, {.name = "MARS"}};

static PoweredRouteCandidate *make_candidate(double cost, int departure_index) {
	PoweredRouteCandidate *candidate = calloc(1, sizeof(*candidate));
	candidate->num_steps = 3;
	candidate->bodies = malloc(3 * sizeof(*candidate->bodies));
	for(int i = 0; i < 3; i++) candidate->bodies[i] = &bodies_fixture[i];
	candidate->best_total_dv = cost;
	candidate->departure_index = departure_index;
	return candidate;
}

static void check_random_sequence(void) {
	uint64_t first = 88172645463325252ULL, second = first;
	uint64_t a = sample_random(&first), b = sample_random(&first);
	check_true("the sampler never returns a repeated value in a row", a != b);
	check_true("the sampler advances its state", first != second);
	check_true("the sampler is deterministic for a seed", sample_random(&second) == a && sample_random(&second) == b);
}

static void check_top_candidates(void) {
	PoweredRouteCandidate *candidates[POWERED_ROUTE_REFINEMENT_LIMIT] = {0};
	double costs[POWERED_ROUTE_REFINEMENT_LIMIT];
	SampleReservoir challenge = {.random = 7};
	ExcludedPools excluded = {.challenge = &challenge};
	int count = 0;

	PoweredRouteCandidate *five = make_candidate(5, 0), *three = make_candidate(3, 1), *nine = make_candidate(9, 2);
	check_true("the first candidates are accepted", insert_top_candidate(five, 5, 3, &count, candidates, costs, &excluded) &&
		insert_top_candidate(three, 3, 3, &count, candidates, costs, &excluded) &&
		insert_top_candidate(nine, 9, 3, &count, candidates, costs, &excluded));
	check_true("candidates are kept in ascending cost order", count == 3 && candidates[0] == three && candidates[1] == five &&
		candidates[2] == nine && costs[0] == 3 && costs[1] == 5 && costs[2] == 9);
	check_true("nothing is excluded while there is room", challenge.population == 0);

	PoweredRouteCandidate *one = make_candidate(1, 3);
	check_true("a cheaper candidate replaces the worst", insert_top_candidate(one, 1, 3, &count, candidates, costs, &excluded));
	check_true("the cheapest candidate leads", count == 3 && candidates[0] == one && candidates[1] == three && candidates[2] == five);
	check_true("the replaced candidate is kept as a challenge sample", challenge.population == 1 && challenge.count == 1 &&
		challenge.samples[0] == nine);

	PoweredRouteCandidate *ten = make_candidate(10, 4);
	check_true("a candidate worse than the cutoff is rejected", !insert_top_candidate(ten, 10, 3, &count, candidates, costs, &excluded));
	check_true("rejection leaves the ranking alone", count == 3 && candidates[2] == five);
	destroy_candidate(ten);

	for(int i = 0; i < 3; i++) destroy_candidate(candidates[i]);
	destroy_candidate(challenge.samples[0]);
}

static void check_reservoir(void) {
	SampleReservoir reservoir = {.random = 12345};
	for(int i = 0; i < 4 * POWERED_CHALLENGE_SLOTS; i++) reservoir_retain(&reservoir, make_candidate(i, 0));
	check_true("the reservoir counts every excluded route", reservoir.population == 4 * POWERED_CHALLENGE_SLOTS);
	check_true("the reservoir keeps at most its slot count", reservoir.count == POWERED_CHALLENGE_SLOTS);
	int filled = 1, distinct = 1;
	for(int i = 0; i < reservoir.count; i++) {
		filled &= reservoir.samples[i] != NULL;
		for(int j = 0; j < i; j++) distinct &= reservoir.samples[i] != reservoir.samples[j];
	}
	check_true("every reservoir slot holds a route", filled);
	check_true("no route fills two slots", distinct);
	for(int i = 0; i < reservoir.count; i++) destroy_candidate(reservoir.samples[i]);

	reservoir_retain(NULL, make_candidate(1, 0));
	check_true("retaining without a reservoir frees the route", 1);
}

static void check_repair_pool(void) {
	RepairPool *pool = calloc(1, sizeof(*pool));
	pool->random = 99;
	const int num_departures = 40;
	for(int i = 0; i < 300; i++) repair_pool_retain(pool, make_candidate(DBL_MAX, i % num_departures), num_departures);
	unsigned long long seen = 0;
	int filled = 0;
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS; slot++) {
		seen += pool->population[slot];
		if(pool->slots[slot] != NULL) filled++;
		check_true("an occupied slot has a population", (pool->slots[slot] != NULL) == (pool->population[slot] > 0));
	}
	check_true("the pool counts every rejected route", pool->total == 300 && seen == 300);
	check_true("the pool keeps at most one route per slot", filled > 0 && filled <= POWERED_REPAIR_SLOTS);
	for(int slot = 0; slot < POWERED_REPAIR_SLOTS; slot++) destroy_candidate(pool->slots[slot]);
	free(pool);
}

static void check_candidate_lifecycle(void) {
	PoweredRouteCandidate *candidate = make_candidate(2, 0);
	check_true("a route without maneuvers has no maneuver leg", !leg_has_dsm(candidate, 0));
	PoweredLegDsm dsms[2] = {{.enabled = 1}, {.enabled = 0}};
	PoweredDsmResult results[2] = {{.used = 1}, {.used = 1}};
	candidate->dsms = dsms;
	candidate->dsm_results = results;
	check_true("an enabled and used maneuver is reported", leg_has_dsm(candidate, 0));
	check_true("a disabled maneuver is ignored", !leg_has_dsm(candidate, 1));
	results[0].used = 0;
	check_true("an unused maneuver is ignored", !leg_has_dsm(candidate, 0));
	candidate->dsms = NULL;
	candidate->dsm_results = NULL;

	free_candidate(candidate);
	check_true("freeing a route clears it", candidate->bodies == NULL && candidate->num_steps == 0 && candidate->best_total_dv == 0);
	free(candidate);
	destroy_candidate(NULL);
	check_true("destroying nothing is allowed", 1);

	check_true("finite vectors are recognized", is_finite_vector(vec3(1, 2, 3)));
	check_true("infinite vectors are rejected", !is_finite_vector(vec3(1, INFINITY, 3)));
	check_true("undefined vectors are rejected", !is_finite_vector(vec3(NAN, 0, 0)));
}

int main(void) {
	check_random_sequence();
	check_top_candidates();
	check_reservoir();
	check_repair_pool();
	check_candidate_lifecycle();
	return finish_tests("KMAT optimizer unit checks");
}
