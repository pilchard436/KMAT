#include "porkchop_analyzer_internal.h"
#include "gui/css_loader.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void update_pa_after_group_visibility_change(void);

static guint group_overview_source_id;
static gpointer group_overview_build_data;

void clear_porkchop_analyzer_analysis() {
	if(group_overview_source_id != 0) {
		g_source_remove(group_overview_source_id);
		group_overview_source_id = 0;
	}
	g_clear_pointer(&group_overview_build_data, g_free);
	if(pa_porkchop_points != NULL) free(pa_porkchop_points);
	pa_porkchop_points = NULL;
	pa_selected_arrival = NULL;
	if(curr_transfer_pa != NULL) free_itinerary(get_first(curr_transfer_pa));
	curr_transfer_pa = NULL;
	free(pa_groups);
	pa_groups = NULL;
	pa_num_groups = 0;
	pa_num_itins = 0;
}

void free_all_porkchop_analyzer_itins() {
	clear_porkchop_analyzer_analysis();
	if(pa_has_both_modes) {
		struct ItinStep ***departure_sets[] = {&pa_dual_flyby_departures, &pa_dual_capture_departures};
		int departure_counts[] = {pa_dual_flyby_num_deps, pa_dual_capture_num_deps};
		for(int set = 0; set < 2; set++) {
			if(*departure_sets[set] == NULL) continue;
			for(int i = 0; i < departure_counts[set]; i++)
				free_itinerary((*departure_sets[set])[i]);
			free(*departure_sets[set]);
			*departure_sets[set] = NULL;
		}
		pa_departures = NULL;
		pa_dual_flyby_num_deps = 0;
		pa_dual_capture_num_deps = 0;
		pa_has_both_modes = 0;
	} else if(pa_departures != NULL) {
		for(int i = 0; i < pa_num_deps; i++) free_itinerary(pa_departures[i]);
		free(pa_departures);
		pa_departures = NULL;
	}
	if(!is_available_system(pa_system)) free_celestial_system(pa_system);
	pa_system = NULL;
	
	if(pa_analysis_params.num_deps > 0 && pa_analysis_params.file_type > 2) {
		if(pa_analysis_params.calc_data.seq_info.to_target.type == ITIN_SEQ_INFO_TO_TARGET) free(pa_analysis_params.calc_data.seq_info.to_target.flyby_bodies);
		if(pa_analysis_params.calc_data.seq_info.spec_seq.type == ITIN_SEQ_INFO_SPEC_SEQ) free(pa_analysis_params.calc_data.seq_info.spec_seq.bodies);
	}
	pa_analysis_params.num_deps = 0;
}

typedef struct {
	int next_group_idx;
} GroupOverviewBuildData;

static gboolean build_group_overview_batch(gpointer user_data) {
	GroupOverviewBuildData *build_data = user_data;
	int groups_built = 0;
	while(build_data->next_group_idx < pa_num_groups && groups_built < 100) {
		int group_idx = build_data->next_group_idx++;
		if(!pa_groups[group_idx].has_itin_inside_filter) continue;
		int row = group_idx * 2 + 3;
		GString *route_label = g_string_new(NULL);
		GPtrArray *route_steps = g_ptr_array_new();
		for(struct ItinStep *step = pa_groups[group_idx].sample_arrival_node; step != NULL; step = step->prev)
			g_ptr_array_add(route_steps, step);
		for(gint step_idx = (gint)route_steps->len - 1; step_idx >= 0; step_idx--) {
			struct ItinStep *step = g_ptr_array_index(route_steps, step_idx);
			if(step_idx != (gint)route_steps->len - 1) g_string_append(route_label, " - ");
			g_string_append(route_label, step->body != NULL ? step->body->name : "DSM");
		}

		GtkWidget *check_button = gtk_check_button_new();
		gtk_widget_set_halign(check_button, GTK_ALIGN_CENTER);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_button), pa_groups[group_idx].show_group);
		g_signal_connect(check_button, "clicked", G_CALLBACK(on_change_itin_group_visibility), &pa_groups[group_idx]);
		pa_groups[group_idx].cb_pa_show_group = check_button;
		gtk_grid_attach(GTK_GRID(grid_pa_groups), check_button, 1, row, 1, 1);
		gtk_widget_show(check_button);

		GtkWidget *route_label_widget = gtk_label_new(route_label->str);
		gtk_widget_set_halign(route_label_widget, GTK_ALIGN_START);
		set_css_class_for_widget(route_label_widget, "pag-group-itin");
		gtk_grid_attach(GTK_GRID(grid_pa_groups), route_label_widget, 3, row, 1, 1);
		gtk_widget_show(route_label_widget);

		GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
		gtk_grid_attach(GTK_GRID(grid_pa_groups), separator, 2, row, 1, 1);
		gtk_widget_show(separator);
		g_ptr_array_free(route_steps, TRUE);
		g_string_free(route_label, TRUE);
		groups_built++;
	}

	if(build_data->next_group_idx < pa_num_groups) return G_SOURCE_CONTINUE;
	group_overview_source_id = 0;
	group_overview_build_data = NULL;
	g_free(build_data);
	return G_SOURCE_REMOVE;
}

