#include "file_io_internal.h"
#include <stdlib.h>
#include <string.h>

void store_single_itinerary_in_bfile(struct ItinStep *itin, CelestSystem *system, char *filepath) {
	if(itin == NULL) return;

	int num_nodes = get_num_of_itin_layers(itin);

	// Check if the string ends with ".itin"
	if (strlen(filepath) >= 5 && strcmp(filepath + strlen(filepath) - 5, ".itin") != 0) {
		// If not, append ".itin" to the string
		strcat(filepath, ".itin");
	}


	printf("Storing Itinerary: %s\n", filepath);

	FILE *file;
	file = fopen(filepath,"wb");
	
	int file_start = -1;	// num_nodes cannot get negative, therefore (to keep backwards-compatible) set -1 at start to show that next number will be version
	fwrite(&file_start, sizeof(int), 1, file);
	int file_type = current_itin_file_types->file_type;
	fwrite(&file_type, sizeof(int), 1, file);

	fwrite(&num_nodes, sizeof(int), 1, file);

	store_celestial_system_in_bfile(system, file, *current_itin_file_types);

	struct ItinStep *ptr = itin;

	// same algorithm as layer counter (part of header)
	while(ptr != NULL) {
		int body_id = (ptr->body != NULL) ? get_body_system_id(ptr->body,system) : -1;
		fwrite(&body_id, sizeof(int), 1, file);
		if(ptr->next != NULL) ptr = ptr->next[0];
		else break;
	}

	int end_of_bodies_designator = -1;
	fwrite(&end_of_bodies_designator, sizeof(int), 1, file);

	ptr = itin;
	while(ptr != NULL) {
		union ItinStepBin bin_step = convert_ItinStep_bin(ptr, system, current_itin_file_types->itin_step_type);
		fwrite(&bin_step.t0, sizeof(struct ItinStepBinT0), 1, file);
		if(ptr->next != NULL) ptr = ptr->next[0];
		else break;
	}

	fclose(file);
}

struct ItinLoadFileResults load_single_itinerary_from_bfile(char *filepath) {
	int num_nodes;
	CelestSystem *system;

	printf("Loading Itinerary: %s\n", filepath);

	FILE *file;
	file = fopen(filepath,"rb");
	
	int buf;
	fread(&buf, sizeof(int), 1, file);
	int file_type = 0;
	
	if(buf == -1) {
		fread(&file_type, sizeof(int), 1, file);
		fread(&num_nodes, sizeof(int), 1, file);
	} else num_nodes = buf;
	
	BinTypes bin_types = get_itin_file_bin_types_from_file_type(file_type);
	
	system = load_celestial_system_from_bfile(file, bin_types);

	int *bodies_id = (int*) malloc(num_nodes * sizeof(int));
	fread(bodies_id, sizeof(int), num_nodes, file);

	fread(&buf, sizeof(int), 1, file);

	if(buf != -1) {
		printf("Problems reading itinerary file (Body list or header wrong)\n");
		fclose(file);
		return (struct ItinLoadFileResults) {.itin = NULL, .system = NULL};
	}

	struct ItinStep *itin;
	union ItinStepBin bin_step;
	struct ItinStep *last_step = NULL;

	for(int i = 0; i < num_nodes; i++) {
		itin = (struct ItinStep*) malloc(sizeof(struct ItinStep));
		fread(&bin_step.t0, sizeof(struct ItinStepBinT0), 1, file);
		Body *step_body = bodies_id[i] >= 0 && bodies_id[i] < system->num_bodies ? system->bodies[bodies_id[i]] : NULL;
		convert_bin_ItinStep(bin_step, itin, step_body, system, bin_types.itin_step_type);
		itin->prev = last_step;
		itin->next = NULL;
		if(last_step != NULL) {
			last_step->next = (struct ItinStep**) malloc(sizeof(struct ItinStep*));
			last_step->next[0] = itin;
		}
		last_step = itin;
	}

	fclose(file);
	free(bodies_id);
	return (struct ItinLoadFileResults) {.itin = itin, .system = system};
}
