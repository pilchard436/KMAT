#include "itinerary_tree.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct ItinStep * get_first(struct ItinStep *tf) {
	if(tf == NULL) return NULL;
	while(tf->prev != NULL) tf = tf->prev;
	return tf;
}

struct ItinStep * get_last(struct ItinStep *tf) {
	if(tf == NULL) return NULL;
	while(tf->next != NULL) tf = tf->next[0];
	return tf;
}

void print_itinerary(struct ItinStep *itin) {
	if(itin->prev != NULL) {
		print_itinerary(itin->prev);
		printf(" - ");
	}
	print_date(convert_JD_date(itin->date, DATE_ISO), 0);
}

int get_number_of_itineraries(struct ItinStep *itin) {
	if(itin == NULL || (itin->prev == NULL && itin->num_next_nodes == 0)) return 0;
	if(itin->num_next_nodes == 0) return 1;
	int counter = 0;
	for(int i = 0; i < itin->num_next_nodes; i++) {
		counter += get_number_of_itineraries(itin->next[i]);
	}
	return counter;
}

int get_total_number_of_stored_steps(struct ItinStep *itin) {
	if(itin == NULL) return 0;
	if(itin->num_next_nodes == 0) return 1;
	int counter = 1;
	for(int i = 0; i < itin->num_next_nodes; i++) {
		counter += get_total_number_of_stored_steps(itin->next[i]);
	}
	return counter;
}

void store_itineraries_in_array(struct ItinStep *itin, struct ItinStep **array, int *index) {
	if(itin->num_next_nodes == 0) {
		array[*index] = itin;
		(*index)++;
	}

	for(int i = 0; i < itin->num_next_nodes; i++) {
		store_itineraries_in_array(itin->next[i], array, index);
	}
}

typedef struct {
	struct ItinStep **arrivals;
	int num_itins;
	int capacity;
	int num_nodes;
} ItineraryCollection;

static int append_itinerary_arrival(ItineraryCollection *collection, struct ItinStep *arrival) {
	if(collection->num_itins == collection->capacity) {
		int new_capacity = collection->capacity == 0 ? 256 :
				(collection->capacity > INT_MAX / 2 ? INT_MAX : collection->capacity * 2);
		if(new_capacity <= collection->capacity ||
			(size_t)new_capacity > SIZE_MAX / sizeof(*collection->arrivals)) return 0;
		struct ItinStep **new_arrivals = realloc(collection->arrivals, (size_t)new_capacity * sizeof(*new_arrivals));
		if(new_arrivals == NULL) return 0;
		collection->arrivals = new_arrivals;
		collection->capacity = new_capacity;
	}
	collection->arrivals[collection->num_itins++] = arrival;
	return 1;
}

static int collect_itinerary_tree(struct ItinStep *step, ItineraryCollection *collection) {
	if(step == NULL) return 1;
	if(collection->num_nodes == INT_MAX) return 0;
	collection->num_nodes++;
	if(step->num_next_nodes < 0) return 0;
	if(step->num_next_nodes == 0)
		return step->prev == NULL || append_itinerary_arrival(collection, step);
	if(step->next == NULL) return 0;
	for(int next_idx = 0; next_idx < step->num_next_nodes; next_idx++)
		if(!collect_itinerary_tree(step->next[next_idx], collection)) return 0;
	return 1;
}

int collect_itineraries_from_departures(struct ItinStep **departures, int num_deps,
		struct ItinStep ***arrivals, int *num_itins, int *num_nodes) {
	if(arrivals == NULL || num_itins == NULL || num_nodes == NULL || num_deps < 0 ||
		(num_deps > 0 && departures == NULL)) return 0;
	*arrivals = NULL;
	*num_itins = 0;
	*num_nodes = 0;

	ItineraryCollection collection = {0};
	for(int dep_idx = 0; dep_idx < num_deps; dep_idx++) {
		if(!collect_itinerary_tree(departures[dep_idx], &collection)) {
			free(collection.arrivals);
			return 0;
		}
	}
	*arrivals = collection.arrivals;
	*num_itins = collection.num_itins;
	*num_nodes = collection.num_nodes;
	return 1;
}

double get_itinerary_duration(struct ItinStep *itin) {
	double jd1 = itin->date;
	while(itin->prev != NULL) itin = itin->prev;
	double jd0 = itin->date;
	return jd1-jd0;
}

int get_num_of_itin_layers(struct ItinStep *step) {
	int counter = 0;
	while(step != NULL) {
		counter++;
		if(step->next != NULL) step = step->next[0];
		else break;
	}
	return counter;
}

