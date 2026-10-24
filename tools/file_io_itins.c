#include "file_io_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

static void restore_arrival_dv_cache_metadata(struct ItinStep *step, double dep_periapsis,
		double arr_periapsis, int moon_assist_enabled) {
	if(step == NULL) return;
	if(step->num_next_nodes == 0) {
		if(step->prev != NULL && step->body != NULL) {
			step->arrival_dv_cache.incoming_excess = subtract_vec3(step->v_arr, step->v_body);
			step->arrival_dv_cache.date = step->date;
			step->arrival_dv_cache.dep_periapsis = dep_periapsis;
			step->arrival_dv_cache.arr_periapsis = arr_periapsis;
			step->arrival_dv_cache.moon_assist_enabled = !!moon_assist_enabled;
			step->arrival_dv_cache.route_values_valid = 1;
			step->arrival_dv_cache.valid = 1;
		}
		return;
	}
	for(int i = 0; i < step->num_next_nodes; i++)
		restore_arrival_dv_cache_metadata(step->next[i], dep_periapsis, arr_periapsis, moon_assist_enabled);
}

static void ensure_route_dv_values(struct ItinStep *step, double dep_periapsis, double arr_periapsis) {
	if(step == NULL) return;
	if(step->num_next_nodes == 0) {
		if(step->prev == NULL || step->body == NULL) return;
		cache_itinerary_arrival_dvs(step, arr_periapsis);
		if(!step->arrival_dv_cache.route_values_valid ||
			fabs(step->arrival_dv_cache.dep_periapsis - dep_periapsis) > 1e-6)
			create_porkchop_point(step, dep_periapsis, arr_periapsis);
		return;
	}
	for(int i = 0; i < step->num_next_nodes; i++)
		ensure_route_dv_values(step->next[i], dep_periapsis, arr_periapsis);
}

