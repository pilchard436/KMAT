#include "powered_route.h"
#include "leg_variants.h"
#include <stdlib.h>
#include <string.h>

#define POWERED_REPAIR_DATE_BINS 64

int initialize_candidate(PoweredRouteCandidate *candidate, struct ItinStep *arrival,
		struct ItinStep **departures, int num_departures, CelestSystem *system) {
	size_t num_steps = 0;
	for(struct ItinStep *step = arrival; step != NULL; step = step->prev) {
		if(step->body == NULL) return 0;
		num_steps++;
	}
	if(num_steps < 3) return 0;

	candidate->num_steps = num_steps;
	candidate->bodies = malloc(num_steps * sizeof(*candidate->bodies));
	candidate->signature_dates = malloc(num_steps * sizeof(*candidate->signature_dates));
	candidate->signature_positions = malloc(num_steps * sizeof(*candidate->signature_positions));
	candidate->signature_v_arr = malloc(num_steps * sizeof(*candidate->signature_v_arr));
	candidate->signature_v_dep = malloc(num_steps * sizeof(*candidate->signature_v_dep));
	candidate->times = malloc(num_steps * sizeof(*candidate->times));
	candidate->best_states = malloc(num_steps * sizeof(*candidate->best_states));
	candidate->best_legs = malloc((num_steps - 1) * sizeof(*candidate->best_legs));
	if(candidate->bodies == NULL || candidate->signature_dates == NULL ||
		candidate->signature_positions == NULL || candidate->signature_v_arr == NULL ||
		candidate->signature_v_dep == NULL || candidate->times == NULL ||
		candidate->best_states == NULL || candidate->best_legs == NULL) return 0;

	struct ItinStep *nodes[num_steps];
	struct ItinStep *step = arrival;
	for(size_t reverse_index = num_steps; reverse_index > 0; reverse_index--) {
		size_t i = reverse_index - 1;
		nodes[i] = step;
		candidate->bodies[i] = step->body;
		candidate->signature_dates[i] = step->date;
		candidate->signature_positions[i] = step->r;
		candidate->signature_v_arr[i] = step->v_arr;
		candidate->signature_v_dep[i] = step->v_dep;
		candidate->times[i] = step->date;
		step = step->prev;
	}
	struct ItinStep *root = nodes[0];
	candidate->departure_index = -1;
	for(int i = 0; i < num_departures; i++) {
		if(departures[i] == root) {
			candidate->departure_index = i;
			break;
		}
	}
	if(candidate->departure_index < 0) return 0;

	candidate->mutable_start = 1;
	for(size_t i = 0; i + 1 < num_steps; i++)
		if(nodes[i]->num_next_nodes > 1) candidate->mutable_start = i + 1;
	candidate->best_total_dv = DBL_MAX;

	// Legs back to the same body keep the revolution variant the search found them with.
	int has_resonant_leg = 0;
	for(size_t i = 0; i + 1 < num_steps; i++) has_resonant_leg |= nodes[i]->body == nodes[i + 1]->body;
	if(has_resonant_leg && system != NULL) {
		candidate->leg_variants = calloc(num_steps - 1, sizeof(*candidate->leg_variants));
		for(size_t i = 0; i + 1 < num_steps && candidate->leg_variants != NULL; i++) {
			if(nodes[i]->body != nodes[i + 1]->body) continue;
			Lambert3 leg;
			if(!nearest_leg_variant_lambert(system->cb, nodes[i]->r, nodes[i + 1]->r,
				(nodes[i + 1]->date - nodes[i]->date) * 86400, nodes[i + 1]->v_dep, &leg,
				&candidate->leg_variants[i])) {
				free(candidate->leg_variants);
				candidate->leg_variants = NULL;
			}
		}
	}
	return candidate->mutable_start < num_steps;
}

void free_candidate(PoweredRouteCandidate *candidate) {
	free(candidate->bodies);
	free(candidate->signature_dates);
	free(candidate->signature_positions);
	free(candidate->signature_v_arr);
	free(candidate->signature_v_dep);
	free(candidate->times);
	free(candidate->best_states);
	free(candidate->best_legs);
	free(candidate->dsms);
	free(candidate->dsm_results);
	free(candidate->leg_variants);
	memset(candidate, 0, sizeof(*candidate));
}

void destroy_candidate(PoweredRouteCandidate *candidate) {
	if(candidate == NULL) return;
	free_candidate(candidate);
	free(candidate);
}

