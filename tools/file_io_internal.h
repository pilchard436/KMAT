#ifndef KMAT_FILE_IO_INTERNAL_H
#define KMAT_FILE_IO_INTERNAL_H

/* Binary layouts and helpers shared by the file_io modules. */

#include "file_io.h"
#include "orbitlib_fileio.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct BinTypes {
	int file_type, header_type, body_type, system_type, itin_step_type;
} BinTypes;

struct CalcDataBinT2 {
	double jd_min_dep;
	double jd_max_dep;
	double jd_max_arr;
	double max_duration;
	double step_dep_date;
	int num_deps_per_date;
	int max_num_waiting_orbits;
	struct Dv_Filter dv_filter;
	struct Calc_Acc calc_acc;
};

struct ItinStepBinHeaderT4 {
	int num_nodes, num_deps, num_itins;
	struct CalcDataBinT2 calc_data;
};

typedef struct ItinSearchOptionsBin {
	int known;
	int allow_dsm;
	int moon_aware_flybys;
	int moon_assist_arrival;
	int allow_resonant;
	int last_transfer_type;
	int use_custom_starting_orbit;
	int starting_orbit_body_id;
	StartingOrbit starting_orbit;
} ItinSearchOptionsBin;

typedef struct ItinArrivalDvHeaderBin {
	double dep_periapsis;
	double arr_periapsis;
	int moon_assist_enabled;
} ItinArrivalDvHeaderBin;

union ItinStepBin {
	struct ItinStepBinValid {
		int *ptr;
	} is_valid;

	struct ItinStepBinT0 {
		Vector3 r;
		Vector3 v_dep, v_arr, v_body;
		double date;
		int num_next_nodes;
	} t0;

	struct ItinStepBinT2 {
		double date;
		Vector3 r;
		Vector3 v_dep, v_arr, v_body;
		int body_id;
		int num_next_nodes;
	} t2;

	struct ItinStepBinT3 {
		struct ItinStepBinT2 node;
		double dv_dep, dv_dsm, dv_flyby, dv_arr_cap, dv_arr_circ;
	} t3;
};

void store_itin_search_options(FILE *file, const ItinSearchOptions *options);
int load_itin_search_options(FILE *file, ItinSearchOptions *options);

extern const BinTypes *current_itins_file_types;
extern const BinTypes *current_itin_file_types;
BinTypes get_itins_file_bin_types_from_file_type(int file_type);
BinTypes get_itin_file_bin_types_from_file_type(int file_type);

/* file_io_system.c */
void store_celestial_system_in_bfile(CelestSystem *system, FILE *file, BinTypes types);
CelestSystem * load_celestial_system_from_bfile(FILE *file, BinTypes types);

/* file_io_steps.c */
union ItinStepBin convert_ItinStep_bin(struct ItinStep *step, CelestSystem *system, int step_type);
void store_step_in_bfile(struct ItinStep *step, CelestSystem *system, FILE *file, int step_type);
void convert_bin_ItinStep(union ItinStepBin bin_step, struct ItinStep *step, struct Body *body, CelestSystem *system, int step_type);
void load_step_from_bfile(struct ItinStep *step, FILE *file, struct Body **body, CelestSystem *system, int step_type);

#endif