static void store_itineraries_in_bfile_internal(struct ItinStep **departures, int num_nodes,
		int num_deps, int num_itins, Itin_Calc_Data calc_data, CelestSystem *system,
		char *filepath, int file_type, struct ItinStep **capture_departures,
		int capture_num_nodes, int capture_num_deps, int capture_num_itins, int both_modes) {
	BinTypes bin_types = get_itins_file_bin_types_from_file_type(file_type);
	if(bin_types.file_type < 0 || file_type != 8) {
		fprintf(stderr, "Unsupported itinerary file type: %d\n", file_type);
		return;
	}
	if(!both_modes) {
		fprintf(stderr, "Itinerary files require both Fly-By and Capture result sets\n");
		return;
	}
	if((num_deps > 0 && departures == NULL) ||
		(both_modes && capture_num_deps > 0 && capture_departures == NULL)) {
		fprintf(stderr, "Cannot write itineraries: a result array is missing\n");
		return;
	}
	if(!both_modes) {
		fprintf(stderr, "Itinerary files require both Fly-By and Capture result sets\n");
		return;
	}
	for(int i = 0; i < num_deps; i++)
		ensure_route_dv_values(departures[i], calc_data.dv_filter.dep_periapsis,
			calc_data.dv_filter.arr_periapsis);
	for(int i = 0; i < capture_num_deps; i++)
		ensure_route_dv_values(capture_departures[i], calc_data.dv_filter.dep_periapsis,
			calc_data.dv_filter.arr_periapsis);
	
	// Check if the string ends with ".itins"
	if (strlen(filepath) >= 6 && strcmp(filepath + strlen(filepath) - 6, ".itins") != 0) {
		// If not, append ".itins" to the string
		strcat(filepath, ".itins");
	}

	union ItinStepBinHeader bin_header;
	int end_of_header_designator = -1;
	FILE *file = fopen(filepath, "wb");
	if(file == NULL) {
		fprintf(stderr, "Failed to open itinerary file for writing: %s\n", filepath);
		return;
	}
	fwrite(&file_type, sizeof(int), 1, file);

	switch(bin_types.header_type) {
		case 3:	bin_header.t3.num_nodes = num_nodes;
				bin_header.t3.num_deps = num_deps;
				bin_header.t3.num_itins = num_itins;
		
				bin_header.t3.calc_data = (struct CalcDataBinT1) {
					.jd_min_dep = calc_data.jd_min_dep,
					.jd_max_dep = calc_data.jd_max_dep,
					.jd_max_arr = calc_data.jd_max_arr,
					.max_duration = calc_data.max_duration,
					.step_dep_date = calc_data.step_dep_date,
					.num_deps_per_date = calc_data.num_deps_per_date,
					.max_num_waiting_orbits = calc_data.max_num_waiting_orbits,
					.dv_filter = calc_data.dv_filter
				};
		
				size_t legacy_step_size = bin_types.itin_step_type == 3 ?
						sizeof(struct ItinStepBinT3) : sizeof(struct ItinStepBinT2);
				printf("Filesize: ~%.3f MB\n", (double) num_nodes*legacy_step_size/1e6);
		
				fwrite(&bin_header.t3, sizeof(struct ItinStepBinHeaderT3), 1, file);
		
				store_celestial_system_in_bfile(system, file, bin_types);
				if(calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
					fwrite(&calc_data.seq_info.to_target.type, sizeof(int), 1, file);
					fwrite(&calc_data.seq_info.to_target.num_flyby_bodies, sizeof(int), 1, file);
					int *body_ids = malloc((calc_data.seq_info.to_target.num_flyby_bodies + 2) * sizeof(int));
					body_ids[0] = get_body_system_id(calc_data.seq_info.to_target.dep_body, system);
					body_ids[1] = get_body_system_id(calc_data.seq_info.to_target.arr_body, system);
					for(int i = 0; i < calc_data.seq_info.to_target.num_flyby_bodies; i++) body_ids[i+2] = get_body_system_id(calc_data.seq_info.to_target.flyby_bodies[i], system);
					fwrite(body_ids, sizeof(int), calc_data.seq_info.to_target.num_flyby_bodies + 2, file);
					free(body_ids);
				} else {
					fwrite(&calc_data.seq_info.spec_seq.type, sizeof(int), 1, file);
					fwrite(&calc_data.seq_info.spec_seq.num_steps, sizeof(int), 1, file);
					int *body_ids = malloc((calc_data.seq_info.spec_seq.num_steps) * sizeof(int));
					for(int i = 0; i < calc_data.seq_info.spec_seq.num_steps; i++) body_ids[i] = get_body_system_id(calc_data.seq_info.spec_seq.bodies[i], system);
					fwrite(body_ids, sizeof(int), calc_data.seq_info.spec_seq.num_steps, file);
					free(body_ids);
				}
				fwrite(&end_of_header_designator, sizeof(int), 1, file);
		
				for(int i = 0; i < num_deps; i++) {
					store_step_in_bfile(departures[i], system, file, bin_types.itin_step_type);
				}
				if(both_modes) {
					fwrite(&capture_num_nodes, sizeof(int), 1, file);
					fwrite(&capture_num_deps, sizeof(int), 1, file);
					fwrite(&capture_num_itins, sizeof(int), 1, file);
					for(int i = 0; i < capture_num_deps; i++)
						store_step_in_bfile(capture_departures[i], system, file, bin_types.itin_step_type);
				}
				break;
		case 4:	bin_header.t4.num_nodes = num_nodes;
			bin_header.t4.num_deps = num_deps;
			bin_header.t4.num_itins = num_itins;
			
			bin_header.t4.calc_data = (struct CalcDataBinT2) {
					.jd_min_dep = calc_data.jd_min_dep,
					.jd_max_dep = calc_data.jd_max_dep,
					.jd_max_arr = calc_data.jd_max_arr,
					.max_duration = calc_data.max_duration,
					.step_dep_date = calc_data.step_dep_date,
					.num_deps_per_date = calc_data.num_deps_per_date,
					.max_num_waiting_orbits = calc_data.max_num_waiting_orbits,
					.dv_filter = calc_data.dv_filter,
					.calc_acc = calc_data.calc_acc
			};
			
			size_t current_step_size = bin_types.itin_step_type == 3 ?
					sizeof(struct ItinStepBinT3) : sizeof(struct ItinStepBinT2);
			printf("Filesize: ~%.3f MB\n", (double) num_nodes*current_step_size/1e6);
			
			fwrite(&bin_header.t4, sizeof(struct ItinStepBinHeaderT4), 1, file);
			
			store_celestial_system_in_bfile(system, file, bin_types);
			if(calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
				fwrite(&calc_data.seq_info.to_target.type, sizeof(int), 1, file);
				fwrite(&calc_data.seq_info.to_target.num_flyby_bodies, sizeof(int), 1, file);
				int *body_ids = malloc((calc_data.seq_info.to_target.num_flyby_bodies + 2) * sizeof(int));
				body_ids[0] = get_body_system_id(calc_data.seq_info.to_target.dep_body, system);
				body_ids[1] = get_body_system_id(calc_data.seq_info.to_target.arr_body, system);
				for(int i = 0; i < calc_data.seq_info.to_target.num_flyby_bodies; i++) body_ids[i+2] = get_body_system_id(calc_data.seq_info.to_target.flyby_bodies[i], system);
				fwrite(body_ids, sizeof(int), calc_data.seq_info.to_target.num_flyby_bodies + 2, file);
				free(body_ids);
			} else {
				fwrite(&calc_data.seq_info.spec_seq.type, sizeof(int), 1, file);
				fwrite(&calc_data.seq_info.spec_seq.num_steps, sizeof(int), 1, file);
				int *body_ids = malloc((calc_data.seq_info.spec_seq.num_steps) * sizeof(int));
				for(int i = 0; i < calc_data.seq_info.spec_seq.num_steps; i++) body_ids[i] = get_body_system_id(calc_data.seq_info.spec_seq.bodies[i], system);
				fwrite(body_ids, sizeof(int), calc_data.seq_info.spec_seq.num_steps, file);
				free(body_ids);
			}
			if(file_type == 8) store_itin_search_options(file, &calc_data.search_options);

			fwrite(&end_of_header_designator, sizeof(int), 1, file);
			
			for(int i = 0; i < num_deps; i++) {
				store_step_in_bfile(departures[i], system, file, bin_types.itin_step_type);
			}
			if(both_modes) {
				fwrite(&capture_num_nodes, sizeof(int), 1, file);
				fwrite(&capture_num_deps, sizeof(int), 1, file);
				fwrite(&capture_num_itins, sizeof(int), 1, file);
				for(int i = 0; i < capture_num_deps; i++)
					store_step_in_bfile(capture_departures[i], system, file, bin_types.itin_step_type);
			}
			break;
		default: bin_header.is_valid.ptr = NULL;
	}
	// ---------------------------------------------------

	fclose(file);
}

