#include "file_io_internal.h"
#include <stdlib.h>

union ItinStepBin convert_ItinStep_bin(struct ItinStep *step, CelestSystem *system, int step_type) {
	union ItinStepBin bin_step = {0};
	switch(step_type) {
		case 0:	bin_step.t0.r = step->r;
				bin_step.t0.v_dep = step->v_dep;
				bin_step.t0.v_arr = step->v_arr;
				bin_step.t0.v_body = step->v_body;
				bin_step.t0.date = step->date;
				bin_step.t0.num_next_nodes = step->num_next_nodes;
				break;
		case 2:	bin_step.t2.r = step->r;
				bin_step.t2.v_dep = step->v_dep;
				bin_step.t2.v_arr = step->v_arr;
				bin_step.t2.v_body = step->v_body;
				bin_step.t2.date = step->date;
				bin_step.t2.body_id = get_body_system_id(step->body, system);
				bin_step.t2.num_next_nodes = step->num_next_nodes;
				break;
		case 3:	bin_step.t3.node.r = step->r;
				bin_step.t3.node.v_dep = step->v_dep;
				bin_step.t3.node.v_arr = step->v_arr;
				bin_step.t3.node.v_body = step->v_body;
				bin_step.t3.node.date = step->date;
				bin_step.t3.node.body_id = get_body_system_id(step->body, system);
				bin_step.t3.node.num_next_nodes = step->num_next_nodes;
				bin_step.t3.dv_dep = step->arrival_dv_cache.dv_dep;
				bin_step.t3.dv_dsm = step->arrival_dv_cache.dv_dsm;
				bin_step.t3.dv_flyby = step->arrival_dv_cache.dv_flyby;
				bin_step.t3.dv_arr_cap = step->arrival_dv_cache.dv_arr_cap;
				bin_step.t3.dv_arr_circ = step->arrival_dv_cache.dv_arr_circ;
				break;
		case 4:	bin_step.t4.node.r = step->r;
				bin_step.t4.node.v_dep = step->v_dep;
				bin_step.t4.node.v_arr = step->v_arr;
				bin_step.t4.node.v_body = step->v_body;
				bin_step.t4.node.date = step->date;
				bin_step.t4.node.body_id = get_body_system_id(step->body, system);
				bin_step.t4.node.num_next_nodes = step->num_next_nodes;
				bin_step.t4.dv_dep = step->arrival_dv_cache.dv_dep;
				bin_step.t4.dv_dsm = step->arrival_dv_cache.dv_dsm;
				bin_step.t4.dv_flyby = step->arrival_dv_cache.dv_flyby;
				bin_step.t4.dv_arr_cap = step->arrival_dv_cache.dv_arr_cap;
				bin_step.t4.dv_arr_circ = step->arrival_dv_cache.dv_arr_circ;
				bin_step.t4.dv_arr_cap_plain = step->arrival_dv_cache.dv_arr_cap_plain;
				bin_step.t4.dv_arr_circ_plain = step->arrival_dv_cache.dv_arr_circ_plain;
				break;
		case 5:	bin_step.t5.node.r = step->r;
				bin_step.t5.node.v_dep = step->v_dep;
				bin_step.t5.node.v_arr = step->v_arr;
				bin_step.t5.node.v_body = step->v_body;
				bin_step.t5.node.date = step->date;
				bin_step.t5.node.body_id = get_body_system_id(step->body, system);
				bin_step.t5.node.num_next_nodes = step->num_next_nodes;
				bin_step.t5.dv_dep = step->arrival_dv_cache.dv_dep;
				bin_step.t5.dv_dsm = step->arrival_dv_cache.dv_dsm;
				bin_step.t5.dv_flyby = step->arrival_dv_cache.dv_flyby;
				bin_step.t5.dv_arr_cap = step->arrival_dv_cache.dv_arr_cap;
				bin_step.t5.dv_arr_circ = step->arrival_dv_cache.dv_arr_circ;
				bin_step.t5.dv_arr_cap_plain = step->arrival_dv_cache.dv_arr_cap_plain;
				bin_step.t5.dv_arr_circ_plain = step->arrival_dv_cache.dv_arr_circ_plain;
				bin_step.t5.dv_from_prev = step->dv_from_prev;
				break;
		default: bin_step = (union ItinStepBin) {0}; break;
	}