uint64_t sample_random(uint64_t *state) {
	*state ^= *state >> 12;
	*state ^= *state << 25;
	*state ^= *state >> 27;
	return *state * UINT64_C(2685821657736338717);
}

/* Algorithm R: every excluded route has equal sampling probability. */
void reservoir_retain(SampleReservoir *pool, PoweredRouteCandidate *candidate) {
	if(pool == NULL) { destroy_candidate(candidate); return; }
	pool->population++;
	unsigned long long slot;
	if(pool->count < POWERED_CHALLENGE_SLOTS) slot = (unsigned long long)pool->count++;
	else {
		uint64_t limit = UINT64_MAX - UINT64_MAX % pool->population, draw;
		do { draw = sample_random(&pool->random); } while(draw >= limit);
		slot = draw % pool->population;
	}
	if(slot >= POWERED_CHALLENGE_SLOTS) { destroy_candidate(candidate); return; }
	destroy_candidate(pool->samples[slot]);
	pool->samples[slot] = candidate;
}

void retain_excluded(ExcludedPools *pools, PoweredRouteCandidate *candidate) {
	reservoir_retain(pools->challenge, candidate);
}

/* Deterministic stratified sampling: one reservoir slot per departure-date/body-sequence hash. */
void repair_pool_retain(RepairPool *pool, PoweredRouteCandidate *candidate, int num_departures) {
	uint64_t sequence_hash = 1469598103934665603ULL;
	for(size_t i = 0; i < candidate->num_steps; i++)
		for(const char *name = candidate->bodies[i]->name; *name; name++)
			sequence_hash = (sequence_hash ^ (unsigned char)*name) * 1099511628211ULL;
	uint64_t date_bin = (uint64_t)candidate->departure_index * POWERED_REPAIR_DATE_BINS / (uint64_t)num_departures;
	size_t slot = (size_t)((date_bin * 17 + sequence_hash % POWERED_REPAIR_SLOTS) % POWERED_REPAIR_SLOTS);
	pool->total++;
	unsigned long long seen = ++pool->population[slot];
	if(seen == 1 || sample_random(&pool->random) % seen == 0) {
		destroy_candidate(pool->slots[slot]);
		pool->slots[slot] = candidate;
	} else {
		destroy_candidate(candidate);
	}
}

static int vector_equal(Vector3 first, Vector3 second) {
	return mag_vec3(subtract_vec3(first, second)) < 1e-6;
}

static int route_node_matches(const PoweredRouteCandidate *candidate,
		const struct ItinStep *step, size_t index) {
	return step != NULL && step->body == candidate->bodies[index] &&
		fabs(step->date - candidate->signature_dates[index]) < 1e-9 &&
		vector_equal(step->r, candidate->signature_positions[index]) &&
		vector_equal(step->v_arr, candidate->signature_v_arr[index]) &&
		vector_equal(step->v_dep, candidate->signature_v_dep[index]);
}

static struct ItinStep *find_matching_leaf(struct ItinStep *step,
		const PoweredRouteCandidate *candidate, size_t index) {
	if(index >= candidate->num_steps || !route_node_matches(candidate, step, index)) return NULL;
	if(index + 1 == candidate->num_steps) return step->num_next_nodes == 0 ? step : NULL;
	for(int child = 0; child < step->num_next_nodes; child++) {
		struct ItinStep *match = find_matching_leaf(step->next[child], candidate, index + 1);
		if(match != NULL) return match;
	}
	return NULL;
}

/* Flyby corrections change inbound-leg velocities on every node after the departure. */
int leg_has_dsm(const PoweredRouteCandidate *candidate, size_t leg) {
	return candidate->dsms != NULL && candidate->dsm_results != NULL &&
		candidate->dsms[leg].enabled && candidate->dsm_results[leg].used;
}