void store_itineraries_in_bfile_both_modes(Itin_Calc_Results results, Itin_Calc_Data calc_data,
		CelestSystem *system, char *filepath) {
	calc_data.dv_filter.last_transfer_type = TF_BOTH;
	calc_data.search_options.last_transfer_type = TF_BOTH;
	store_itineraries_in_bfile_internal(results.departures, results.num_nodes, results.num_deps,
		results.num_itins, calc_data, system, filepath, 8, results.capture_departures,
		results.capture_num_nodes, results.capture_num_deps, results.capture_num_itins, 1);
}

ItinStepBinHeaderData get_itins_bfile_header(FILE *file) {
	int file_type;
	BinTypes bin_types;
	union ItinStepBinHeader bin_header;
	fread(&file_type, sizeof(int), 1, file);
	ItinStepBinHeaderData header_data = {.file_type = file_type, .num_deps = -1, .calc_data.seq_info.to_target.type = -1};
	
	bin_types = get_itins_file_bin_types_from_file_type(file_type);
	
	int buf;
	switch(bin_types.header_type) {
		case 2:	fread(&bin_header.t2, sizeof(struct ItinStepBinHeaderT2), 1, file);
				// avoid overflows
				if(bin_header.t2.num_deps > 1e10) return header_data;
				header_data.num_deps = bin_header.t2.num_deps;
				header_data.num_nodes = bin_header.t2.num_nodes;
		
				fread(&buf, sizeof(int), 1, file);
		
				if(buf != -1) {
					printf("Problems reading itinerary file (Body list or header wrong)\n");
					fclose(file);
					header_data.num_deps = -1;
					return header_data;
				}
		
				header_data.system = load_celestial_system_from_bfile(file, bin_types);
				break;
		case 3: fread(&bin_header.t3, sizeof(struct ItinStepBinHeaderT3), 1, file);
				// avoid overflows
				if(bin_header.t3.num_deps > 1e10)  return header_data;
				header_data.num_deps = bin_header.t3.num_deps;
				header_data.num_nodes = bin_header.t3.num_nodes;
				header_data.num_itins = bin_header.t3.num_itins;
				header_data.calc_data.jd_min_dep = bin_header.t3.calc_data.jd_min_dep;
				header_data.calc_data.jd_max_dep = bin_header.t3.calc_data.jd_max_dep;
				header_data.calc_data.jd_max_arr = bin_header.t3.calc_data.jd_max_arr;
				header_data.calc_data.max_duration = bin_header.t3.calc_data.max_duration;
				header_data.calc_data.dv_filter = bin_header.t3.calc_data.dv_filter;
				header_data.calc_data.max_num_waiting_orbits = bin_header.t3.calc_data.max_num_waiting_orbits;
				header_data.calc_data.step_dep_date = bin_header.t3.calc_data.step_dep_date;
				header_data.calc_data.num_deps_per_date = bin_header.t3.calc_data.num_deps_per_date;
		
				header_data.system = load_celestial_system_from_bfile(file, bin_types);
		
				int calc_type3;
				fread(&calc_type3, sizeof(int), 1, file);
				header_data.calc_data.seq_info.to_target.type = calc_type3;
		
				if(calc_type3 == ITIN_SEQ_INFO_TO_TARGET) {
					int num_flyby_bodies;
					fread(&num_flyby_bodies, sizeof(int), 1, file);
					header_data.calc_data.seq_info.to_target.num_flyby_bodies = num_flyby_bodies;
					int *body_ids = malloc((num_flyby_bodies + 2) * sizeof(int));
					fread(body_ids, sizeof(int), num_flyby_bodies + 2, file);
					header_data.calc_data.seq_info.to_target.flyby_bodies = malloc((num_flyby_bodies) * sizeof(struct Body*));
					header_data.calc_data.seq_info.to_target.dep_body = header_data.system->bodies[body_ids[0]];
					header_data.calc_data.seq_info.to_target.arr_body = header_data.system->bodies[body_ids[1]];
					for(int i = 0; i < num_flyby_bodies; i++) header_data.calc_data.seq_info.to_target.flyby_bodies[i] = header_data.system->bodies[body_ids[i+2]];
					free(body_ids);
					header_data.calc_data.seq_info.to_target.system = header_data.system;
				} else if(calc_type3 == ITIN_SEQ_INFO_SPEC_SEQ){
					int num_steps;
					fread(&num_steps, sizeof(int), 1, file);
					header_data.calc_data.seq_info.spec_seq.num_steps = num_steps;
					int *body_ids = malloc(num_steps * sizeof(int));
					fread(body_ids, sizeof(int), num_steps, file);
					header_data.calc_data.seq_info.spec_seq.bodies = malloc((num_steps) * sizeof(struct Body*));
					for(int i = 0; i < num_steps; i++) header_data.calc_data.seq_info.spec_seq.bodies[i] = header_data.system->bodies[body_ids[i]];
					free(body_ids);
					header_data.calc_data.seq_info.spec_seq.system = header_data.system;
				} else {
					free_celestial_system(header_data.system);
					header_data.num_deps = -1;
					return header_data;
				}

				fread(&buf, sizeof(int), 1, file);
		
				if(buf != -1) {
					printf("Problems reading itinerary file (Body list or header wrong)\n");
					fclose(file);
					free_celestial_system(header_data.system);
					if(header_data.calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) free(header_data.calc_data.seq_info.to_target.flyby_bodies);
					if(header_data.calc_data.seq_info.spec_seq.type == ITIN_SEQ_INFO_SPEC_SEQ) free(header_data.calc_data.seq_info.spec_seq.bodies);
					header_data.num_deps = -1;
					return header_data;
				}
				break;
		case 4: fread(&bin_header.t4, sizeof(struct ItinStepBinHeaderT4), 1, file);
			// avoid overflows
			if(bin_header.t4.num_deps > 1e10)  return header_data;
			header_data.num_deps = bin_header.t4.num_deps;
			header_data.num_nodes = bin_header.t4.num_nodes;
			header_data.num_itins = bin_header.t4.num_itins;
			header_data.calc_data.jd_min_dep = bin_header.t4.calc_data.jd_min_dep;
			header_data.calc_data.jd_max_dep = bin_header.t4.calc_data.jd_max_dep;
			header_data.calc_data.jd_max_arr = bin_header.t4.calc_data.jd_max_arr;
			header_data.calc_data.max_duration = bin_header.t4.calc_data.max_duration;
			header_data.calc_data.dv_filter = bin_header.t4.calc_data.dv_filter;
			header_data.calc_data.calc_acc = bin_header.t4.calc_data.calc_acc;
			header_data.calc_data.max_num_waiting_orbits = bin_header.t4.calc_data.max_num_waiting_orbits;
			header_data.calc_data.step_dep_date = bin_header.t4.calc_data.step_dep_date;
			header_data.calc_data.num_deps_per_date = bin_header.t4.calc_data.num_deps_per_date;
			
			header_data.system = load_celestial_system_from_bfile(file, bin_types);
			
			int calc_type4;
			fread(&calc_type4, sizeof(int), 1, file);
			header_data.calc_data.seq_info.to_target.type = calc_type4;
			
			if(calc_type4 == ITIN_SEQ_INFO_TO_TARGET) {
				int num_flyby_bodies;
				fread(&num_flyby_bodies, sizeof(int), 1, file);
				header_data.calc_data.seq_info.to_target.num_flyby_bodies = num_flyby_bodies;
				int *body_ids = malloc((num_flyby_bodies + 2) * sizeof(int));
				fread(body_ids, sizeof(int), num_flyby_bodies + 2, file);
				header_data.calc_data.seq_info.to_target.flyby_bodies = malloc((num_flyby_bodies) * sizeof(struct Body*));
				header_data.calc_data.seq_info.to_target.dep_body = header_data.system->bodies[body_ids[0]];
				header_data.calc_data.seq_info.to_target.arr_body = header_data.system->bodies[body_ids[1]];
				for(int i = 0; i < num_flyby_bodies; i++) header_data.calc_data.seq_info.to_target.flyby_bodies[i] = header_data.system->bodies[body_ids[i+2]];
				free(body_ids);
				header_data.calc_data.seq_info.to_target.system = header_data.system;
			} else if(calc_type4 == ITIN_SEQ_INFO_SPEC_SEQ){
				int num_steps;
				fread(&num_steps, sizeof(int), 1, file);
				header_data.calc_data.seq_info.spec_seq.num_steps = num_steps;
				int *body_ids = malloc(num_steps * sizeof(int));
				fread(body_ids, sizeof(int), num_steps, file);
				header_data.calc_data.seq_info.spec_seq.bodies = malloc((num_steps) * sizeof(struct Body*));
				for(int i = 0; i < num_steps; i++) header_data.calc_data.seq_info.spec_seq.bodies[i] = header_data.system->bodies[body_ids[i]];
				free(body_ids);
				header_data.calc_data.seq_info.spec_seq.system = header_data.system;
			} else {
				free_celestial_system(header_data.system);
				header_data.num_deps = -1;
				return header_data;
			}

			if(file_type == 8) {
				if(!load_itin_search_options(file, &header_data.calc_data.search_options)) {
					free_celestial_system(header_data.system);
					if(calc_type4 == ITIN_SEQ_INFO_TO_TARGET)
						free(header_data.calc_data.seq_info.to_target.flyby_bodies);
					else
						free(header_data.calc_data.seq_info.spec_seq.bodies);
					fclose(file);
					header_data.num_deps = -1;
					return header_data;
				}
				header_data.calc_data.allow_dsm = header_data.calc_data.search_options.allow_dsm;
				header_data.calc_data.moon_aware_flybys = header_data.calc_data.search_options.moon_aware_flybys;
				header_data.calc_data.moon_assist_arrival = header_data.calc_data.search_options.moon_assist_arrival;
				if(calc_type4 == ITIN_SEQ_INFO_TO_TARGET)
					header_data.calc_data.seq_info.to_target.allow_resonant =
						header_data.calc_data.search_options.allow_resonant;
				header_data.calc_data.dv_filter.last_transfer_type =
					header_data.calc_data.search_options.last_transfer_type;
			}
			
			fread(&buf, sizeof(int), 1, file);
			
			if(buf != -1) {
				printf("Problems reading itinerary file (Body list or header wrong)\n");
				fclose(file);
				free_celestial_system(header_data.system);
				if(header_data.calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) free(header_data.calc_data.seq_info.to_target.flyby_bodies);
				if(header_data.calc_data.seq_info.spec_seq.type == ITIN_SEQ_INFO_SPEC_SEQ) free(header_data.calc_data.seq_info.spec_seq.bodies);
				header_data.num_deps = -1;
				return header_data;
			}
			break;
	}
	return header_data;
}

