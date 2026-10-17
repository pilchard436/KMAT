cmake_minimum_required(VERSION 3.26)
project(KMAT C)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
include(CTest)
option(KMAT_ENABLE_SEARCH_PROFILE "Enable detailed per-body itinerary search profiling" OFF)

if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/O2>)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<CONFIG:Release>:-O3>)
endif()

include_directories(.)

add_subdirectory(external/orbitlib)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(external/orbitlib/src/transfer.c PROPERTIES
        COMPILE_OPTIONS "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-ffast-math>"
    )
elseif(MSVC)
    set_source_files_properties(external/orbitlib/src/transfer.c PROPERTIES
        COMPILE_OPTIONS "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/fp:fast>"
    )
endif()

set(KMAT_FILE_IO_SOURCES
    tools/file_io.c
    tools/file_io_system.c
    tools/file_io_steps.c
    tools/file_io_itins.c
    tools/file_io_itin.c
)

set(KMAT_ITINERARY_SOURCES
    orbit_calculator/itin_tool.c
    orbit_calculator/itinerary_tree.c
    orbit_calculator/itinerary_text.c
    orbit_calculator/flyby_policy.c
    orbit_calculator/flyby_search.c
    orbit_calculator/leg_variants.c
    orbit_calculator/resonant_flybys.c
)

add_executable(KMAT
    ${KMAT_FILE_IO_SOURCES}
    tools/file_io_format.c
    tools/itinerary_date_limits.c
    tools/itinerary_reference_defaults.c
    tools/itinerary_reference_defaults.h
    tools/starting_orbit.c
    tools/starting_orbit.h
    tools/file_io.h
    launch_calculator/launch_calculator.c
    launch_calculator/launch_calculator.h
    launch_calculator/lv_profile.c
    launch_calculator/lv_profile.h
    main.c
    tools/tool_funcs.c
    tools/tool_funcs.h
    launch_calculator/launch_circularization.c
    launch_calculator/launch_circularization.h
    launch_calculator/lp_parameters.c
    launch_calculator/lp_parameters.h
    orbit_calculator/transfer_calc.c
    orbit_calculator/transfer_search_worker.c
    orbit_calculator/transfer_search_progress.c
    orbit_calculator/transfer_calc.h
    orbit_calculator/powered_flyby_optimizer.c
    orbit_calculator/powered_route_objective.c
    orbit_calculator/powered_route_dsm.c
    orbit_calculator/powered_route_candidates.c
    orbit_calculator/powered_flyby_optimizer.h
    launch_calculator/lp_parameters.h
    tools/thread_pool.c
    tools/thread_pool.h
    gui/drawing_3d.c
    gui/drawing_2d.c
    gui/drawing_primitives.c
    gui/drawing_plots.c
    gui/drawing.h
    tools/thread_pool.h
    database/database.c
    database/database.h
    ${KMAT_ITINERARY_SOURCES}
    orbit_calculator/itin_tool.h
    gui/transfer_app/transfer_planner.c
    gui/transfer_app/transfer_planner_view.c
    gui/transfer_app/transfer_planner_panel.c
    gui/transfer_app/transfer_planner_io.c
    gui/transfer_app/transfer_planner.h
    gui/transfer_app/porkchop_analyzer.c
    gui/transfer_app/porkchop_analyzer_groups.c
    gui/transfer_app/porkchop_analyzer_filter.c
    gui/transfer_app/porkchop_analyzer_io.c
    gui/transfer_app/porkchop_analyzer.h
    gui/transfer_app/porkchop_analyzer_model.c
    gui/transfer_app/porkchop_analyzer_model.h
    gui/transfer_app/porkchop_analyzer_tools.c
    gui/transfer_app/porkchop_analyzer_tools.h
    gui/transfer_app/sequence_calculator.c
    gui/transfer_app/sequence_calculator.h
    launch_calculator/launch_sim.c
    launch_calculator/launch_sim.h
    launch_calculator/launch_state.c
    launch_calculator/launch_state.h
    gui/launch_app/launch_analyzer.c
    gui/launch_app/launch_analyzer.h
    database/lv_database.c
    database/lv_database.h
    gui/launch_app/capability_analyzer.c
    gui/launch_app/capability_analyzer.h
    launch_calculator/capability_calculator.c
    launch_calculator/capability_calculator.h
    gui/launch_app/launch_parameter_analyzer.c
    gui/launch_app/launch_parameter_analyzer.h
    gui/database_app/mission_db.c
    gui/database_app/mission_db.h
    database/mission_database.c
    database/mission_database.h
    gui/css_loader.c
    gui/css_loader.h
    gui/database_app/database_app_tools/mission_db_tools.c
    gui/database_app/database_app_tools/mission_db_tools.h
    gui/database_app/database_app_tools/mission_manager_tools.c
    gui/database_app/database_app_tools/mission_manager_tools.h
    tools/gmat_interface.c
    tools/gmat_interface.h
    gui/transfer_app/itinerary_calculator.c
    gui/transfer_app/itinerary_calculator_dates.c
    gui/transfer_app/itinerary_calculator_run.c
    gui/transfer_app/itinerary_calculator.h
    gui/transfer_app/starting_orbit_editor.c
    gui/transfer_app/starting_orbit_editor.h
    gui/gui_manager.c
    gui/gui_manager.h
    gui/info_win_manager.c
    gui/info_win_manager.h
    gui/settings.c
    gui/settings.h
    00_Testing/testing.c
    00_Testing/testing.h
    00_Testing/testing_algorithms.c
    00_Testing/testing_algorithms.h
    gui/gui_tools/camera.c
    gui/gui_tools/camera.h
    gui/gui_tools/screen.c
    gui/gui_tools/screen.h
    tools/version_tool.c
    tools/version_tool.h
    tools/celestial_systems.c
)

