#ifndef KMAT_GMAT_INTERNAL_H
#define KMAT_GMAT_INTERNAL_H

/* Tables and helpers shared by the GMAT script writer modules. */

#include "gmat_interface.h"
#include <stdio.h>

extern char *body_name[];
extern char *coord_name[];
extern char *sc_var[];
extern char *sc_name[];
extern char propagator[];
extern char force_model[];
extern char vf13ad_solver[];
extern char solvermode[];
extern char propagator_algo[];
extern int gmat_jd_mod;

/* gmat_interface.c */
void setup_gmat_sc(FILE *filePointer, const char *sc, const char *var_sc, const char *body, const char *coord, int num_tabs, int outgoing);
void print_gmat_section(FILE *filePointer, const char *section);

/* gmat_mission_sequence.c */
void create_gmat_variables(FILE *filePointer, int num_steps);
void set_gmat_initial_guess(FILE *filePointer, int num_steps, struct ItinStep *step2pr);
void set_gmat_all_sc_setup(FILE *filePointer, int num_steps, struct ItinStep *step2pr, int num_tabs);
void determine_gmat_mean_epochs(FILE *filePointer, int num_steps, struct ItinStep *step2pr);
void write_gmat_optimizers(FILE *filePointer, int num_steps, struct ItinStep *step2pr);
void write_gmat_final_optimizer(FILE *filePointer, int num_steps, struct ItinStep *step2pr);

#endif