void update_itin_body_osvs(struct ItinStep *step, CelestSystem *system) {
	OSV body_osv;
	while(step != NULL) {
		if(step->body != NULL) {
			body_osv = system->prop_method == ORB_ELEMENTS ?
					osv_from_elements(step->body->orbit, step->date) :
					osv_from_ephem(step->body->ephem, step->body->num_ephems, step->date, system->cb);
			step->r = body_osv.r;
			step->v_body = body_osv.v;
		} else step->v_body = vec3(1, 0, 0);	// non-zero marks a valid maneuver node so its trajectory is drawn
		step = step->next != NULL ? step->next[0] : NULL;
	}
}

void calc_itin_v_vectors_from_dates_and_r(struct ItinStep *step, CelestSystem *system) {
	if(step == NULL) return;
	struct ItinStep *next;
	while(step->next != NULL) {
		next = step->next[0];
		// A deep-space maneuver node keeps its stored position, so its two arcs are ordinary Lambert legs.
		double dt = (next->date - step->date) * 86400;
		Lambert3 transfer = calc_lambert3(step->r, next->r, dt, system->cb);
		next->v_dep = transfer.v0;
		next->v_arr = transfer.v1;
		step = next;
	}
}

void copy_step_body_vectors_and_date(struct ItinStep *orig_step, struct ItinStep *step_copy) {
	step_copy->body = orig_step->body;
	step_copy->r = orig_step->r;
	step_copy->v_body = orig_step->v_body;
	step_copy->v_arr = orig_step->v_arr;
	step_copy->v_dep = orig_step->v_dep;
	step_copy->date = orig_step->date;
	step_copy->dv_from_prev = orig_step->dv_from_prev;
	step_copy->dv_from_prev_valid = orig_step->dv_from_prev_valid;
	step_copy->arrival_dv_cache = orig_step->arrival_dv_cache;
}

struct ItinStep * create_itin_copy(struct ItinStep *step) {
	struct ItinStep *new_step = (struct ItinStep*) calloc(1, sizeof(struct ItinStep));
	copy_step_body_vectors_and_date(step, new_step);
	new_step->prev = NULL;
	new_step->num_next_nodes = step->num_next_nodes;
	if(step->num_next_nodes > 0) {
		new_step->next = (struct ItinStep **) malloc(step->num_next_nodes * sizeof(struct ItinStep *));
		for(int i = 0; i < new_step->num_next_nodes; i++) {
			new_step->next[i] = create_itin_copy(step->next[i]);
			new_step->next[i]->prev = new_step;
		}
	} else new_step->next = NULL;
	return new_step;
}

struct ItinStep * create_itin_copy_from_arrival(struct ItinStep *step) {
	struct ItinStep *step_copy = (struct ItinStep*) calloc(1, sizeof(struct ItinStep));
	copy_step_body_vectors_and_date(step, step_copy);
	step_copy->next = NULL;
	step_copy->num_next_nodes = 0;

	while(step->prev != NULL) {
		step_copy->prev = (struct ItinStep*) calloc(1, sizeof(struct ItinStep));
		step_copy->prev->next = (struct ItinStep**) malloc(sizeof(struct ItinStep*));
		step_copy->prev->next[0] = step_copy;
		step_copy->prev->num_next_nodes = 1;

		step = step->prev;
		step_copy = step_copy->prev;
		copy_step_body_vectors_and_date(step, step_copy);
	}


	step_copy->prev = NULL;
	return get_last(step_copy);
}

int is_valid_itinerary(struct ItinStep *step) {
	if(step == NULL || step->body == NULL || step->prev == NULL) return 0;
	while(step != NULL) {
		if(step->body == NULL && step->v_body.x == 0) return 0;
		step = step->prev;
	}

	return 1;
}

void set_itinerary_step_date(struct ItinStep *step, CelestSystem *system, double date) {
	if(step->body == NULL && step->prev != NULL)
		step->r = propagate_osv_time((OSV){step->prev->r, step->v_dep}, system->cb, (date - step->prev->date) * 86400).r;
	step->date = date;
	for(struct ItinStep *node = step; node->next != NULL; node = node->next[0])
		if(node->next[0]->date <= node->date) node->next[0]->date = node->date + 1;
}

void remove_step_from_itinerary(struct ItinStep *step) {
	struct ItinStep *prev = step->prev;

	while(prev->num_next_nodes == 1 && step->prev->prev != NULL) {
		step = prev;
		prev = step->prev;
	}

	int index = 0;
	for(int i = 0; i < prev->num_next_nodes; i++) {
		if(prev->next[i] == step) { index = i; break; }
	}
	prev->num_next_nodes--;
	for(int i = index; i < prev->num_next_nodes; i++) {
		prev->next[i] = prev->next[i+1];
	}
	free_itinerary(step);
}

void free_itinerary(struct ItinStep *step) {
	if(step == NULL) return;
	if(step->next != NULL) {
		for(int i = 0; i < step->num_next_nodes; i++) {
			free_itinerary(step->next[i]);
		}
		free(step->next);
	}
	free(step);
}
