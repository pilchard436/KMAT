#include "file_io_internal.h"
#include <stdlib.h>
#include <string.h>

void store_single_itinerary_in_bfile(struct ItinStep *itin, CelestSystem *system, char *filepath) {
	ItinSearchOptions search_options = {.starting_orbit_body_id = -1, .last_transfer_type = -1};
	struct ItinStep *arrival = itin;
	while(arrival != NULL && arrival->num_next_nodes > 0 && arrival->next != NULL)
		arrival = arrival->next[0];
	double dep_periapsis = itin != NULL && itin->body != NULL ? itin->body->atmo_alt + 50e3 : 0;
	double arr_periapsis = arrival != NULL && arrival->body != NULL ?
		arrival->body->atmo_alt + 50e3 : 0;
	store_single_itinerary_in_bfile_with_options(itin, system, filepath, search_options,
		dep_periapsis, arr_periapsis);
}

void store_single_itinerary_in_bfile_with_options(struct ItinStep *itin, CelestSystem *system,
		char *filepath, ItinSearchOptions search_options, double dep_periapsis, double arr_periapsis) {
	if(itin == NULL || system == NULL) return;
	BinTypes bin_types = *current_itin_file_types;

	struct ItinStep *arrival = itin;
	while(arrival != NULL && arrival->num_next_nodes > 0 && arrival->next != NULL)
		arrival = arrival->next[0];
	if(arrival == NULL || arrival->body == NULL) {
		fprintf(stderr, "Failed to calculate arrival delta-v values for itinerary file\n");
		return;
	}
	create_porkchop_point(arrival, dep_periapsis, arr_periapsis);
	ItinArrivalDvHeaderBin arrival_dv_header = {
		.dep_periapsis = dep_periapsis,
		.arr_periapsis = arr_periapsis,
		.moon_assist_enabled = arrival->arrival_dv_cache.moon_assist_enabled
	};

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
	if(file_type == 3) {
		store_itin_search_options(file, &search_options);
		fwrite(&arrival_dv_header, sizeof(arrival_dv_header), 1, file);
	}

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
		if(bin_types.itin_step_type == 3)
			fwrite(&bin_step.t3, sizeof(struct ItinStepBinT3), 1, file);
		else
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
	ItinArrivalDvHeaderBin arrival_dv_header = {0};

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
	if(file_type == 3 && fread(&arrival_dv_header, sizeof(arrival_dv_header), 1, file) != 1) {
		fprintf(stderr, "Itinerary file has incomplete arrival delta-v data: %s\n", filepath);
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
		itin = (struct ItinStep*) calloc(1, sizeof(struct ItinStep));
		if(bin_types.itin_step_type == 3)
			fread(&bin_step.t3, sizeof(struct ItinStepBinT3), 1, file);
		else
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
	if(file_type == 3 && last_step != NULL && last_step->body != NULL) {
		last_step->arrival_dv_cache.incoming_excess = subtract_vec3(last_step->v_arr, last_step->v_body);
		last_step->arrival_dv_cache.date = last_step->date;
		last_step->arrival_dv_cache.dep_periapsis = arrival_dv_header.dep_periapsis;
		last_step->arrival_dv_cache.arr_periapsis = arrival_dv_header.arr_periapsis;
		last_step->arrival_dv_cache.moon_assist_enabled = !!arrival_dv_header.moon_assist_enabled;
		last_step->arrival_dv_cache.route_values_valid = 1;
		last_step->arrival_dv_cache.valid = 1;
	}

	fclose(file);
	free(bodies_id);
	return (struct ItinLoadFileResults) {
		.itin = itin,
		.system = system,
		.search_options = search_options,
		.departure_periapsis = arrival_dv_header.dep_periapsis,
		.arrival_periapsis = arrival_dv_header.arr_periapsis,
		.arrival_dv_cache_stored = file_type == 3
	};
}
