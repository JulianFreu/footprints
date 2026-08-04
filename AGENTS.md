# AGENTS

This is supposed to be a lean application with as little dependencies as possible. When making 
changes try to modify lines only when necessary. Less lines changed is better.

## Build

Build targets:

| Target             | What it does                                                        |
|--------------------|---------------------------------------------------------------------|
| `make`             | Optimised incremental build (`-O2 -Wall -Wextra`)                    |
| `make debug`       | `footprints-debug` with AddressSanitizer + UBSan                     |
| `make test`        | Build and run the unit tests, with both sanitizers on                |
| `make format`      | Apply `.clang-format` to all non-vendored sources                    |
| `make check-format`| Fail if anything is unformatted                                      |
| `make run`         | Build, then run                                                      |
| `make clean`       | Remove build output                                                  |

The build should be warning-free. `src/clay.h` and `src/clay_renderer_sdl.c` are
vendored third-party code: they are excluded from formatting, compiled only via
`src/clay_sdl.c`, and should not be hand-edited.

## Development

### Tests

```bash
make test
```

The harness is about eighty lines of macros over a pair of counters — no
framework, so `make test` needs nothing that `make` does not already need. It
builds with AddressSanitizer and UBSan on, since the suites cover the
hand-rolled parsing and buffer arithmetic where an out-of-bounds read would
otherwise go unnoticed.

Each file under `tests/` `#include`s the module it covers rather than linking
it, so a module's `static` helpers are reachable without widening its
interface. A module is therefore included by at most one test file. The suite
links no SDL, which is also the line the coverage follows: the modules that are
only arithmetic are tested, and the ones that draw are not — so a question
worth testing belongs on the pure side of the split (`stats.c` rather than
`ui_stats.c`, `filters.c` rather than `ui_filters.c`).

Covered: timestamp parsing, the tile queue, the filter table, the digits a
field is typed with and the text and bound derived from them, the predicate the
table drives, the parser's geometry and elevation maths, the display formatting,
the calendar bucketing and the metrics reduced over it, the sliding window
that finds a record distance inside a longer track, the records table and its
exclusions, the k-d tree radius
search (against brute force), the spatial index (against the full scan it
replaced), the background job's concurrency contract, the Garmin import job's
contract, and the animation primitive's easing and timing.

`tests/test_garmin.c` points the two macros naming the import helper at
`tests/fake_garmin.sh`, so the fork, the pipes, the line protocol and the exit
codes are covered without a Garmin account. It runs the binary from the project
root, which is where `make test` runs it from.

For threading changes, build the suite with ThreadSanitizer instead:

```bash
cc -O1 -g -fsanitize=thread -I$(xml2-config --cflags | sed 's/-I//') \
   tests/*.c -o run-tests-tsan -lxml2 -lm -lpthread && ./run-tests-tsan
```

### Code layout