struct ItinStep *make_optimized_suffix(const PoweredRouteCandidate *candidate) {
	size_t suffix_count = candidate->num_steps - 1;
	for(size_t leg = 0; leg + 1 < candidate->num_steps; leg++)
		suffix_count += leg_has_dsm(candidate, leg);
	struct ItinStep **nodes = calloc(suffix_count, sizeof(*nodes));
	if(nodes == NULL) return NULL;
	for(size_t i = 0; i < suffix_count; i++) {
		nodes[i] = calloc(1, sizeof(*nodes[i]));
		if(nodes[i] == NULL) {
			for(size_t j = 0; j < i; j++) free(nodes[j]);
			free(nodes);
			return NULL;
		}
	}
	size_t k = 0;
	for(size_t route_index = 1; route_index < candidate->num_steps; route_index++) {
		size_t leg = route_index - 1;
		Vector3 arriving_leg_start_velocity = candidate->best_legs[leg].v0;
		if(leg_has_dsm(candidate, leg)) {
			const PoweredDsmResult *dsm = &candidate->dsm_results[leg];
			struct ItinStep *maneuver = nodes[k++];
			maneuver->body = NULL;
			maneuver->date = dsm->date;
			maneuver->r = dsm->position;
			/* Non-zero v_body marks a real maneuver for the drawing code. */
			maneuver->v_body = dsm->velocity_before;
			maneuver->v_arr = dsm->velocity_before;
			maneuver->v_dep = candidate->best_legs[leg].v0;
			arriving_leg_start_velocity = dsm->velocity_after;
		}
		struct ItinStep *node = nodes[k++];
		node->body = candidate->bodies[route_index];
		node->date = candidate->times[route_index];
		node->r = candidate->best_states[route_index].r;
		node->v_body = candidate->best_states[route_index].v;
		node->v_arr = candidate->best_legs[leg].v1;
		node->v_dep = arriving_leg_start_velocity;
	}
	for(size_t i = 0; i + 1 < suffix_count; i++) {
		nodes[i]->next = malloc(sizeof(*nodes[i]->next));
		if(nodes[i]->next == NULL) {
			for(size_t j = 0; j < suffix_count; j++) {
				free(nodes[j]->next);
				free(nodes[j]);
			}
			free(nodes);
			return NULL;
		}
		nodes[i]->num_next_nodes = 1;
		nodes[i]->next[0] = nodes[i + 1];
	}
	for(size_t i = 0; i < suffix_count; i++)
		nodes[i]->prev = i == 0 ? NULL : nodes[i - 1];
	struct ItinStep *first = nodes[0];
	free(nodes);
	return first;
}

int replace_candidate_route(struct ItinStep **departures, PoweredRouteCandidate *candidate) {
	struct ItinStep *original_root = departures[candidate->departure_index];
	struct ItinStep *root = create_itin_copy(original_root);
	if(root == NULL) return 0;
	struct ItinStep *leaf = find_matching_leaf(root, candidate, 0);
	if(leaf == NULL) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep *old_suffix = leaf;
	for(size_t i = candidate->num_steps - 1; i > candidate->mutable_start; i--)
		old_suffix = old_suffix->prev;
	struct ItinStep *parent = old_suffix->prev;
	if(parent == NULL) {
		free_itinerary(root);
		return 0;
	}
	int child_index = -1;
	for(int i = 0; i < parent->num_next_nodes; i++) {
		if(parent->next[i] == old_suffix) {
			child_index = i;
			break;
		}
	}
	if(child_index < 0) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep *suffix = make_optimized_suffix(candidate);
	if(suffix == NULL) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep **expanded_children = realloc(root->next,
		(root->num_next_nodes + 1) * sizeof(*root->next));
	if(expanded_children == NULL) {
		free_itinerary(suffix);
		free_itinerary(root);
		return 0;
	}
	root->next = expanded_children;
	for(int i = child_index; i + 1 < parent->num_next_nodes; i++)
		parent->next[i] = parent->next[i + 1];
	parent->num_next_nodes--;
	free_itinerary(old_suffix);
	suffix->prev = root;
	root->next[root->num_next_nodes++] = suffix;
	departures[candidate->departure_index] = root;
	free_itinerary(original_root);
	return 1;
}

int insert_top_candidate(PoweredRouteCandidate *candidate, double cost,
		int max_routes, int *count,
		PoweredRouteCandidate *candidates[POWERED_ROUTE_REFINEMENT_LIMIT],
		double costs[POWERED_ROUTE_REFINEMENT_LIMIT], ExcludedPools *excluded) {
	if(*count == max_routes && cost >= costs[*count - 1]) return 0;
	int insert = *count < max_routes ? (*count)++ : max_routes - 1;
	if(*count == max_routes && candidates[insert] != NULL)
		retain_excluded(excluded, candidates[insert]);
	while(insert > 0 && cost < costs[insert - 1]) {
		candidates[insert] = candidates[insert - 1];
		costs[insert] = costs[insert - 1];
		insert--;
	}
	candidates[insert] = candidate;
	costs[insert] = cost;
	return 1;
}
