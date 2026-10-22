#ifndef KSP_ITIN_TOOL_H
#define KSP_ITIN_TOOL_H

#include "tools/celestial_systems.h"
#include "orbitlib_transfer.h"
#include <stdio.h>

struct ItinStep {
	Body *body;
	Vector3 r;
	Vector3 v_arr, v_body, v_dep;
	double date;
	int num_next_nodes;
	struct ItinStep *prev;
	struct ItinStep **next;
};

enum ItinSequenceInfoType {ITIN_SEQ_INFO_TO_TARGET, ITIN_SEQ_INFO_SPEC_SEQ};

typedef union ItinSequenceInfo {
	struct ItinSequenceInfoToTarget {
		enum ItinSequenceInfoType type;
		CelestSystem *system;
		Body *dep_body, *arr_body, **flyby_bodies;
		int num_flyby_bodies;
		int allow_resonant;	// also search flybys that return to the same body after revolutions
	} to_target;

	struct ItinSequenceInfoSpecItin {
		enum ItinSequenceInfoType type;
		CelestSystem *system;
		struct Body **bodies;
		int num_steps;
	} spec_seq;
} ItinSequenceInfo;

struct Dv_Filter {
	double max_totdv, max_depdv, max_satdv;
	double dep_periapsis, arr_periapsis;
	int last_transfer_type;
};

typedef struct Calc_Acc {
	double lambert, fb_finder;
} Calc_Acc;

struct PorkchopPoint {
	struct ItinStep *arrival;
	double dep_date, dur;
	double dv_dep, dv_dsm, dv_flyby, dv_arr_cap, dv_arr_circ;
};

enum LastTransferType {
	TF_FLYBY,
	TF_CAPTURE,
	// Retained for existing calculation files; new circularization selection is deferred to custom final orbits.
	TF_CIRC,
	TF_BOTH
};

// returns an array of porkchop points analyzed from the given departures (allocates porkchop array memory --> needs to be freed)
struct PorkchopPoint *create_porkchop_array_from_departures(struct ItinStep **departures, int num_deps, double dep_periapsis, double arr_periapsis);

// add itinerary departure date, duration, departure dv, deep-space maneuvre dv and arrival dv in porkchop array
struct PorkchopPoint create_porkchop_point(struct ItinStep *itin, double dep_periapsis, double arr_periapsis);
int set_porkchop_exact_flybys(int exact);

// from current step and given information, initiate calculation of next steps
int calc_next_spec_itin_step(struct ItinStep *curr_step, CelestSystem *system, Body **bodies, double jd_max_arr, struct Dv_Filter *dv_filter, int num_steps, int step);

// from current step and given information, initiate calculation of next steps
int calc_next_itin_to_target_step(struct ItinStep *curr_step, struct ItinSequenceInfoToTarget *seq_info, double jd_max_arr, double max_total_duration, struct Dv_Filter *dv_filter);

// initiate calc of next itinerary steps and return 0 if no steps are remaining in this itinerary (1 otherwise)
int continue_to_next_steps_and_check_for_valid_itins(struct ItinStep *curr_step, int num_of_end_nodes, struct ItinSequenceInfoToTarget *seq_info, double jd_max_arr, double max_total_duration, struct Dv_Filter *dv_filter);

// find end nodes (next step at arrival body) and copy to the end of the next steps array of curr_step
int find_copy_and_store_end_nodes(struct ItinStep *curr_step, Body *arr_body);

// removes end nodes from initerary that do not satisfy dv requirements (returns new number of end nodes)
int remove_end_nodes_that_do_not_satisfy_dv_requirements(struct ItinStep *curr_step, int num_of_end_nodes, struct Dv_Filter *dv_filter);

// delta-v of one step of a single itinerary, using the same burns as the itinerary overview texts
double itinerary_step_dv(struct ItinStep *step, double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type);

// The itinerary modules build on the types above; including this header gives all of them.
#include "itinerary_tree.h"
#include "itinerary_text.h"
#include "flyby_policy.h"
#include "flyby_search.h"
#include "leg_variants.h"
#include "resonant_flybys.h"

#endif //KSP_ITIN_TOOL_H
