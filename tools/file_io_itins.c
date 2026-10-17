#include "file_io_internal.h"
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

void store_itineraries_in_bfile(struct ItinStep **departures, int num_nodes, int num_deps, int num_itins, Itin_Calc_Data calc_data, CelestSystem *system, char *filepath, int file_type) {
	BinTypes bin_types = get_itins_file_bin_types_from_file_type(file_type);
	if(bin_types.file_type < 0) return;
	
	// Check if the string ends with ".itins"
	if (strlen(filepath) >= 6 && strcmp(filepath + strlen(filepath) - 6, ".itins") != 0) {
		// If not, append ".itins" to the string
		strcat(filepath, ".itins");
	}

	union ItinStepBinHeader bin_header;
	int end_of_header_designator = -1;
	FILE *file = fopen(filepath, "wb");
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
		
				printf("Filesize: ~%.3f MB\n", (double) num_nodes*sizeof(struct ItinStepBinT2)/1e6);
		
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
			
			printf("Filesize: ~%.3f MB\n", (double) num_nodes*sizeof(struct ItinStepBinT2)/1e6);
			
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
			
			fwrite(&end_of_header_designator, sizeof(int), 1, file);
			
			for(int i = 0; i < num_deps; i++) {
				store_step_in_bfile(departures[i], system, file, bin_types.itin_step_type);
			}
			break;
		default: bin_header.is_valid.ptr = NULL;
	}
	// ---------------------------------------------------

	fclose(file);
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

	ItinStepBinHeaderData header_data = get_itins_bfile_header(file);
	BinTypes bin_types = get_itins_file_bin_types_from_file_type(header_data.file_type);

	char header_string[1024];
	print_header_data_to_string(header_data, header_string, sizeof(header_string), DATE_ISO);
	printf("\n--\n%s--\n", header_string);

	if(header_data.num_deps < 0) return (struct ItinsLoadFileResults){header_data, NULL};

	departures = (struct ItinStep **) malloc(header_data.num_deps * sizeof(struct ItinStep *));

	for(int i = 0; i < header_data.num_deps; i++) {
		departures[i] = (struct ItinStep *) malloc(sizeof(struct ItinStep));
		departures[i]->prev = NULL;
		load_step_from_bfile(departures[i], file, NULL, header_data.system, bin_types.itin_step_type);
	}
	// ---------------------------------------------------

	fclose(file);
	return (struct ItinsLoadFileResults){header_data, departures};
}