void update_group_overview() {
	if(group_overview_source_id != 0) {
		g_source_remove(group_overview_source_id);
		group_overview_source_id = 0;
	}
	g_clear_pointer(&group_overview_build_data, g_free);
	if(grid_pa_groups != NULL && GTK_WIDGET(vp_pa_groups) == gtk_widget_get_parent(grid_pa_groups))
		gtk_container_remove(GTK_CONTAINER(vp_pa_groups), grid_pa_groups);

	grid_pa_groups = gtk_grid_new();
	GtkWidget *show_header = gtk_label_new("Show");
	gtk_widget_set_size_request(show_header, 50, -1);
	set_css_class_for_widget(show_header, "pag-header");
	gtk_widget_set_halign(show_header, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid_pa_groups), show_header, 1, 1, 1, 1);
	GtkWidget *itinerary_header = gtk_label_new("Itinerary");
	set_css_class_for_widget(itinerary_header, "pag-header");
	gtk_widget_set_halign(itinerary_header, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid_pa_groups), itinerary_header, 3, 1, 1, 1);
	GtkWidget *header_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_grid_attach(GTK_GRID(grid_pa_groups), header_separator, 2, 1, 1, 1);
	gtk_container_add(GTK_CONTAINER(vp_pa_groups), grid_pa_groups);
	gtk_widget_show_all(grid_pa_groups);

	GroupOverviewBuildData *build_data = g_new0(GroupOverviewBuildData, 1);
	group_overview_build_data = build_data;
	group_overview_source_id = g_idle_add(build_group_overview_batch, build_data);
}

G_MODULE_EXPORT void on_change_itin_group_visibility(GtkWidget* widget, gpointer data) {
	struct PorkchopGroup *group = (struct PorkchopGroup *) data;
	int visibility = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
	group->show_group = visibility;
	update_pa_after_group_visibility_change();
}

static void set_all_visible_groups_shown(int show) {
	set_porkchop_groups_visibility(pa_groups, pa_num_groups, show);
	for(int group_idx = 0; group_idx < pa_num_groups; group_idx++) {
		struct PorkchopGroup *group = &pa_groups[group_idx];
		if(!group->has_itin_inside_filter) continue;
		// set_active emits "toggled", not "clicked", so this does not re-enter the handler.
		if(group->cb_pa_show_group != NULL)
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(group->cb_pa_show_group), show);
	}
	update_pa_after_group_visibility_change();
}

G_MODULE_EXPORT void on_pa_select_all_groups(GtkWidget* widget, gpointer data) {
	set_all_visible_groups_shown(1);
}

G_MODULE_EXPORT void on_pa_deselect_all_groups(GtkWidget* widget, gpointer data) {
	set_all_visible_groups_shown(0);
}