	return bin_step;
}

void store_step_in_bfile(struct ItinStep *step, CelestSystem *system, FILE *file, int step_type) {
	union ItinStepBin bin_step;
	switch(step_type) {
		case 0:	bin_step = convert_ItinStep_bin(step, system, step_type);
				fwrite(&bin_step.t0, sizeof(struct ItinStepBinT0), 1, file);
				break;
		case 2: bin_step = convert_ItinStep_bin(step, system, step_type);
				fwrite(&bin_step.t2, sizeof(struct ItinStepBinT2), 1, file);
				break;
		case 3: bin_step = convert_ItinStep_bin(step, system, step_type);
				fwrite(&bin_step.t3, sizeof(struct ItinStepBinT3), 1, file);
				break;
		case 4: bin_step = convert_ItinStep_bin(step, system, step_type);
				fwrite(&bin_step.t4, sizeof(struct ItinStepBinT4), 1, file);
				break;
		case 5: bin_step = convert_ItinStep_bin(step, system, step_type);
				fwrite(&bin_step.t5, sizeof(struct ItinStepBinT5), 1, file);
				break;
		default: break;
	}
	for(int i = 0; i < step->num_next_nodes; i++) {
		store_step_in_bfile(step->next[i], system, file, step_type);
	}
}

void convert_bin_ItinStep(union ItinStepBin bin_step, struct ItinStep *step, struct Body *body, CelestSystem *system, int step_type) {
	switch(step_type) {
		case 0:	step->body = body;
				step->r = bin_step.t0.r;
				step->v_arr = bin_step.t0.v_arr;
				step->v_body = bin_step.t0.v_body;
				step->v_dep = bin_step.t0.v_dep;
				step->date = bin_step.t0.date;
				step->num_next_nodes = bin_step.t0.num_next_nodes;
				step->arrival_dv_cache = (struct ItinArrivalDvCache) {0};
				break;
		case 2:	step->body = bin_step.t2.body_id >= 0 && bin_step.t2.body_id < system->num_bodies ?
					system->bodies[bin_step.t2.body_id] : NULL;
				step->r = bin_step.t2.r;
				step->v_arr = bin_step.t2.v_arr;
				step->v_body = bin_step.t2.v_body;
				step->v_dep = bin_step.t2.v_dep;
				step->date = bin_step.t2.date;
				step->num_next_nodes = bin_step.t2.num_next_nodes;
				step->arrival_dv_cache = (struct ItinArrivalDvCache) {0};
				break;
		case 3:	step->body = bin_step.t3.node.body_id >= 0 &&
					bin_step.t3.node.body_id < system->num_bodies ?
					system->bodies[bin_step.t3.node.body_id] : NULL;
				step->r = bin_step.t3.node.r;
				step->v_arr = bin_step.t3.node.v_arr;
				step->v_body = bin_step.t3.node.v_body;
				step->v_dep = bin_step.t3.node.v_dep;
				step->date = bin_step.t3.node.date;
				step->num_next_nodes = bin_step.t3.node.num_next_nodes;
				step->arrival_dv_cache = (struct ItinArrivalDvCache) {
					.dv_dep = bin_step.t3.dv_dep,
					.dv_dsm = bin_step.t3.dv_dsm,
					.dv_flyby = bin_step.t3.dv_flyby,
					.dv_arr_cap = bin_step.t3.dv_arr_cap,
					.dv_arr_circ = bin_step.t3.dv_arr_circ,
					.route_values_valid = 1,
					.incoming_excess = subtract_vec3(step->v_arr, step->v_body),
					.date = step->date
				};
				break;
		case 4:	step->body = bin_step.t4.node.body_id >= 0 &&
					bin_step.t4.node.body_id < system->num_bodies ?
					system->bodies[bin_step.t4.node.body_id] : NULL;
				step->r = bin_step.t4.node.r;
				step->v_arr = bin_step.t4.node.v_arr;
				step->v_body = bin_step.t4.node.v_body;
				step->v_dep = bin_step.t4.node.v_dep;
				step->date = bin_step.t4.node.date;
				step->num_next_nodes = bin_step.t4.node.num_next_nodes;
				step->arrival_dv_cache = (struct ItinArrivalDvCache) {
					.dv_dep = bin_step.t4.dv_dep,
					.dv_dsm = bin_step.t4.dv_dsm,
					.dv_flyby = bin_step.t4.dv_flyby,
					.dv_arr_cap = bin_step.t4.dv_arr_cap,
					.dv_arr_circ = bin_step.t4.dv_arr_circ,
					.dv_arr_cap_assisted = bin_step.t4.dv_arr_cap,
					.dv_arr_circ_assisted = bin_step.t4.dv_arr_circ,
					.dv_arr_cap_plain = bin_step.t4.dv_arr_cap_plain,
					.dv_arr_circ_plain = bin_step.t4.dv_arr_circ_plain,
					.moon_assist_dvs_valid = 1,
					.route_values_valid = 1,
					.incoming_excess = subtract_vec3(step->v_arr, step->v_body),
					.date = step->date
				};
				break;
		case 5:	step->body = bin_step.t5.node.body_id >= 0 &&
					bin_step.t5.node.body_id < system->num_bodies ?
					system->bodies[bin_step.t5.node.body_id] : NULL;
				step->r = bin_step.t5.node.r;
				step->v_arr = bin_step.t5.node.v_arr;
				step->v_body = bin_step.t5.node.v_body;
				step->v_dep = bin_step.t5.node.v_dep;
				step->date = bin_step.t5.node.date;
				step->num_next_nodes = bin_step.t5.node.num_next_nodes;
				step->arrival_dv_cache = (struct ItinArrivalDvCache) {
					.dv_dep = bin_step.t5.dv_dep,
					.dv_dsm = bin_step.t5.dv_dsm,
					.dv_flyby = bin_step.t5.dv_flyby,
					.dv_arr_cap = bin_step.t5.dv_arr_cap,
					.dv_arr_circ = bin_step.t5.dv_arr_circ,
					.dv_arr_cap_assisted = bin_step.t5.dv_arr_cap,
					.dv_arr_circ_assisted = bin_step.t5.dv_arr_circ,
					.dv_arr_cap_plain = bin_step.t5.dv_arr_cap_plain,
					.dv_arr_circ_plain = bin_step.t5.dv_arr_circ_plain,
					.moon_assist_dvs_valid = 1,
					.route_values_valid = 1,
					.incoming_excess = subtract_vec3(step->v_arr, step->v_body),
					.date = step->date
				};
				step->dv_from_prev = bin_step.t5.dv_from_prev;
				step->dv_from_prev_valid = step->prev != NULL;
				break;
		default: break;
	}
}

