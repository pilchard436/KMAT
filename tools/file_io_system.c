#include "file_io_internal.h"
#include <stdlib.h>
#include <string.h>

union CelestialSystemBin {
	struct CelestialSystemBinValid {
		int *ptr;
	} is_valid;

	struct CelestialSystemBinT2 {
		char name[50];
		int num_bodies;
		enum CelestSystemPropMethod calc_method;
		double ut0;
	} t2;
};

union CelestialBodyBin {
	struct CelestialBodyBinValid {
		int *ptr;
	} is_valid;

	struct CelestialBodyBinT2 {
		char name[32];
		double color[3];
		int id;  // ID given by JPL's Horizon API
		double mu;
		double radius;
		double atmo_alt;
		double a, e, incl, raan, arg_peri, theta;
	} t2;
	
	struct CelestialBodyBinT3 {
		char name[32];
		bool is_home_body;
		double color[3];
		int id;  // ID given by JPL's Horizon API
		double mu;
		double radius;
		double atmo_alt;
		double north_pole_ra, north_pole_decl, rot_ut0;
		double a, e, incl, raan, arg_peri, theta;
	} t3;
	
	struct CelestialBodyBinT4 {
		char name[32];
		bool is_home_body;
		double color[3];
		int id;  // ID given by JPL's Horizon API
		double mu;
		double radius;
		double atmo_alt;
		double scale_height;
		double north_pole_ra, north_pole_decl, rot_ut0;
		double a, e, incl, raan, arg_peri, theta;
	} t4;
};

union CelestialSystemBin convert_celestial_system_bin(CelestSystem *system, int system_bin_type) {
	union CelestialSystemBin bin_system = {0};
	if(system_bin_type == 2) {
		sprintf(bin_system.t2.name, "%s", system->name);
		bin_system.t2.num_bodies = system->num_bodies;
		bin_system.t2.calc_method = system->prop_method;
		bin_system.t2.ut0 = system->ut0;
	}
	return bin_system;
}

CelestSystem * convert_bin_celestial_system(union CelestialSystemBin bin_system, int system_bin_type) {
	CelestSystem *system = new_system();
	if(system_bin_type == 2) {
		sprintf(system->name, "%s", bin_system.t2.name);
		system->num_bodies = bin_system.t2.num_bodies;
		if(system->num_bodies > 1e3) { free(system); return NULL; }    // avoid overflows
		system->prop_method = bin_system.t2.calc_method;
		system->ut0 = bin_system.t2.ut0;
	}
	system->bodies = (Body **) malloc(system->num_bodies*sizeof(Body *));
	return system;
}

