#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <gtk/gtk.h>
#include "celestial_systems.h"
#include "tools/file_io.h"

Body *sun;
Body *earth;

Body * all_celestial_bodies[20];

CelestSystem **available_systems;

int num_available_systems = 0;


void init_available_systems(const char *directory) {
	available_systems = init_available_systems_from_path(directory, &num_available_systems);
}

int get_num_available_systems() {return num_available_systems;}
CelestSystem ** get_available_systems() {return available_systems;}

static int same_system_value(double left, double right) {
    return isfinite(left) && isfinite(right) && fabs(left - right) <= 1e-8 * fmax(1.0, fmax(fabs(left), fabs(right)));
}

static Body *find_direct_body(CelestSystem *system, const char *name) {
    for(int i = 0; system != NULL && i < system->num_bodies; i++)
        if(strcmp(system->bodies[i]->name, name) == 0) return system->bodies[i];
    return NULL;
}

static int bodies_match(const Body *left, const Body *right) {
    if(left == NULL || right == NULL || strcmp(left->name, right->name) != 0 ||
        !same_system_value(left->mu, right->mu) || !same_system_value(left->radius, right->radius) ||
        !same_system_value(left->rotation_period, right->rotation_period) ||
        !same_system_value(left->atmo_alt, right->atmo_alt) || ((left->orbit.cb == NULL) != (right->orbit.cb == NULL))) return 0;
    if((left->system == NULL) != (right->system == NULL)) return 0;
    int left_count = left->system != NULL ? left->system->num_bodies : 0;
    int right_count = right->system != NULL ? right->system->num_bodies : 0;
    if(left_count != right_count) return 0;
    if(left->orbit.cb != NULL &&
        (!same_system_value(left->orbit.a, right->orbit.a) || !same_system_value(left->orbit.e, right->orbit.e) ||
        !same_system_value(left->orbit.i, right->orbit.i) || !same_system_value(left->orbit.raan, right->orbit.raan) ||
        !same_system_value(left->orbit.arg_peri, right->orbit.arg_peri) || !same_system_value(left->orbit.ta, right->orbit.ta)))
        return 0;
    for(int i = 0; i < left_count; i++) {
        Body *right_child = find_direct_body(right->system, left->system->bodies[i]->name);
        if(!bodies_match(left->system->bodies[i], right_child)) return 0;
    }
    return 1;
}

int celestial_systems_equivalent(const CelestSystem *left, const CelestSystem *right) {
    return left != NULL && right != NULL && left->cb != NULL && right->cb != NULL &&
        left->num_bodies == right->num_bodies && bodies_match(left->cb, right->cb);
}

int find_equivalent_available_system(const CelestSystem *system) {
    for(int i = 0; system != NULL && i < num_available_systems; i++)
        if(celestial_systems_equivalent(available_systems[i], system)) return i;
    return -1;
}

int is_available_system(CelestSystem *system) {
	if(system == NULL) return 0;
	for(int i = 0; i < num_available_systems; i++) {
		if(system == get_available_systems()[i]) return 1;
	}
	return 0;
}

CelestSystem * get_subsystem_from_system_and_id_rec(CelestSystem *system, int *id) {
	if(*id == 0) return system;
	(*id)--;
	for(int i = 0; i < system->num_bodies; i++) {
		if(system->bodies[i]->system != NULL) {
			CelestSystem *subsystem = get_subsystem_from_system_and_id_rec(system->bodies[i]->system, id);
			if(subsystem != NULL) return subsystem;
		}
	}
	return NULL;
}

CelestSystem * get_subsystem_from_system_and_id(CelestSystem *system, int id) {
	return get_subsystem_from_system_and_id_rec(system, &id);
}

CelestSystem * get_system_by_name(char *name) {
	for(int i = 0; i < get_num_available_systems(); i++) {
		if(strcmp(get_available_systems()[i]->name, name) == 0) return get_available_systems()[i];
	}
	
	return NULL;
}

void free_all_celestial_systems() {
	free_celestial_systems(available_systems, num_available_systems);
}




// LEGACY CODE (to be removed in future) --------------------------------------------

struct Body ** all_celest_bodies() {
    all_celestial_bodies[0] = SUN();
    all_celestial_bodies[3] = EARTH();
    return all_celestial_bodies;
}

void init_SUN() {
    sun = (struct Body*)malloc(sizeof(struct Body));
    strcpy(sun->name, "SUN");
	set_body_color(sun, 1.0, 1.0, 0.3);
    sun->id = 10;
    sun->mu = 1327124.40042e14;
    sun->radius = 695700e3;
    sun->rotation_period = 2192832;
    sun->sl_atmo_p = 0;
    sun->scale_height = 0;
    sun->atmo_alt = 0;
	sun->orbit.cb = NULL;
    // sun's orbit not declared as it should not be used and can't give any meaningful information
}

void init_EARTH() {
    earth = (struct Body*)malloc(sizeof(struct Body));
    strcpy(earth->name, "EARTH");
	set_body_color(earth, 0.2, 0.2, 1.0);
    earth->id = 3;
    earth->mu = 3.986004418e14;
    earth->radius = 6371e3;
    earth->rotation_period = 86164.098903691;
    earth->sl_atmo_p = 101325;
    earth->scale_height = 8500;
    earth->atmo_alt = 140e3;
    earth->orbit = constr_orbit_from_elements(
        /*  a  */ 149.5978872e+9,
        /*  e  */ 0.01671022,
        /*  i  */ deg2rad(0.00005),
        /* raan */ deg2rad(-11.26064),
        /*  w  */ deg2rad((102.94719-(-11.26064))),
		/*  θ  */ deg2rad(0),
        /*pbody*/ SUN()
    );

	earth->ephem = NULL;
//	get_body_ephems(earth, SUN());
}

void init_celestial_bodies() {
    init_SUN();
    init_EARTH();
}

struct Body * SUN() {
    return sun;
}

struct Body * EARTH() {
    return earth;
}