void load_step_from_bfile(struct ItinStep *step, FILE *file, struct Body **body, CelestSystem *system, int step_type) {
	union ItinStepBin bin_step;
	
	switch(step_type) {
		case 0:	fread(&bin_step.t0, sizeof(struct ItinStepBinT0), 1, file);
				convert_bin_ItinStep(bin_step, step, body[0], system, step_type);
				break;
		case 2:	fread(&bin_step.t2, sizeof(struct ItinStepBinT2), 1, file);
				convert_bin_ItinStep(bin_step, step, NULL, system, step_type);
				break;
		case 3:	fread(&bin_step.t3, sizeof(struct ItinStepBinT3), 1, file);
				convert_bin_ItinStep(bin_step, step, NULL, system, step_type);
				break;
		case 4:	fread(&bin_step.t4, sizeof(struct ItinStepBinT4), 1, file);
				convert_bin_ItinStep(bin_step, step, NULL, system, step_type);
				break;
		case 5:	fread(&bin_step.t5, sizeof(struct ItinStepBinT5), 1, file);
				convert_bin_ItinStep(bin_step, step, NULL, system, step_type);
				break;
		default: break;
	}

	if(step->num_next_nodes > 1e6) return;	// avoid overflows

	if(step->num_next_nodes > 0)
		step->next = (struct ItinStep **) malloc(step->num_next_nodes * sizeof(struct ItinStep *));
	else step->next = NULL;

	for(int i = 0; i < step->num_next_nodes; i++) {
		step->next[i] = (struct ItinStep *) calloc(1, sizeof(struct ItinStep));
		step->next[i]->prev = step;
		load_step_from_bfile(step->next[i], file, body == NULL ? NULL : body + 1, system, step_type);
	}
}