union CelestialBodyBin convert_celestial_body_bin(Body *body, CelestSystem *system, int body_bin_type) {
	union CelestialBodyBin bin_body;
	if(body_bin_type == 2) {
		sprintf(bin_body.t2.name, "%s", body->name);
		bin_body.t2.color[0] = body->color[0];
		bin_body.t2.color[1] = body->color[1];
		bin_body.t2.color[2] = body->color[2];
		bin_body.t2.id = body->id;
		bin_body.t2.mu = body->mu;
		bin_body.t2.radius = body->radius;
		bin_body.t2.atmo_alt = body->atmo_alt;
		bin_body.t2.a = body->orbit.a;
		bin_body.t2.e = body->orbit.e;
		bin_body.t2.incl = body->orbit.i;
		bin_body.t2.raan = body->orbit.raan;
		bin_body.t2.arg_peri = body->orbit.arg_peri;
		bin_body.t2.theta = body->orbit.ta;
	} else if(body_bin_type == 3) {
		sprintf(bin_body.t3.name, "%s", body->name);
		bin_body.t3.color[0] = body->color[0];
		bin_body.t3.color[1] = body->color[1];
		bin_body.t3.color[2] = body->color[2];
		bin_body.t3.id = body->id;
		bin_body.t3.mu = body->mu;
		bin_body.t3.radius = body->radius;
		bin_body.t3.atmo_alt = body->atmo_alt;
		bin_body.t3.north_pole_ra = body->north_pole_ra;
		bin_body.t3.north_pole_decl = body->north_pole_decl;
		bin_body.t3.rot_ut0 = body->rot_ut0;
		bin_body.t3.a = body->orbit.a;
		bin_body.t3.e = body->orbit.e;
		bin_body.t3.incl = body->orbit.i;
		bin_body.t3.raan = body->orbit.raan;
		bin_body.t3.arg_peri = body->orbit.arg_peri;
		bin_body.t3.theta = body->orbit.ta;
		bin_body.t3.is_home_body = system->home_body == body;
	} else if(body_bin_type == 4) {
		sprintf(bin_body.t4.name, "%s", body->name);
		bin_body.t4.color[0] = body->color[0];
		bin_body.t4.color[1] = body->color[1];
		bin_body.t4.color[2] = body->color[2];
		bin_body.t4.id = body->id;
		bin_body.t4.mu = body->mu;
		bin_body.t4.radius = body->radius;
		bin_body.t4.atmo_alt = body->atmo_alt;
		bin_body.t4.scale_height = body->scale_height;
		bin_body.t4.north_pole_ra = body->north_pole_ra;
		bin_body.t4.north_pole_decl = body->north_pole_decl;
		bin_body.t4.rot_ut0 = body->rot_ut0;
		bin_body.t4.a = body->orbit.a;
		bin_body.t4.e = body->orbit.e;
		bin_body.t4.incl = body->orbit.i;
		bin_body.t4.raan = body->orbit.raan;
		bin_body.t4.arg_peri = body->orbit.arg_peri;
		bin_body.t4.theta = body->orbit.ta;
		bin_body.t4.is_home_body = system->home_body == body;
	}
	return bin_body;
}

struct Body * convert_bin_celestial_body(union CelestialBodyBin bin_body, CelestSystem *system, int body_bin_type) {
	struct Body *body = new_body();
	if(body_bin_type == 2) {
		sprintf(body->name, "%s", bin_body.t2.name);
		body->color[0] = bin_body.t2.color[0];
		body->color[1] = bin_body.t2.color[1];
		body->color[2] = bin_body.t2.color[2];
		body->id = bin_body.t2.id;
		body->mu = bin_body.t2.mu;
		body->radius = bin_body.t2.radius;
		body->atmo_alt = bin_body.t2.atmo_alt;
		if(system->cb != NULL)
			body->orbit = constr_orbit_from_elements(
					bin_body.t2.a,
					bin_body.t2.e,
					bin_body.t2.incl,
					bin_body.t2.raan,
					bin_body.t2.arg_peri,
					bin_body.t2.theta,
					system->cb);
	} else if(body_bin_type == 3) {
		sprintf(body->name, "%s", bin_body.t3.name);
		body->color[0] = bin_body.t3.color[0];
		body->color[1] = bin_body.t3.color[1];
		body->color[2] = bin_body.t3.color[2];
		body->id = bin_body.t3.id;
		body->mu = bin_body.t3.mu;
		body->radius = bin_body.t3.radius;
		body->atmo_alt = bin_body.t3.atmo_alt;
		body->north_pole_ra = bin_body.t3.north_pole_ra;
		body->north_pole_decl = bin_body.t3.north_pole_decl;
		body->rot_ut0 = bin_body.t3.rot_ut0;
		if(bin_body.t3.is_home_body) system->home_body = body;
		if(system->cb != NULL)
			body->orbit = constr_orbit_from_elements(
					bin_body.t3.a,
					bin_body.t3.e,
					bin_body.t3.incl,
					bin_body.t3.raan,
					bin_body.t3.arg_peri,
					bin_body.t3.theta,
					system->cb);
	} else if(body_bin_type == 4) {
		sprintf(body->name, "%s", bin_body.t4.name);
		body->color[0] = bin_body.t4.color[0];
		body->color[1] = bin_body.t4.color[1];
		body->color[2] = bin_body.t4.color[2];
		body->id = bin_body.t4.id;
		body->mu = bin_body.t4.mu;
		body->radius = bin_body.t4.radius;
		body->atmo_alt = bin_body.t4.atmo_alt;
		body->scale_height = bin_body.t4.scale_height;
		body->north_pole_ra = bin_body.t4.north_pole_ra;
		body->north_pole_decl = bin_body.t4.north_pole_decl;
		body->rot_ut0 = bin_body.t4.rot_ut0;
		if(bin_body.t4.is_home_body) system->home_body = body;
		if(system->cb != NULL)
			body->orbit = constr_orbit_from_elements(
					bin_body.t4.a,
					bin_body.t4.e,
					bin_body.t4.incl,
					bin_body.t4.raan,
					bin_body.t4.arg_peri,
					bin_body.t4.theta,
					system->cb);
	}
	return body;
}

