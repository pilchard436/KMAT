#include "gmat_internal.h"
#include <stdio.h>
#include <math.h>

void create_gmat_sc_variables(FILE *filePointer, const char *var) {
	fprintf(filePointer, "Create Variable %s_epoch %s_c3 %s_rha %s_dha %s_bvazi %s_periapsis %s_incl\n", var, var, var, var, var, var, var);
}

void create_gmat_variables(FILE *filePointer, int num_steps) {
	print_gmat_section(filePointer, "Arrays, Variables, Strings");

	fprintf(filePointer, "%% Propagation time and Mean Epochs between states\n");
	fprintf(filePointer, "Create Variable prop_time");
	for(int i = 1; i < num_steps; i++) fprintf(filePointer, " mean_epoch%d", i);

	fprintf(filePointer, "\n\n%% Error and Cost Variables\n");
	fprintf(filePointer, "Create Variable Cost dep_speed");
	for(int i = 1; i < num_steps; i++) fprintf(filePointer, " errorR%d errorV%d", i, i);

	fprintf(filePointer, "\n\n%% Initial Guesses (hyperbolas)\n");
	create_gmat_sc_variables(filePointer, sc_var[0]);
	for(int i = 1; i < num_steps-1; i++) {
		char var[20];
		sprintf(var, "%s%i", sc_var[1], i);
		create_gmat_sc_variables(filePointer, var);
	}
	create_gmat_sc_variables(filePointer, sc_var[2]);

	fprintf(filePointer, "\n%% misc\n");
	fprintf(filePointer, "Create Variable loopIdx EarthGravParam\n");
}

void set_gmat_sc_initial_guess(FILE *filePointer, const char *var, struct ItinStep *step2pr) {
	struct HyperbolaParams hyp_params;
	double rp;

	if(step2pr->prev == NULL)
		hyp_params = get_hyperbola_params(vec3(0,0,0), step2pr->next[0]->v_dep, step2pr->v_body, step2pr->body, 200e3, HYP_DEPARTURE);
	else if(step2pr->num_next_nodes == 0) {
		hyp_params = get_hyperbola_params(step2pr->v_arr, vec3(0,0,0), step2pr->v_body, step2pr->body,200e3, HYP_ARRIVAL);
	} else {
		Vector3 v_arr = step2pr->v_arr;
		Vector3 v_dep = step2pr->next[0]->v_dep;
		Vector3 v_body = step2pr->v_body;
		rp = get_flyby_periapsis(v_arr, v_dep, v_body, step2pr->body);
		hyp_params = get_hyperbola_params(step2pr->v_arr, step2pr->next[0]->v_dep, step2pr->v_body, step2pr->body, 0, HYP_FLYBY);
	}


	fprintf(filePointer, "\t%s_epoch = %f\n", var, step2pr->date-gmat_jd_mod);
	fprintf(filePointer, "\t%s_c3 = %f\n", var, hyp_params.c3_energy/1e6);
	fprintf(filePointer, "\t%s_rha = %f\n", var, rad2deg(hyp_params.incoming.bplane_angle));
	fprintf(filePointer, "\t%s_dha = %f\n", var, rad2deg(hyp_params.incoming.decl));

	if(step2pr->prev != NULL && step2pr->next != NULL) {
		fprintf(filePointer, "\t%s_bvazi = %f\n", var, rad2deg(hyp_params.incoming.bvazi));
		fprintf(filePointer, "\t%s_periapsis = %f\n", var, rp*1e-3);
	}
}

void set_gmat_initial_guess(FILE *filePointer, int num_steps, struct ItinStep *step2pr) {
	step2pr = get_first(step2pr);
	fprintf(filePointer, "BeginScript 'Initial Guess Values'\n");
	fprintf(filePointer, "\t%% Set Initial Guess Hyperbolas\n");
	set_gmat_sc_initial_guess(filePointer, sc_var[0], step2pr);
	for(int i = 1; i < num_steps-1; i++) {
		step2pr = step2pr->next[0];
		fprintf(filePointer, "\n");
		char var[20];
		sprintf(var, "%s%i", sc_var[1], i);
		set_gmat_sc_initial_guess(filePointer, var, step2pr);
	}
	fprintf(filePointer, "\n");
	step2pr = step2pr->next[0];
	set_gmat_sc_initial_guess(filePointer, sc_var[2], step2pr);

	step2pr = get_first(step2pr);
	fprintf(filePointer, "\n\t%% Set Base Constraints\n");
	fprintf(filePointer, "\t%s_bvazi = 90\n", sc_var[0]);
	fprintf(filePointer, "\t%s_periapsis = %f\n", sc_var[0], step2pr->body->radius / 1e3 + 200);
	step2pr = get_last(step2pr);
	fprintf(filePointer, "\t%s_bvazi = 90\n", sc_var[2]);
	fprintf(filePointer, "\t%s_periapsis = %f\n", sc_var[2], step2pr->body->radius / 1e3 * 1.1);

	fprintf(filePointer, "\n\t%% Misc\n");
	fprintf(filePointer, "\tEarthGravParam = 398600\n");
	fprintf(filePointer, "\tCost = sqrt(%s_c3 + 2*EarthGravParam/%s_periapsis) - sqrt(EarthGravParam/%s_periapsis)\n", sc_var[0], sc_var[0], sc_var[0]);
	fprintf(filePointer, "EndScript\n");
}

