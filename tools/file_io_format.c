#include "tools/file_io.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void append_header_text(char *string, size_t string_size, const char *format, ...) {
	size_t current_length = strlen(string);
	if(current_length >= string_size) return;
	va_list args;
	va_start(args, format);
	vsnprintf(string + current_length, string_size - current_length, format, args);
	va_end(args);
}

static void append_header_date(char *string, size_t string_size, const char *label, double jd, enum DateType date_format) {
	if(!isfinite(jd) || fabs(jd) > 6000000.0) {
		append_header_text(string, string_size, "%s: Unbounded\n", label);
		return;
	}
	char date_string[32];
	date_to_string(convert_JD_date(jd, date_format), date_string, 1);
	append_header_text(string, string_size, "%s: %s\n", label, date_string);
}

void print_header_data_to_string(ItinStepBinHeaderData header, char *string, size_t string_size, enum DateType date_format) {
	if(string == NULL || string_size == 0) return;
	string[0] = '\0';
	append_header_text(string, string_size, "Number of stored nodes: %d\n", header.num_nodes);
	append_header_text(string, string_size, "Number of Departures: %d\n", header.num_deps);
	if(header.file_type > 2) {
		append_header_text(string, string_size, "Number of Itineraries: %d\n", header.num_itins);
		append_header_date(string, string_size, "Min departure date", header.calc_data.jd_min_dep, date_format);
		append_header_date(string, string_size, "Max departure date", header.calc_data.jd_max_dep, date_format);
		append_header_date(string, string_size, "Max arrival date", header.calc_data.jd_max_arr, date_format);
		if(!isfinite(header.calc_data.max_duration) || fabs(header.calc_data.max_duration) > 1e12)
			append_header_text(string, string_size, "Max duration: Unbounded\n");
		else
			append_header_text(string, string_size, "Max duration: %.2f days\n", header.calc_data.max_duration);
		append_header_text(string, string_size, "Dep date step: %.4f days\n", header.calc_data.step_dep_date);
		append_header_text(string, string_size, "Num of deps per date: %d\n", header.calc_data.num_deps_per_date);
		append_header_text(string, string_size, "Max num of waiting orbits: %d\n", header.calc_data.max_num_waiting_orbits);
		append_header_text(string, string_size, "Max tot dv: %.0f m/s\n", header.calc_data.dv_filter.max_totdv);
		append_header_text(string, string_size, "Max dep dv: %.0f m/s\n", header.calc_data.dv_filter.max_depdv);
		append_header_text(string, string_size, "Max sat dv: %.0f m/s\n", header.calc_data.dv_filter.max_satdv);
		if(header.file_type >= 5) {
			append_header_text(string, string_size, "Lambert accuracy: %e s\n", header.calc_data.calc_acc.lambert);
			append_header_text(string, string_size, "Fly-by Pos accuracy: %e m\n", header.calc_data.calc_acc.fb_finder);
		} else {
			append_header_text(string, string_size, "Lambert accuracy: %.2e s\n", 1.0);
			append_header_text(string, string_size, "Fly-by Pos accuracy: %.2e m\n", 1.0);
		}
		append_header_text(string, string_size, "Departure Periapsis: %.0fkm\n", header.calc_data.dv_filter.dep_periapsis / 1e3);
		append_header_text(string, string_size, "Arrival Periapsis: %.0fkm\n", header.calc_data.dv_filter.arr_periapsis / 1e3);
		if(header.calc_data.dv_filter.last_transfer_type == TF_BOTH)
			append_header_text(string, string_size, "Last transfer type: Fly-By and Capture\n");
		else if(header.calc_data.dv_filter.last_transfer_type == TF_FLYBY)
			append_header_text(string, string_size, "Last transfer type: Fly-By\n");
		else if(header.calc_data.dv_filter.last_transfer_type == TF_CAPTURE ||
			header.calc_data.dv_filter.last_transfer_type == TF_CIRC)
			append_header_text(string, string_size, "Last transfer type: Capture\n");
		if(header.calc_data.search_options.known) {
			ItinSearchOptions options = header.calc_data.search_options;
			append_header_text(string, string_size, "Search options: DSM %s, moon-aware flybys %s, arrival moon assist %s, resonant flybys %s, custom starting orbit %s\n",
				options.allow_dsm ? "on" : "off",
				options.moon_aware_flybys ? "on" : "off",
				options.moon_assist_arrival ? "on" : "off",
				options.allow_resonant ? "on" : "off",
				options.use_custom_starting_orbit ? "on" : "off");
		} else if(header.file_type >= 8) {
			append_header_text(string, string_size, "Search options: unavailable\n");
		}

		if(header.calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) {
			append_header_text(string, string_size, "Departure body: %s\n", header.calc_data.seq_info.to_target.dep_body->name);
			append_header_text(string, string_size, "Arrival body: %s\n", header.calc_data.seq_info.to_target.arr_body->name);
			append_header_text(string, string_size, "Allowed Fly-By bodies:\n");
			for(int i = 0; i < header.calc_data.seq_info.to_target.num_flyby_bodies; i++)
				append_header_text(string, string_size, "  - %s\n", header.calc_data.seq_info.to_target.flyby_bodies[i]->name);
		} else if(header.calc_data.seq_info.spec_seq.type == ITIN_SEQ_INFO_SPEC_SEQ) {
			append_header_text(string, string_size, "Sequence:\n");
			for(int i = 0; i < header.calc_data.seq_info.spec_seq.num_steps; i++)
				append_header_text(string, string_size, "  %d. %s\n", i + 1, header.calc_data.seq_info.spec_seq.bodies[i]->name);
		}
	} else {
		append_header_text(string, string_size, "\nCan't find further analysis parameters because the analysis file\nwas created in version 1.2.2 or before\n");
	}
}