void store_celestial_system_in_bfile(CelestSystem *system, FILE *file, BinTypes types) {
	union CelestialSystemBin system_bin = convert_celestial_system_bin(system, types.system_type);
	switch(types.system_type) {
		case 2: fwrite(&system_bin.t2, sizeof(struct CelestialSystemBinT2), 1, file); break;
	}

	union CelestialBodyBin body_bin = convert_celestial_body_bin(system->cb, system, types.body_type);
	switch(types.body_type) {
		case 2: fwrite(&body_bin.t2, sizeof(struct CelestialBodyBinT2), 1, file); break;
		case 3: fwrite(&body_bin.t3, sizeof(struct CelestialBodyBinT3), 1, file); break;
		case 4: fwrite(&body_bin.t4, sizeof(struct CelestialBodyBinT4), 1, file); break;
	}

	for(int i = 0; i < system->num_bodies; i++) {
		body_bin = convert_celestial_body_bin(system->bodies[i], system, types.body_type);
		switch(types.body_type) {
			case 2: fwrite(&body_bin.t2, sizeof(struct CelestialBodyBinT2), 1, file); break;
			case 3: fwrite(&body_bin.t3, sizeof(struct CelestialBodyBinT3), 1, file); break;
			case 4: fwrite(&body_bin.t4, sizeof(struct CelestialBodyBinT4), 1, file); break;
		}
	}
}

CelestSystem * load_celestial_system_from_bfile(FILE *file, BinTypes types) {
	CelestSystem *system = 0;
	union CelestialSystemBin system_bin;
	switch(types.system_type) {
		case 2: fread(&system_bin.t2, sizeof(struct CelestialSystemBinT2), 1, file); break;
	}
	system = convert_bin_celestial_system(system_bin, types.system_type);
	if(system == NULL) return NULL;

	union CelestialBodyBin body_bin;
	switch(types.body_type) {
		case 2: fread(&body_bin.t2, sizeof(struct CelestialBodyBinT2), 1, file); break;
		case 3: fread(&body_bin.t3, sizeof(struct CelestialBodyBinT3), 1, file); break;
		case 4: fread(&body_bin.t4, sizeof(struct CelestialBodyBinT4), 1, file); break;
	}
	system->cb = convert_bin_celestial_body(body_bin, system, types.body_type);

	for(int i = 0; i < system->num_bodies; i++) {
		switch(types.body_type) {
			case 2: fread(&body_bin.t2, sizeof(struct CelestialBodyBinT2), 1, file); break;
			case 3: fread(&body_bin.t3, sizeof(struct CelestialBodyBinT3), 1, file); break;
			case 4: fread(&body_bin.t4, sizeof(struct CelestialBodyBinT4), 1, file); break;
		}
		system->bodies[i] = convert_bin_celestial_body(body_bin, system, types.body_type);
	}

	if(system->prop_method == EPHEMS) {
		for(int i = 0; i < system->num_bodies; i++) {
			get_body_ephems(system->bodies[i], (Datetime){1950, 1, 1},(Datetime){1950, 1, 1}, (Datetime){0,1}, "../Ephemerides");
		}
	}
	
	parse_and_sort_into_celestial_subsystems(system);

	return system;
}