void set_gmat_all_sc_setup(FILE *filePointer, int num_steps, struct ItinStep *step2pr, int num_tabs) {
	step2pr = get_first(step2pr);
	fprintf(filePointer, "\n");
	for(int i = 0; i < num_tabs; i++) fprintf(filePointer, "\t");
	fprintf(filePointer, "BeginScript 'All S/C Setup'\n");
	setup_gmat_sc(filePointer, sc_name[0], sc_var[0], body_name[step2pr->body->id], coord_name[step2pr->body->id], 1+num_tabs, 1);

	for(int i = 1; i < num_steps-1; i++) {
		fprintf(filePointer, "\n");
		step2pr = step2pr->next[0];
		char sc[20];
		char var[20];
		sprintf(var, "%s%i", sc_var[1], i);
		sprintf(sc, "%s%i_Arr", sc_name[1], i);
		setup_gmat_sc(filePointer, sc, var, body_name[step2pr->body->id], coord_name[step2pr->body->id], 1+num_tabs, 0);
		for(int j = 0; j < num_tabs; j++) fprintf(filePointer, "\t");
		fprintf(filePointer, "\t%s%i_Dep = %s%i_Arr\n", sc_name[1], i, sc_name[1], i);
	}
	fprintf(filePointer, "\n");
	step2pr = step2pr->next[0];
	setup_gmat_sc(filePointer, sc_name[2], sc_var[2], body_name[step2pr->body->id], coord_name[step2pr->body->id], 1+num_tabs, 0);

	for(int i = 0; i < num_tabs; i++) fprintf(filePointer, "\t");
	fprintf(filePointer, "EndScript\n");
}

void determine_gmat_mean_epochs(FILE *filePointer, int num_steps, struct ItinStep *step2pr) {
	fprintf(filePointer, "\nBeginScript 'Determine Mean Epochs'\n");
	fprintf(filePointer, "\tmean_epoch1 = (%s_epoch+%s1_epoch)/2'\n", sc_var[0], sc_var[1]);
	for(int i = 2; i < num_steps-1; i++) {
		fprintf(filePointer, "\tmean_epoch%d = (%s%d_epoch+%s%d_epoch)/2'\n", i, sc_var[1], i-1, sc_var[1], i);
	}
	fprintf(filePointer, "\tmean_epoch%d = (%s%d_epoch+%s_epoch)/2'\n", num_steps-1, sc_var[1], num_steps-2, sc_var[2]);
	fprintf(filePointer, "EndScript\n");
}