| File | Responsibility |
|------|----------------|
| `main.c` | Startup, the frame loop, SDL setup and teardown, event routing |
| `anim.c` | The animated scalar every moving thing is built from, and its easing |
| `zoom.c` | The gap between the zoom level the model is at and the one being drawn |
| `gpx_parser.c` | Reads `gpx_files/`, builds the track collection, derives per-track stats |
| `filters.c` | The filter table: what each filter reads, how its digits are laid out and weighted, which tracks pass |
| `track_sort.c` | Ordering the run list |
| `track_format.c` | Turning a track's numbers into the strings shown beside them |
| `track_splits.c` | The fastest 5k/10k/half/marathon found anywhere inside one track |
| `gpx_activity.c` | The one place activity types are spelled for display |
| `background.c` | Runs the library scan and the heat calculation off the main thread |
| `garmin.c` | Runs `garmin_sync.py`, feeds it credentials and reads its progress back |
| `heat.c` | The implicit k-d tree and the threaded heat calculation |
| `point_index.c` | Tile-keyed spatial index over every point, shared by all zooms |
| `tracks.c` | Track rendering: heat tiles, the selected-track overlay, elevation profiles |
| `map.c` | The world-to-screen projection, tile URLs, the download thread, the tile texture cache, map background |
| `fifo.c` | The bounded queue between the main loop and the download thread |
| `time_util.c` | The single place timestamps are parsed — everything is UTC |
| `stats.c` | Bucketing tracks into days, weeks, months and years, and the metrics reduced over them |
| `records.c` | Which activities hold each personal record, ranked over the whole collection |
| `ui.c` | Clay setup, icons, the left menu bar, sidebar, and the composition of the panels |
| `ui_filters.c` | The filter panel and the text input that feeds it |
| `ui_runlist.c` | The run list: sortable header, rows, virtualised scrolling |
| `ui_stats.c` | The statistics panel: the bar plot, its axes, its buttons, and the pan |
| `ui_records.c` | The records panel: a scrolling section per category, its rows clickable |
| `ui_settings.c` | The settings panel, its fields and the text input that feeds them |
| `ui_garmin.c` | The Garmin panel: the account, the import button, and what the import is doing |
| `ui_internal.h` | Layout vocabulary shared by `ui*.c`; not part of the UI's interface |
| `settings.c` | What is configurable, its defaults, and reading and writing `settings.conf` |
| `progress.h` | How a long operation reports progress and is asked to stop |
| `clay_sdl.c` | The one translation unit carrying Clay and its vendored SDL renderer |
| `*_types.h` | Types only, so a module can include what it needs without the rest |
| `config.h` | Compile-time tunables, and the defaults the settings start from |

### Conventions

- Functions, struct members and locals are `snake_case`; typedef'd types are
  `PascalCase`; macros and constants are `UPPER_SNAKE`.
- Header guards are `UPPER_SNAKE` matching the filename, e.g. `GPX_PARSER_H`.
- Anything not declared in a header is `static`. Running `nm` on a module's
  object file should show its interface and nothing else.
- A header includes only what its own declarations need; the `.c` file includes
  what it uses.
- Every timestamp is UTC, and is parsed through `time_util.h`.
- Laying out the UI does not mutate the model it is drawing. Anything that
  changes state happens on the event that caused it, or in the update pass that
  runs before the layout.
- Anything that moves over time is an `Anim` from `anim.h`, and asks for the
  next frame itself through `app_request_redraw`. The frame loop does not know
  what the program can animate, and nothing has to be added to it.
- World coordinates go through `map_world_to_screen` and `map_screen_to_world`
  rather than the shift they are built on. `conv_pixel_to_tile_and_offset` is
  the exception: tile-space arithmetic is integer on purpose.
- Tiles are chosen, fetched and rasterised at whole zoom levels. Only the blit
  knows about `MapTransform`, which is how the picture is allowed to sit
  between two levels without anything else having to.
- Clay keeps a pointer to the text it is given rather than copying it, so
  anything drawn must outlive the layout pass. Use `ui_frame_printf` or
  `ui_track_text`, never a local buffer.
- `CLAY()` evaluates its id before it opens the element, so the parent
  `CLAY_ID_LOCAL` and `CLAY_IDI_LOCAL` seed with is the enclosing element's own
  parent, not the enclosing element. A helper drawing the same labels inside
  each of several sibling rows therefore needs an index unique across the rows,
  not just within one — otherwise every row asks for the same ids and Clay
  reports each of them as already declared.
- While a background job is running the collection belongs to its worker. The
  main thread does not read or draw it until `background_collect` says so.
- Comments describe what the code does now. Git history holds what it used to do.
- Only stdout output a user asked for is unconditional; diagnostics go through
  `LOG_DEBUG` in `log.h` (build with `-DFOOTPRINTS_DEBUG_LOG` to see them).
- Run `make format` before committing; `make check-format` and `make test` must
  pass.