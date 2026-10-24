# KMAT Copilot Instructions

Before changing code, read the relevant project documentation in [`docs/`](../docs/) and check the project overview in [`README.md`](../README.md). Keep documentation accurate when behavior or usage changes. The README does not need updating after every logic change; update it when a change makes existing README information incorrect or changes user-facing features, setup, or usage details that the README covers.

Keep tutorial and workflow documents focused on one topic. Do not append unrelated tutorials to an existing Markdown document; create a separate file in `docs/` when a distinct guide is needed, and link it from the README or relevant documentation.

For stable UI tooltips, reuse [`set_stable_tooltip`](../GUI/gui_manager.h) or `set_stable_tooltip_markup` from `GUI/gui_manager.c`; avoid native GTK tooltip APIs for hover content that changes, since they can flicker or obstruct pointer interaction.

Use the workspace-root `Itineraries/` folder for local itinerary fixtures and performance reproductions. This local data may not be present in a clean clone.

## Project conventions

- KMAT is a C project using C11 and CMake. Follow the style and patterns of nearby code.
- Keep changes focused; avoid unrelated refactoring and changes to vendored code under `external/`.
- Prefer existing helpers and tests over introducing duplicate logic.
- After a feature branch is successfully merged, delete its local branch. Delete a remote branch only when explicitly requested.

## Build and test

- Configure with `cmake --preset default` and build with `cmake --build --preset default`.
- Run tests with `ctest --test-dir build --output-on-failure`; use CTest labels or test-name filters to run only relevant tests when possible.
- Add or update unit tests for every code or behavior change. Add integration tests when a change affects interactions between components, end-to-end workflows, or behavior that unit tests cannot meaningfully cover.
- The CTest suite includes unit tests and integration tests. Run the relevant tests and report any that could not be run.