add_executable(KMATUnitTests
    00_Testing/unit_tests.c
    tools/file_io_format.c
    tools/itinerary_date_limits.c
    tools/itinerary_reference_defaults.c
    tools/starting_orbit.c
    gui/transfer_app/porkchop_analyzer_model.c
    tools/tool_funcs.c
)

target_link_libraries(KMATUnitTests PRIVATE orbitlib geometrylib)
target_include_directories(KMATUnitTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/external/geometrylib/include
)

add_executable(KMATItineraryUnitTests
    00_Testing/unit_tests_itinerary.c
    ${KMAT_ITINERARY_SOURCES}
    ${KMAT_FILE_IO_SOURCES}
    tools/file_io_format.c
    tools/starting_orbit.c
    tools/thread_pool.c
    tools/tool_funcs.c
)
target_link_libraries(KMATItineraryUnitTests PRIVATE orbitlib geometrylib)
target_include_directories(KMATItineraryUnitTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/external/geometrylib/include
)

add_executable(KMATOptimizerUnitTests
    00_Testing/unit_tests_optimizer.c
    orbit_calculator/powered_route_candidates.c
    ${KMAT_ITINERARY_SOURCES}
    tools/starting_orbit.c
    tools/thread_pool.c
    tools/tool_funcs.c
)
target_link_libraries(KMATOptimizerUnitTests PRIVATE orbitlib geometrylib)
target_include_directories(KMATOptimizerUnitTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/external/geometrylib/include
)

add_executable(KMATLambertBenchmark 00_Testing/lambert_benchmark.c)
target_link_libraries(KMATLambertBenchmark PRIVATE orbitlib geometrylib)
target_include_directories(KMATLambertBenchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/external/geometrylib/include
)

add_executable(KMATSearchIntegration
    00_Testing/itinerary_search_integration.c
    00_Testing/integration_search.c
    00_Testing/integration_starting_orbit.c
    00_Testing/integration_benchmarks.c
    00_Testing/integration_operations.c
    00_Testing/integration_flybys.c
    orbit_calculator/transfer_calc.c
    orbit_calculator/transfer_search_worker.c
    orbit_calculator/transfer_search_progress.c
    orbit_calculator/powered_flyby_optimizer.c
    orbit_calculator/powered_route_objective.c
    orbit_calculator/powered_route_dsm.c
    orbit_calculator/powered_route_candidates.c
    ${KMAT_ITINERARY_SOURCES}
    tools/starting_orbit.c
    ${KMAT_FILE_IO_SOURCES}
    tools/file_io_format.c
    tools/thread_pool.c
    tools/tool_funcs.c
)
target_link_libraries(KMATSearchIntegration PRIVATE orbitlib geometrylib)
target_include_directories(KMATSearchIntegration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/external/geometrylib/include
)