struct ItinsLoadFileResults load_itineraries_from_bfile(char *filepath) {
	struct ItinStep **departures = NULL;

	FILE *file;
	file = fopen(filepath,"rb");
	if(file == NULL) {
		fprintf(stderr, "Failed to open itineraries file: %s\n", filepath);
		return (struct ItinsLoadFileResults){0};
	}

	ItinStepBinHeaderData header_data = get_itins_bfile_header(file);
	BinTypes bin_types = get_itins_file_bin_types_from_file_type(header_data.file_type);

	char header_string[1024];
	print_header_data_to_string(header_data, header_string, sizeof(header_string), DATE_ISO);
	printf("\n--\n%s--\n", header_string);

	if(header_data.num_deps < 0) {
		fclose(file);
		return (struct ItinsLoadFileResults){.header = header_data};
	}

	departures = calloc((size_t)(header_data.num_deps > 0 ? header_data.num_deps : 1), sizeof(*departures));
	if(departures == NULL) {
		fclose(file);
		return (struct ItinsLoadFileResults){.header = header_data};
	}

	for(int i = 0; i < header_data.num_deps; i++) {
		departures[i] = (struct ItinStep *) calloc(1, sizeof(struct ItinStep));
		departures[i]->prev = NULL;
		load_step_from_bfile(departures[i], file, NULL, header_data.system, bin_types.itin_step_type);
	}
	struct ItinStep **capture_departures = NULL;
	int capture_num_deps = 0, capture_num_nodes = 0, capture_num_itins = 0;
	if(header_data.file_type == 8) {
		if(fread(&capture_num_nodes, sizeof(int), 1, file) != 1 ||
			fread(&capture_num_deps, sizeof(int), 1, file) != 1 ||
			fread(&capture_num_itins, sizeof(int), 1, file) != 1 ||
			capture_num_deps < 0 || capture_num_nodes < 0 || capture_num_itins < 0) {
			fprintf(stderr, "Dual-mode itinerary file has an invalid capture section: %s\n", filepath);
			for(int i = 0; i < header_data.num_deps; i++) free_itinerary(departures[i]);
			free(departures);
			fclose(file);
			return (struct ItinsLoadFileResults){.header = header_data};
		}
		capture_departures = calloc((size_t)(capture_num_deps > 0 ? capture_num_deps : 1), sizeof(*capture_departures));
		if(capture_departures == NULL) {
			fprintf(stderr, "Could not allocate capture itineraries while loading: %s\n", filepath);
			for(int i = 0; i < header_data.num_deps; i++) free_itinerary(departures[i]);
			free(departures);
			fclose(file);
			return (struct ItinsLoadFileResults){.header = header_data};
		}
		for(int i = 0; i < capture_num_deps; i++) {
			capture_departures[i] = calloc(1, sizeof(*capture_departures[i]));
			if(capture_departures[i] == NULL) {
				for(int j = 0; j < i; j++) free_itinerary(capture_departures[j]);
				free(capture_departures);
				for(int j = 0; j < header_data.num_deps; j++) free_itinerary(departures[j]);
				free(departures);
				fclose(file);
				return (struct ItinsLoadFileResults){.header = header_data};
			}
			load_step_from_bfile(capture_departures[i], file, NULL, header_data.system, bin_types.itin_step_type);
		}
	}
	if(header_data.file_type == 8) {
		double dep_periapsis = header_data.calc_data.dv_filter.dep_periapsis;
		double arr_periapsis = header_data.calc_data.dv_filter.arr_periapsis;
		int moon_assist_enabled = header_data.calc_data.search_options.known &&
				header_data.calc_data.search_options.moon_assist_arrival;
		for(int i = 0; i < header_data.num_deps; i++)
			restore_arrival_dv_cache_metadata(departures[i], dep_periapsis, arr_periapsis,
				moon_assist_enabled);
		for(int i = 0; i < capture_num_deps; i++)
			restore_arrival_dv_cache_metadata(capture_departures[i], dep_periapsis, arr_periapsis,
				moon_assist_enabled);
	}
	// ---------------------------------------------------

	fclose(file);
	return (struct ItinsLoadFileResults){
		.header = header_data,
		.departures = departures,
		.capture_departures = capture_departures,
		.capture_num_deps = capture_num_deps,
		.capture_num_nodes = capture_num_nodes,
		.capture_num_itins = capture_num_itins
	};
}
