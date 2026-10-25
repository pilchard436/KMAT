#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <gtk/gtk.h>

#include "file_io_internal.h"


BinTypes all_itins_file_types[] = {{10, 4, 5, 3, 5}};
const int num_all_itins_file_types = 1;
const BinTypes *current_itins_file_types = &(all_itins_file_types[0]);

BinTypes all_itin_file_types[] = {{3, 0, 5, 3, 3}, {2, 0, 5, 3, 0}, {1, 0, 3, 2, 0}, {0, 0, 2, 2, 0}};
const int num_all_itin_file_types = 4;
const BinTypes *current_itin_file_types = &(all_itin_file_types[0]);

void store_itin_search_options(FILE *file, const ItinSearchOptions *options) {
	ItinSearchOptionsBin bin = {0};
	if(options != NULL) {
		bin = (ItinSearchOptionsBin) {
			.known = !!options->known,
			.allow_dsm = !!options->allow_dsm,
			.moon_aware_flybys = !!options->moon_aware_flybys,
			.moon_assist_arrival = !!options->moon_assist_arrival,
			.allow_resonant = !!options->allow_resonant,
			.last_transfer_type = options->last_transfer_type,
			.use_custom_starting_orbit = !!options->use_custom_starting_orbit,
			.starting_orbit_body_id = options->starting_orbit_body_id,
			.starting_orbit = options->starting_orbit
		};
	}
	fwrite(&bin, sizeof(bin), 1, file);
}

int load_itin_search_options(FILE *file, ItinSearchOptions *options) {
	ItinSearchOptionsBin bin;
	if(fread(&bin, sizeof(bin), 1, file) != 1) return 0;
	if(options != NULL) {
		*options = (ItinSearchOptions) {
			.known = !!bin.known,
			.allow_dsm = !!bin.allow_dsm,
			.moon_aware_flybys = !!bin.moon_aware_flybys,
			.moon_assist_arrival = !!bin.moon_assist_arrival,
			.allow_resonant = !!bin.allow_resonant,
			.last_transfer_type = bin.last_transfer_type,
			.use_custom_starting_orbit = !!bin.use_custom_starting_orbit,
			.starting_orbit_body_id = bin.starting_orbit_body_id,
			.starting_orbit = bin.starting_orbit
		};
	}
	return 1;
}

int get_current_bin_file_type() {
	return current_itins_file_types->file_type;
}

BinTypes get_itins_file_bin_types_from_file_type(int file_type) {
	for(int i = 0; i < num_all_itins_file_types; i++) {
		if(file_type == all_itins_file_types[i].file_type) return all_itins_file_types[i];
	}
	return (BinTypes) {-1};
}

BinTypes get_itin_file_bin_types_from_file_type(int file_type) {
	for(int i = 0; i < num_all_itin_file_types; i++) {
		if(file_type == all_itin_file_types[i].file_type) return all_itin_file_types[i];
	}
	return (BinTypes) {-1};
}


void create_directory_if_not_exists(const char *path) {
	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		GError *error = NULL;
		if (g_mkdir_with_parents(path, 0755) == -1) {
			g_warning("Failed to create directory: %s", error->message);
			g_error_free(error);
		}
	}
}


void write_csv(char fields[], double data[]) {
    
    time_t currtime;
    time(&currtime);
    struct tm now = *gmtime(&currtime);
    char filename[19];  // 14 for date + 4 for .csv + 1 for string terminator
    sprintf(filename, "%04d%02d%02d%02d%02d%02d.csv", now.tm_year+1900, now.tm_mon, now.tm_mday,now.tm_hour, now.tm_min, now.tm_sec);

    // -------------------

    int n_fields = amt_of_fields(fields);

    FILE *file;
    file = fopen(filename,"w");

    fprintf(file,"%s\n", fields);

    for(int i = 1; i < data[0]; i+=n_fields) {   // data[0] amount of data points in data
        for(int j = 0; j < n_fields-1; j++) fprintf(file, "%f,",data[i+j]);
        fprintf(file, "%f\n", data[i+n_fields-1]);
    }

    fclose(file);
}

int amt_of_fields(char *fields) {
    int c = 1;
    int i = 0;
    do {
        if(fields[i] == ',') c++;
        i++;
    } while(fields[i] != 0);    // 0 = NULL Terminator for strings
    return c;
}