// Comparison function for group qsort
int compare_by_count(const void *a, const void *b) {
	const struct PorkchopGroup *groupA = (const struct PorkchopGroup *) a;
	const struct PorkchopGroup *groupB = (const struct PorkchopGroup *) b;
	// Compare based on the count field
	return (groupB->count - groupA->count);
}

static size_t hash_itinerary_route(struct ItinStep *arrival) {
	size_t hash = (size_t)1469598103934665603ULL;
	for(struct ItinStep *step = arrival; step != NULL; step = step->prev) {
		hash ^= (size_t)(uintptr_t)step->body;
		hash *= (size_t)1099511628211ULL;
	}
	return hash;
}

static int itineraries_have_same_route(struct ItinStep *first, struct ItinStep *second) {
	while(first != NULL && second != NULL) {
		if(first->body != second->body) return 0;
		if(first->prev == NULL || second->prev == NULL) return first->prev == NULL && second->prev == NULL;
		first = first->prev;
		second = second->prev;
	}
	return first == NULL && second == NULL;
}

void initialize_itinerary_groups() {
	if(pa_num_itins <= 0) {
		pa_num_groups = 0;
		pa_groups = NULL;
		return;
	}

	size_t group_table_size = (size_t)pa_num_itins * 2 + 1;
	int *group_slots = calloc(group_table_size, sizeof(int));
	pa_groups = calloc(pa_num_itins, sizeof(struct PorkchopGroup));
	pa_num_groups = 0;
	if(group_slots == NULL || pa_groups == NULL) {
		free(group_slots);
		free(pa_groups);
		pa_groups = NULL;
		return;
	}

	for(int i = 0; i < pa_num_itins; i++) {
		struct ItinStep *arrival = pa_porkchop_points[i].data.arrival;
		size_t slot = hash_itinerary_route(arrival) % group_table_size;
		while(group_slots[slot] != 0) {
			int group_idx = group_slots[slot] - 1;
			if(itineraries_have_same_route(arrival, pa_groups[group_idx].sample_arrival_node)) {
				pa_groups[group_idx].count++;
				break;
			}
			slot = (slot + 1) % group_table_size;
		}
		if(group_slots[slot] == 0) {
			int group_idx = pa_num_groups++;
			group_slots[slot] = group_idx + 1;
			pa_groups[group_idx].sample_arrival_node = arrival;
			pa_groups[group_idx].count = 1;
			pa_groups[group_idx].num_steps = 1;
			pa_groups[group_idx].show_group = 1;
			pa_groups[group_idx].has_itin_inside_filter = 1;
			for(struct ItinStep *step = arrival; step->prev != NULL; step = step->prev)
				pa_groups[group_idx].num_steps++;
		}
	}

	qsort(pa_groups, pa_num_groups, sizeof(struct PorkchopGroup), compare_by_count);

	memset(group_slots, 0, group_table_size * sizeof(int));
	for(int group_idx = 0; group_idx < pa_num_groups; group_idx++) {
		size_t slot = hash_itinerary_route(pa_groups[group_idx].sample_arrival_node) % group_table_size;
		while(group_slots[slot] != 0) slot = (slot + 1) % group_table_size;
		group_slots[slot] = group_idx + 1;
	}
	for(int i = 0; i < pa_num_itins; i++) {
		struct ItinStep *arrival = pa_porkchop_points[i].data.arrival;
		size_t slot = hash_itinerary_route(arrival) % group_table_size;
		while(group_slots[slot] != 0) {
			int group_idx = group_slots[slot] - 1;
			if(itineraries_have_same_route(arrival, pa_groups[group_idx].sample_arrival_node)) {
				pa_porkchop_points[i].group = &pa_groups[group_idx];
				break;
			}
			slot = (slot + 1) % group_table_size;
		}
	}
	free(group_slots);

	update_group_overview();
}

// Rebuilding the group list here would reset its scroll position.
static void update_pa_after_group_visibility_change(void) {
	update_best_itin();
	update_pa_porkchop_diagram();
	update_pa_itinerary_preview();
	reset_min_max_feedback(1);
}