void write_gmat_optimizer(FILE *filePointer, int traj_id, const char *sc1, const char *var1, const char *coord1, const char *body1, const char *sc2, const char *var2, const char *coord2, const char *body2, int is_departure) {
	fprintf(filePointer, "\nOptimize 'Optimize Trajectory %d' %s {SolveMode = %s, ExitMode = DiscardAndContinue, ShowProgressWindow = true}\n", traj_id, vf13ad_solver, solvermode);
	fprintf(filePointer, "\tloopIdx = loopIdx + 1\n\n");
	fprintf(filePointer, "\t%% Vary states\n");

	if(is_departure) {
		fprintf(filePointer, "\tVary %s(%s_c3  = %s_c3)\n", vf13ad_solver, var1, var1);
		fprintf(filePointer, "\tVary %s(%s_rha = %s_rha)\n", vf13ad_solver, var1, var1);
		fprintf(filePointer, "\tVary %s(%s_dha = %s_dha)\n", vf13ad_solver, var1, var1);
	} else {
		fprintf(filePointer, "\tVary %s(%s_bvazi  = %s_bvazi)\n", vf13ad_solver, var1, var1);
		fprintf(filePointer, "\tVary %s(%s_periapsis = %s_periapsis)\n", vf13ad_solver, var1, var1);
		fprintf(filePointer, "\tVary %s(%s_epoch = %s_epoch)\n", vf13ad_solver, var2, var2);
	}
	fprintf(filePointer, "\tVary %s(%s_c3  = %s_c3)\n", vf13ad_solver, var2, var2);
	fprintf(filePointer, "\tVary %s(%s_rha = %s_rha)\n", vf13ad_solver, var2, var2);
	fprintf(filePointer, "\tVary %s(%s_dha = %s_dha)\n", vf13ad_solver, var2, var2);

	fprintf(filePointer, "\n\tBeginScript 'Set-up S/C'\n");
	setup_gmat_sc(filePointer, sc1, var1, body1, coord1, 2, is_departure);
	fprintf(filePointer, "\n");
	setup_gmat_sc(filePointer, sc2, var2, body2, coord2, 2, 0);
	fprintf(filePointer, "\tEndScript\n");

	fprintf(filePointer, "\n\t%% Propagate forwards from initial body and backwards from target body\n");
	fprintf(filePointer, "\tprop_time = mean_epoch%d - %s.UTCModJulian\n", traj_id, sc1);
	fprintf(filePointer, "\tPropagate 'PropForwards' %s(%s) {%s.ElapsedDays = prop_time}\n", propagator, sc1, sc1);
	fprintf(filePointer, "\tprop_time = mean_epoch%d - %s.UTCModJulian\n", traj_id, sc2);
	fprintf(filePointer, "\tPropagate 'PropBackwards' BackProp %s(%s) {%s.ElapsedDays = prop_time}\n", propagator, sc2, sc2);

	fprintf(filePointer, "\n\t%% Calculate Error in Position and Velocity\n");
	fprintf(filePointer, "\terrorR%d = sqrt((%s.%s.X - %s.%s.X)^2 + (%s.%s.Y - %s.%s.Y)^2 + (%s.%s.Z - %s.%s.Z)^2)\n",
			traj_id, sc1, coord_name[0], sc2, coord_name[0], sc1, coord_name[0], sc2, coord_name[0], sc1, coord_name[0], sc2, coord_name[0]);
	fprintf(filePointer, "\terrorV%d = sqrt((%s.%s.VX - %s.%s.VX)^2 + (%s.%s.VY - %s.%s.VY)^2 + (%s.%s.VZ - %s.%s.VZ)^2)\n",
			traj_id, sc1, coord_name[0], sc2, coord_name[0], sc1, coord_name[0], sc2, coord_name[0], sc1, coord_name[0], sc2, coord_name[0]);

	fprintf(filePointer, "\n\t%% Apply the collocation constraints constraints on final states\n");
	fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.X  = %s.%s.X)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
	fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.Y  = %s.%s.Y)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
	fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.Z  = %s.%s.Z)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
	fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.VX = %s.%s.VX)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
	fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.VY = %s.%s.VY)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
	fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.VZ = %s.%s.VZ)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);

	fprintf(filePointer, "EndOptimize\n");
}

void write_gmat_optimizers(FILE *filePointer, int num_steps, struct ItinStep *step2pr) {
	step2pr = get_first(step2pr);
	char sc1[20];
	char var1[20];
	char sc2[20];
	char var2[20];
	sprintf(var2, "%s%i", sc_var[1], 1);
	sprintf(sc2, "%s%i_Arr", sc_name[1], 1);
	write_gmat_optimizer(filePointer, 1, sc_name[0], sc_var[0], coord_name[step2pr->body->id], body_name[step2pr->body->id], sc2, var2, coord_name[step2pr->next[0]->body->id], body_name[step2pr->next[0]->body->id], 1);
	for(int i = 1; i < num_steps-2; i++) {
		fprintf(filePointer, "\n");
		step2pr = step2pr->next[0];
		sprintf(var1, "%s%i", sc_var[1], i);
		sprintf(sc1, "%s%i_Dep", sc_name[1], i);
		sprintf(var2, "%s%i", sc_var[1], i+1);
		sprintf(sc2, "%s%i_Arr", sc_name[1], i+1);
		write_gmat_optimizer(filePointer, i+1, sc1, var1, coord_name[step2pr->body->id], body_name[step2pr->body->id], sc2, var2, coord_name[step2pr->next[0]->body->id], body_name[step2pr->next[0]->body->id], 0);
	}
	fprintf(filePointer, "\n");
	step2pr = step2pr->next[0];
	sprintf(var1, "%s%i", sc_var[1], num_steps-2);
	sprintf(sc1, "%s%i_Dep", sc_name[1], num_steps-2);
	write_gmat_optimizer(filePointer, num_steps-1, sc1, var1, coord_name[step2pr->body->id], body_name[step2pr->body->id], sc_name[2], sc_var[2], coord_name[step2pr->next[0]->body->id], body_name[step2pr->next[0]->body->id], 0);
}

