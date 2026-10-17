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

}
