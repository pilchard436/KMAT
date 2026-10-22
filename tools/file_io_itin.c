#include "file_io_internal.h"
#include <stdlib.h>
#include <string.h>

void store_single_itinerary_in_bfile(struct ItinStep *itin, CelestSystem *system, char *filepath) {
	ItinSearchOptions search_options = {.starting_orbit_body_id = -1, .last_transfer_type = -1};
	store_single_itinerary_in_bfile_with_options(itin, system, filepath, search_options);
}

void store_single_itinerary_in_bfile_with_options(struct ItinStep *itin, CelestSystem *system,
		char *filepath, ItinSearchOptions search_options) {
	if(itin == NULL || system == NULL) return;
	BinTypes bin_types = *current_itin_file_types;

	int num_nodes = get_num_of_itin_layers(itin);

	// Check if the string ends with ".itin"
	if (strlen(filepath) >= 5 && strcmp(filepath + strlen(filepath) - 5, ".itin") != 0) {
		// If not, append ".itin" to the string
		strcat(filepath, ".itin");
	}


	printf("Storing Itinerary: %s\n", filepath);

	FILE *file;
	file = fopen(filepath,"wb");
	if(file == NULL) {
		fprintf(stderr, "Failed to open itinerary file for writing: %s\n", filepath);
		return;
	}
	
	int file_start = -1;	// num_nodes cannot get negative, therefore (to keep backwards-compatible) set -1 at start to show that next number will be version
	int file_type = bin_types.file_type;
	fwrite(&file_start, sizeof(int), 1, file);
	fwrite(&file_type, sizeof(int), 1, file);

	fwrite(&num_nodes, sizeof(int), 1, file);
	if(file_type == 3) store_itin_search_options(file, &search_options);

	store_celestial_system_in_bfile(system, file, bin_types);

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
		union ItinStepBin bin_step = convert_ItinStep_bin(ptr, system, bin_types.itin_step_type);
		fwrite(&bin_step.t0, sizeof(struct ItinStepBinT0), 1, file);
		if(ptr->next != NULL) ptr = ptr->next[0];
		else break;
	}

	int failed = ferror(file);
	if(fclose(file) != 0) failed = 1;
	if(failed) {
		fprintf(stderr, "Failed to write itinerary file: %s\n", filepath);
	}
}

struct ItinLoadFileResults load_single_itinerary_from_bfile(char *filepath) {
	int num_nodes;
	CelestSystem *system;

	printf("Loading Itinerary: %s\n", filepath);

	FILE *file;
	file = fopen(filepath,"rb");
	if(file == NULL) {
		fprintf(stderr, "Failed to open itinerary file: %s\n", filepath);
		return (struct ItinLoadFileResults) {.itin = NULL, .system = NULL};
	}
	
	int buf;
	fread(&buf, sizeof(int), 1, file);
	int file_type = 0;
	
	if(buf == -1) {
		fread(&file_type, sizeof(int), 1, file);
		fread(&num_nodes, sizeof(int), 1, file);
	} else num_nodes = buf;

	ItinSearchOptions search_options = {.starting_orbit_body_id = -1, .last_transfer_type = -1};
	if(file_type == 3 && !load_itin_search_options(file, &search_options)) {
		fprintf(stderr, "Itinerary file has incomplete search options: %s\n", filepath);
		fclose(file);
		return (struct ItinLoadFileResults) {.itin = NULL, .system = NULL};
	}
	
	BinTypes bin_types = get_itin_file_bin_types_from_file_type(file_type);
	if(bin_types.file_type < 0 || num_nodes <= 0) {
		fprintf(stderr, "Itinerary file has an unsupported format: %s\n", filepath);
		fclose(file);
		return (struct ItinLoadFileResults) {.itin = NULL, .system = NULL};
	}
	
	system = load_celestial_system_from_bfile(file, bin_types);
	if(system == NULL) {
		fclose(file);
		return (struct ItinLoadFileResults) {.itin = NULL, .system = NULL};
	}

	int *bodies_id = (int*) malloc(num_nodes * sizeof(int));
	fread(bodies_id, sizeof(int), num_nodes, file);

	fread(&buf, sizeof(int), 1, file);

	if(buf != -1) {
		printf("Problems reading itinerary file (Body list or header wrong)\n");
		fclose(file);
		free(bodies_id);
		free_celestial_system(system);
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
	return (struct ItinLoadFileResults) {
		.itin = itin,
		.system = system,
		.search_options = search_options
	};
}