void write_gmat_final_optimizer(FILE *filePointer, int num_steps, struct ItinStep *step2pr) {
	fprintf(filePointer, "\nOptimize 'Final Trajectory Optimization' %s {SolveMode = %s, ExitMode = DiscardAndContinue, ShowProgressWindow = true}\n", vf13ad_solver, solvermode);
	fprintf(filePointer, "\tloopIdx = loopIdx + 1\n\n");
	fprintf(filePointer, "\t%% Vary states\n");

	// fprintf(filePointer, "\tVary %s(%s_epoch  = %s_epoch)\n", vf13ad_solver, sc_var[0], sc_var[0]);
	fprintf(filePointer, "\tVary %s(%s_c3  = %s_c3)\n", vf13ad_solver, sc_var[0], sc_var[0]);
	fprintf(filePointer, "\tVary %s(%s_rha = %s_rha)\n", vf13ad_solver, sc_var[0], sc_var[0]);
	fprintf(filePointer, "\tVary %s(%s_dha = %s_dha)\n", vf13ad_solver, sc_var[0], sc_var[0]);
	for(int i = 1; i < num_steps; i++) {
		fprintf(filePointer, "\n");
		char var[20];
		if(i < num_steps-1) sprintf(var, "%s%i", sc_var[1], i);
		else sprintf(var, "%s", sc_var[2]);
		/*if(i > 1)*/ fprintf(filePointer, "\tVary %s(%s_epoch = %s_epoch)\n", vf13ad_solver, var, var);
		fprintf(filePointer, "\tVary %s(%s_c3  = %s_c3)\n", vf13ad_solver, var, var);
		fprintf(filePointer, "\tVary %s(%s_rha = %s_rha)\n", vf13ad_solver, var, var);
		fprintf(filePointer, "\tVary %s(%s_dha = %s_dha)\n", vf13ad_solver, var, var);
		if(i < num_steps-1) fprintf(filePointer, "\tVary %s(%s_bvazi  = %s_bvazi)\n", vf13ad_solver, var, var);
		if(i < num_steps-1) fprintf(filePointer, "\tVary %s(%s_periapsis = %s_periapsis)\n", vf13ad_solver, var, var);
	}

	set_gmat_all_sc_setup(filePointer, num_steps, step2pr, 1);

	step2pr = get_first(step2pr);

	fprintf(filePointer, "\n\t%% Delta-v from starting orbit\n");
	fprintf(filePointer, "\tCost = %s.%s.VMAG - sqrt(EarthGravParam/6571)\n", sc_name[0], coord_name[step2pr->body->id]);

	fprintf(filePointer, "\n\t%% Store inclinations\n");
	fprintf(filePointer, "\t%s_incl = %s.%s.INC\n", sc_var[0], sc_name[0], coord_name[step2pr->body->id]);
	for(int i = 1; i < num_steps-1; i++) {
		step2pr = step2pr->next[0];
		char sc[20];
		char var[20];
		sprintf(var, "%s%i", sc_var[1], i);
		sprintf(sc, "%s%i_Arr", sc_name[1], i);

		fprintf(filePointer, "\t%s_incl = %s.%s.INC\n", var, sc, coord_name[step2pr->body->id]);
	}
	step2pr = step2pr->next[0];
	fprintf(filePointer, "\t%s_incl = %s.%s.INC\n", sc_var[0], sc_name[0], coord_name[step2pr->body->id]);

	fprintf(filePointer, "\n\t%% Propagate all\n");
	fprintf(filePointer, "\tprop_time = mean_epoch%d - %s.UTCModJulian\n", 1, sc_name[0]);
	fprintf(filePointer, "\tPropagate 'PropForwards' %s(%s) {%s.ElapsedDays = prop_time}\n", propagator, sc_name[0], sc_name[0]);

	for(int i = 1; i < num_steps-1; i++) {
		char sc[20];
		sprintf(sc, "%s%i", sc_name[1], i);
		fprintf(filePointer, "\tprop_time = mean_epoch%d - %s_Arr.UTCModJulian\n", i, sc);
		fprintf(filePointer, "\tPropagate 'PropBackwards' BackProp %s(%s_Arr) {%s_Arr.ElapsedDays = prop_time}\n\n", propagator, sc, sc);
		fprintf(filePointer, "\tprop_time = mean_epoch%d - %s_Dep.UTCModJulian\n", i+1, sc);
		fprintf(filePointer, "\tPropagate 'PropForwards' %s(%s_Dep) {%s_Dep.ElapsedDays = prop_time}\n", propagator, sc, sc);
	}
	fprintf(filePointer, "\tprop_time = mean_epoch%d - %s.UTCModJulian\n", num_steps-1, sc_name[2]);
	fprintf(filePointer, "\tPropagate 'PropBackwards' BackProp %s(%s) {%s.ElapsedDays = prop_time}\n", propagator, sc_name[2], sc_name[2]);


	fprintf(filePointer, "\n\t%% Calculate Error in Position and Velocity\n");
	for(int i = 0; i < num_steps-1; i++) {
		int traj_id = i+1;
		char sc1[20];
		char sc2[20];
		if(i == 0) sprintf(sc1, "%s", sc_name[0]);
		else sprintf(sc1, "%s%i_Dep", sc_name[1], i);
		if(i >= num_steps-2) sprintf(sc2, "%s", sc_name[2]);
		else sprintf(sc2, "%s%i_Arr", sc_name[1], i+1);

		fprintf(filePointer,
				"\terrorR%d = sqrt((%s.%s.X - %s.%s.X)^2 + (%s.%s.Y - %s.%s.Y)^2 + (%s.%s.Z - %s.%s.Z)^2)\n",
				traj_id, sc1, coord_name[0], sc2, coord_name[0], sc1, coord_name[0], sc2, coord_name[0], sc1, coord_name[0], sc2, coord_name[0]);
		fprintf(filePointer,
				"\terrorV%d = sqrt((%s.%s.VX - %s.%s.VX)^2 + (%s.%s.VY - %s.%s.VY)^2 + (%s.%s.VZ - %s.%s.VZ)^2)\n",
				traj_id, sc1, coord_name[0], sc2, coord_name[0], sc1, coord_name[0], sc2, coord_name[0], sc1, coord_name[0], sc2, coord_name[0]);
	}


	fprintf(filePointer, "\n\t%% Apply the collocation constraints constraints on final states");
	for(int i = 0; i < num_steps-1; i++) {
		char sc1[20];
		char sc2[20];
		if(i == 0) sprintf(sc1, "%s", sc_name[0]);
		else sprintf(sc1, "%s%i_Dep", sc_name[1], i);
		if(i >= num_steps-2) sprintf(sc2, "%s", sc_name[2]);
		else sprintf(sc2, "%s%i_Arr", sc_name[1], i+1);


		fprintf(filePointer, "\n\tNonlinearConstraint %s(%s.%s.X  = %s.%s.X)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
		fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.Y  = %s.%s.Y)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
		fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.Z  = %s.%s.Z)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
		fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.VX = %s.%s.VX)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
		fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.VY = %s.%s.VY)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
		fprintf(filePointer, "\tNonlinearConstraint %s(%s.%s.VZ = %s.%s.VZ)\n", vf13ad_solver, sc1, coord_name[0], sc2, coord_name[0]);
	}

	fprintf(filePointer, "\n\tMinimize %s(Cost)\n", vf13ad_solver);
	fprintf(filePointer, "EndOptimize\n");

	fprintf(filePointer, "\nReport RFDepStates %s_epoch %s_c3 %s_rha %s_dha %s_bvazi %s_periapsis\n",
			sc_var[0], sc_var[0], sc_var[0], sc_var[0], sc_var[0], sc_var[0]);
	char *coord = coord_name[0];
	for(int i = 1; i < num_steps-1; i++) {
		char sc[20];
		sprintf(sc, "%s%i_Arr", sc_name[1], i);
		fprintf(filePointer, "Report RFDepStates %s.UTCModJulian %s.%s.X %s.%s.Y %s.%s.Z %s.%s.VX %s.%s.VY %s.%s.VZ\n",
				sc, sc, coord, sc, coord, sc, coord, sc, coord, sc, coord, sc, coord);
	}
	fprintf(filePointer, "Report RFDepStates %s.UTCModJulian %s.%s.X %s.%s.Y %s.%s.Z %s.%s.VX %s.%s.VY %s.%s.VZ %s_epoch\n",
			sc_name[2], sc_name[2], coord, sc_name[2], coord, sc_name[2], coord, sc_name[2], coord, sc_name[2], coord, sc_name[2], coord, sc_var[2]);
}