if(KMAT_ENABLE_SEARCH_PROFILE)
    target_compile_definitions(KMAT PRIVATE KMAT_ENABLE_SEARCH_PROFILE)
    target_compile_definitions(KMATSearchIntegration PRIVATE KMAT_ENABLE_SEARCH_PROFILE)
endif()

# Platform-specific setup
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        message(STATUS "Configuring Cross-Compilation for Windows (MinGW-w64)")
        set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
        set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)

        # Ensure MinGW headers are used
        include_directories(/usr/x86_64-w64-mingw32/include)
        link_directories(/usr/x86_64-w64-mingw32/lib)

        # GTK3 installation directory (modify if needed)
        set(GTK3_ROOT "/usr/x86_64-w64-mingw32/gtk" CACHE PATH "GTK3 installation directory")
    else()
        message(STATUS "Configuring for Windows (native MinGW)")
        set(GTK3_ROOT "C:/msys64/mingw64" CACHE PATH "GTK3 installation directory")
    endif ()

    include_directories(
        ${GTK3_ROOT}/include
        ${GTK3_ROOT}/include/gtk-3.0
        ${GTK3_ROOT}/include/glib-2.0
        ${GTK3_ROOT}/lib/glib-2.0/include
        ${GTK3_ROOT}/include/pango-1.0
        ${GTK3_ROOT}/include/cairo
        ${GTK3_ROOT}/include/gdk-pixbuf-2.0
        ${GTK3_ROOT}/include/atk-1.0
        ${GTK3_ROOT}/include/harfbuzz
    )

    # Explicit GTK3 library directories
    link_directories(
        ${GTK3_ROOT}/lib
        ${GTK3_ROOT}/lib/gtk-3.0
        ${GTK3_ROOT}/lib/glib-2.0
    )

    target_link_libraries(KMAT
        orbitlib
        gtk-3
        gdk-3
        gobject-2.0
        glib-2.0
        pango-1.0
        cairo
        gdk_pixbuf-2.0
        atk-1.0
        gio-2.0
        urlmon
        sqlite3
        wininet
    )

    target_link_libraries(KMATUnitTests PRIVATE urlmon)
    target_link_libraries(KMATSearchIntegration PRIVATE urlmon glib-2.0 gobject-2.0)
    target_link_libraries(KMATItineraryUnitTests PRIVATE urlmon glib-2.0 gobject-2.0)
    target_link_libraries(KMATOptimizerUnitTests PRIVATE urlmon)

    target_include_directories(KMAT PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    )


    set_target_properties(KMAT PROPERTIES WIN32_EXECUTABLE FALSE)

else(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Configuring for Linux (GCC)")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

    include_directories(${GTK3_INCLUDE_DIRS})
    link_directories(${GTK3_LIBRARY_DIRS})
    add_definitions(${GTK3_CFLAGS_OTHER})
    target_link_libraries(KMAT m ${GTK3_LIBRARIES} sqlite3 orbitlib)
    target_link_libraries(KMATSearchIntegration PRIVATE ${GTK3_LIBRARIES})
    target_link_libraries(KMATItineraryUnitTests PRIVATE ${GTK3_LIBRARIES})

    target_link_options(KMAT PRIVATE -rdynamic)

    target_include_directories(KMAT PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    )

endif()

if(BUILD_TESTING)
    add_test(NAME KMATIntegrationTests COMMAND KMAT --test)
    add_test(NAME KMATSearchIntegration COMMAND KMATSearchIntegration)
    add_test(NAME KMATUnitTests COMMAND KMATUnitTests)
    add_test(NAME KMATItineraryUnitTests COMMAND KMATItineraryUnitTests)
    add_test(NAME KMATOptimizerUnitTests COMMAND KMATOptimizerUnitTests)
    set_tests_properties(KMATIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LABELS integration
    )
    set_tests_properties(KMATUnitTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LABELS unit
    )
    set_tests_properties(KMATOptimizerUnitTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LABELS unit
    )
    set_tests_properties(KMATItineraryUnitTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LABELS unit
    )
    set_tests_properties(KMATSearchIntegration PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LABELS integration
    )

    add_custom_target(run_tests ALL
        COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure -L unit
        DEPENDS KMATUnitTests KMATItineraryUnitTests KMATOptimizerUnitTests
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        USES_TERMINAL
    )

    add_custom_target(run_integration_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure -L integration
        DEPENDS KMAT KMATSearchIntegration
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        USES_TERMINAL
    )
endif()
