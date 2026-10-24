#include "unit_tests.h"

void run_porkchop_tests(void) {
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    (void)pi;
    (void)tolerance;
    struct PorkchopGroup hidden_group = {.show_group = 0};
    struct PorkchopGroup visible_group = {.show_group = 1};
    struct PorkchopAnalyzerPoint porkchop_points[] = {
        {.inside_filter = 0, .group = &visible_group},
        {.inside_filter = 1, .group = &hidden_group},
        {.inside_filter = 1, .group = &visible_group}
    };
    expect_int("visible porkchop selection skips filtered and hidden points",
                           find_first_visible_porkchop_point(porkchop_points, 3), 2);
    porkchop_points[2].inside_filter = 0;
    expect_int("visible porkchop selection returns no-match sentinel",
                           find_first_visible_porkchop_point(porkchop_points, 3), -1);
    expect_int("arrival-date filter is active on arrival-date axis",
                           porkchop_axis_filter_field_is_active(PA_ARR, PA_YAXIS_ARRDATE), 1);
    expect_int("arrival-date filter is inactive on duration axis",
                           porkchop_axis_filter_field_is_active(PA_ARR, PA_YAXIS_DUR), 0);
    expect_int("duration filter is active on duration axis",
                           porkchop_axis_filter_field_is_active(PA_DUR, PA_YAXIS_DUR), 1);
    expect_int("duration filter is inactive on arrival-date axis",
                           porkchop_axis_filter_field_is_active(PA_DUR, PA_YAXIS_ARRDATE), 0);
    expect_int("departure filter stays active on both axes",
                           porkchop_axis_filter_field_is_active(PA_DEP, PA_YAXIS_ARRDATE) &&
                           porkchop_axis_filter_field_is_active(PA_DEP, PA_YAXIS_DUR), 1);

    struct PorkchopGroup selectable_group = {.show_group = 1};
    struct PorkchopGroup hidden_selectable_group = {.show_group = 0};
    struct PorkchopAnalyzerPoint selectable_points[] = {
        {.data = {.dep_date = 5, .dur = 5, .dv_dep = 100}, .inside_filter = 1, .group = &selectable_group},
        {.data = {.dep_date = 8, .dur = 8, .dv_dep = 5}, .inside_filter = 1, .group = &selectable_group},
        {.data = {.dep_date = 4, .dur = 4, .dv_dep = 1}, .inside_filter = 1, .group = &hidden_selectable_group},
        {.data = {.dep_date = 2, .dur = 2, .dv_dep = 2}, .inside_filter = 0, .group = &selectable_group},
        {.data = {.dep_date = 2, .dur = 3, .dv_dep = 20}, .inside_filter = 1, .group = &selectable_group}
    };
    struct PorkchopPlotRange selectable_range = {0, 10, 0, 10};
    expect_int("best porkchop node in duration view ignores filtered, hidden, and off-view nodes",
               find_best_porkchop_point_in_view(selectable_points, 5, TF_FLYBY, 0, &selectable_range), 1);
    expect_int("best porkchop node in arrival-date view uses arrival date coordinates",
               find_best_porkchop_point_in_view(selectable_points, 5, TF_FLYBY, 1, &selectable_range), 4);
    expect_int("porkchop node hit testing selects the nearest visible point",
               find_nearest_porkchop_point(selectable_points, 5, 0, &selectable_range,
                       0, 100, 100, 51, 50, 10), 0);
    expect_int("porkchop node hit testing ignores points outside the click radius",
               find_nearest_porkchop_point(selectable_points, 5, 0, &selectable_range,
                       0, 100, 100, 100, 0, 5), -1);
    struct PorkchopAnalyzerPoint nonfinite_dv_point = {
        .data = {.dep_date = 5, .dur = 5, .dv_dep = NAN},
        .inside_filter = 1,
        .group = &selectable_group
    };
    expect_int("porkchop hover hit testing recognizes nodes with unavailable delta-v",
               find_nearest_porkchop_point(&nonfinite_dv_point, 1, 0, &selectable_range,
                       0, 100, 100, 50, 50, 12), 0);

    struct ItinStep navigation_nodes[7] = {{0}};
    struct PorkchopAnalyzerPoint navigation_points[] = {
        {.data = {.arrival = &navigation_nodes[0], .dep_date = 5, .dur = 5}, .inside_filter = 1, .group = &selectable_group},
        {.data = {.arrival = &navigation_nodes[1], .dep_date = 3, .dur = 5}, .inside_filter = 1, .group = &selectable_group},
        {.data = {.arrival = &navigation_nodes[2], .dep_date = 7, .dur = 5}, .inside_filter = 1, .group = &selectable_group},
        {.data = {.arrival = &navigation_nodes[3], .dep_date = 5, .dur = 7}, .inside_filter = 1, .group = &selectable_group},
        {.data = {.arrival = &navigation_nodes[4], .dep_date = 5, .dur = 3}, .inside_filter = 1, .group = &selectable_group},
        {.data = {.arrival = &navigation_nodes[5], .dep_date = 5.5, .dur = 5}, .inside_filter = 1, .group = &hidden_selectable_group},
        {.data = {.arrival = &navigation_nodes[6], .dep_date = 5.6, .dur = 5.8}, .inside_filter = 1, .group = &selectable_group}
    };
    expect_int("left arrow selects the nearest visible node to the left",
               find_directional_porkchop_neighbor(navigation_points, 7, &navigation_nodes[0],
                       PORKCHOP_LEFT, 0, &selectable_range, 100, 100), 1);
    expect_int("right arrow prefers a cardinal node over a closer diagonal node",
               find_directional_porkchop_neighbor(navigation_points, 7, &navigation_nodes[0],
                       PORKCHOP_RIGHT, 0, &selectable_range, 100, 100), 2);
    navigation_points[2].inside_filter = 0;
    expect_int("right arrow falls back to the nearest off-axis node when needed",
               find_directional_porkchop_neighbor(navigation_points, 7, &navigation_nodes[0],
                       PORKCHOP_RIGHT, 0, &selectable_range, 100, 100), 6);
    navigation_points[2].inside_filter = 1;
    expect_int("up arrow prefers a straight-up node over an up-left node",
               find_directional_porkchop_neighbor(navigation_points, 7, &navigation_nodes[0],
                       PORKCHOP_UP, 0, &selectable_range, 100, 100), 3);
    expect_int("down arrow selects the nearest visible node below",
               find_directional_porkchop_neighbor(navigation_points, 7, &navigation_nodes[0],
                       PORKCHOP_DOWN, 0, &selectable_range, 100, 100), 4);

    Body earth = {.name = "Earth"};
    Body mars = {.name = "Mars"};
    Body venus = {.name = "Venus"};
    Body jupiter = {.name = "Jupiter"};
    Body *system_bodies[] = {&earth, &mars, &venus, &jupiter};
    CelestSystem system = {.num_bodies = 4, .bodies = system_bodies};
    struct ItinStep departure = {.body = &earth};
    struct ItinStep flyby = {.body = &venus, .prev = &departure};
    struct ItinStep arrival = {.body = &mars, .prev = &flyby};
    int body_visibility[] = {0, 0, 0, 1};
    mark_itinerary_bodies_visible(body_visibility, 4, &arrival, &system);
    expect_int("route marks departure body visible", body_visibility[0], 1);
    expect_int("route marks flyby body visible", body_visibility[2], 1);
    expect_int("route marks arrival body visible", body_visibility[1], 1);
    expect_int("route leaves unrelated body hidden", body_visibility[3], 0);


    struct PorkchopGroup group_list[] = {
        {.has_itin_inside_filter = 1, .show_group = 1},
        {.has_itin_inside_filter = 0, .show_group = 1},
        {.has_itin_inside_filter = 1, .show_group = 0}
    };
    struct PorkchopAnalyzerPoint group_points[] = {
        {.inside_filter = 1, .group = &group_list[2]},
        {.inside_filter = 1, .group = &group_list[0]}
    };
    expect_int("only groups with a filtered itinerary change when hiding all",
               set_porkchop_groups_visibility(group_list, 3, 0), 1);
    expect_int("hide all hides the groups inside the filter", group_list[0].show_group == 0 && group_list[2].show_group == 0, 1);
    expect_int("hide all leaves a group outside the filter alone", group_list[1].show_group, 1);
    expect_int("nothing is visible after hide all", find_first_visible_porkchop_point(group_points, 2), -1);
    expect_int("show all reports the groups it changed", set_porkchop_groups_visibility(group_list, 3, 1), 2);
    expect_int("the first visible point follows the shown groups", find_first_visible_porkchop_point(group_points, 2), 0);
    group_list[2].show_group = 0;
    expect_int("a single group toggle leaves the others shown", group_list[0].show_group, 1);
    expect_int("the first visible point skips the hidden group", find_first_visible_porkchop_point(group_points, 2), 1);
    // Toggling must not reorder or reallocate the list, or the rebuilt rows would reset the scroll position.
    struct PorkchopGroup *list_before = group_list;
    set_porkchop_groups_visibility(group_list, 3, 0);
    set_porkchop_groups_visibility(group_list, 3, 1);
    expect_int("toggling groups keeps the list and its point links in place", list_before == group_list && group_points[0].group == &group_list[2] && group_points[1].group == &group_list[0], 1);
    expect_int("a missing group list changes nothing", set_porkchop_groups_visibility(NULL, 3, 1), 0);

    struct PorkchopPlotRange plot_range = {20, 80, 20, 80};
    struct PorkchopPlotRange plot_bounds = {0, 100, 0, 100};
    expect_int("porkchop plot range accepts increasing finite bounds", porkchop_plot_range_is_valid(&plot_range), 1);
    struct PorkchopPlotRange axis_switch_range = {100, 200, 10, 30};
    expect_int("duration to arrival axis preserves x range and plot span",
               porkchop_plot_switch_y_axis(&axis_switch_range, PA_YAXIS_DUR, PA_YAXIS_ARRDATE), 1);
    expect_near("duration to arrival axis keeps departure range",
                axis_switch_range.min_x, 100, 0);
    expect_near("duration to arrival axis translates y range around departure midpoint",
                axis_switch_range.min_y, 160, 0);
    expect_near("duration to arrival axis preserves y range span",
                axis_switch_range.max_y - axis_switch_range.min_y, 20, 0);
    expect_int("arrival to duration axis restores the view range",
               porkchop_plot_switch_y_axis(&axis_switch_range, PA_YAXIS_ARRDATE, PA_YAXIS_DUR), 1);
    expect_near("axis switching preserves the original y minimum",
                axis_switch_range.min_y, 10, 1e-12);
    expect_near("axis switching preserves the original y maximum",
                axis_switch_range.max_y, 30, 1e-12);
    porkchop_plot_pan(&plot_range, &plot_bounds, 0.5, -1.0);
    expect_int("porkchop plot pan stays inside full data bounds",
               plot_range.min_x == 0 && plot_range.max_x == 60 &&
               plot_range.min_y == 0 && plot_range.max_y == 60, 1);

    plot_range = (struct PorkchopPlotRange) {0, 100, 10, 110};
    struct PorkchopPlotRange zoom_bounds = {0, 200, 0, 200};
    porkchop_plot_zoom(&plot_range, &zoom_bounds, 0.5, 0.25, 0.75);
    expect_int("porkchop plot zoom keeps its cursor anchor fixed",
               plot_range.min_x == 12.5 && plot_range.max_x == 62.5 &&
               plot_range.min_y == 47.5 && plot_range.max_y == 97.5, 1);
    double x_value = 0, y_value = 0;
    expect_int("porkchop plot converts cursor fractions to data values",
               porkchop_plot_value_at_fraction(&plot_range, 0.25, 0.75, &x_value, &y_value) &&
               x_value == 25 && y_value == 85, 1);
    expect_int("porkchop plot rejects a degenerate range",
               porkchop_plot_value_at_fraction(&(struct PorkchopPlotRange) {1, 1, 0, 1}, 0.5, 0.5, &x_value, &y_value), 0);
    plot_range = (struct PorkchopPlotRange) {20, 80, 0, 60};
    porkchop_plot_zoom(&plot_range, &plot_bounds, 2.0, 0.5, 0.0);
    expect_int("zooming out at the duration boundary stays within data bounds",
               plot_range.min_x == 0 && plot_range.max_x == 100 &&
               plot_range.min_y == 0 && plot_range.max_y == 100, 1);
    plot_range = (struct PorkchopPlotRange) {0, 100, 0, 100};
    expect_int("box zoom selects the chosen data rectangle",
               porkchop_plot_zoom_to_box(&plot_range, 0.2, 0.6, 0.1, 0.7) &&
               plot_range.min_x == 20 && plot_range.max_x == 60 &&
               plot_range.min_y == 10 && plot_range.max_y == 70, 1);
    porkchop_plot_pan(&plot_range, &plot_bounds, -0.5, -0.5);
    expect_int("panning after box zoom can move beyond the selected rectangle",
               plot_range.min_x == 40 && plot_range.max_x == 80 &&
               plot_range.min_y == 0 && plot_range.max_y == 60, 1);

    struct PorkchopPoint dv_point = {
        .dv_dep = 100,
        .dv_dsm = 20,
        .dv_flyby = 30,
        .dv_arr_cap = 40,
        .dv_arr_circ = 50
    };
    expect_int("flyby delta-v excludes arrival capture and circularization",
               porkchop_point_total_dv(dv_point, TF_FLYBY) == 150, 1);
    expect_int("capture delta-v includes arrival capture",
               porkchop_point_total_dv(dv_point, TF_CAPTURE) == 190, 1);
    expect_int("circularization delta-v includes arrival circularization",
               porkchop_point_total_dv(dv_point, TF_CIRC) == 200, 1);
}
