# Footprints

Footprints turns your GPX tracks into an interactive personal heatmap.
Visualize all your runs, hikes, and rides in one place, explore your most frequented routes, and filter activities by distance, duration, pace and more.

## Features

- Generate a personal **heatmap** to see where you’ve spent the most time
- View **all your GPX tracks** together on one interactive map
- **Filter** activities by distance, pace, duration, elevation gain, and more — the list, the charts, the records and the map all follow as you type
- **Click any track** for detailed stats and insights
- **Chart your training** by day, week, month or year — up to two of total distance, longest run, total time and total ascent at once, and scroll the plot back through the years
- **Track your personal records** — longest run and activity, highest peak, most elevation gain, and your fastest 5k, 10k, half marathon and marathon, found anywhere inside a longer run rather than only from its start
- Convert **Garmin `.fit` and `.tcx` files** to `.gpx` with the included Python tool

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/JulianFreu/footprints.git
cd footprints
```

### 2. Install build dependencies

SDL 2.0.10 or newer is required; every current distribution is well past it.

Ubuntu / Debian:

```bash
sudo apt update
sudo apt install build-essential libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libcurl4-openssl-dev libxml2-dev
```

Fedora:

```bash
sudo dnf install gcc make SDL2-devel SDL2_image-devel SDL2_ttf-devel libcurl-devel libxml2-devel
```

### 3. Build from source

```bash
make
```

Other targets:

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

### 4. Run the application

After building, run the executable directly:

```bash
./footprints
```

Run it from the project root — `gpx_files/`, `tilecache/` and `resources/` are
all resolved relative to the working directory.

### Map tiles

Footprints uses OpenStreetMap tiles by default, which need no API key and no
extra setup.

To use Stadia Maps terrain tiles instead, supply a key and pass the flag:

```bash
cp src/api_key.h.example src/api_key.h
# paste your key into src/api_key.h, then
make && ./footprints -stadiamaps
```

`src/api_key.h` is gitignored, so your key stays out of version control. It is
optional: without it the build succeeds as normal and only `-stadiamaps` is
unavailable, which the program tells you if you ask for it.

## Usage

At startup, Footprints scans the `gpx_files/` directory and automatically loads all GPX files it finds there.
So your first step should be to copy your GPX files into that folder.

If you use a Garmin watch, you can request a full data export from Garmin.
The export will contain your recorded activities as `.fit` files, usually bundled in one or more ZIP archives.

You can use the included Python script to convert these `.fit` files to `.gpx` format:

```bash
python convert_fit_to_gpx.py <src_folder> gpx_files/
```

It also accepts `.tcx` files, and copies `.gpx` files through unchanged, in
every case renaming the output with the activity's start time so the directory
sorts chronologically.

During conversion the script injects the detected activity type as a `<type>`
element. That element is not part of the GPX standard — it is how Footprints
tells a run from a ride. GPX files from other tools load fine without it; their
activity type simply reads as `Other`.

### Python script dependencies

Only `.fit` files need a third-party package. `.tcx` and `.gpx` are handled with
the standard library alone, so you only need this if you are converting `.fit`:

```bash
pip install -r requirements.txt
```

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
replaced), the background job's concurrency contract, and the animation
primitive's easing and timing.

For threading changes, build the suite with ThreadSanitizer instead:

```bash
cc -O1 -g -fsanitize=thread -I$(xml2-config --cflags | sed 's/-I//') \
   tests/*.c -o run-tests-tsan -lxml2 -lm -lpthread && ./run-tests-tsan
```

### Branch naming

`type/short-description`, e.g. `feat/tile-fade-in`, `fix/fifo-oob-write`,
`chore/update-deps`. Types: `feat`, `fix`, `chore`, `docs`, `refactor`, `test`.
Keep the description a few kebab-case words, not a full sentence.

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
| `ui_panels.c` | The settings panel, and the shape a panel takes before it has content |
| `ui_internal.h` | Layout vocabulary shared by `ui*.c`; not part of the UI's interface |
| `progress.h` | How a long operation reports progress and is asked to stop |
| `clay_sdl.c` | The one translation unit carrying Clay and its vendored SDL renderer |
| `*_types.h` | Types only, so a module can include what it needs without the rest |
| `config.h` | Compile-time tunables |

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

### Adding a filter

The filters are described by one table in `filters.c`. A new filter needs a
value in the `FilterAttribute` enum in `filter_types.h` and a row in that
table saying how it is typed, which `GpxTrack` member each end of its range is
compared against, and what to label it.

Nothing in `ui.c` or `ui_filters.c` changes: the panel is drawn by looping over
the table, and the same table decides which tracks pass. If the filter reads
like an existing one (plain number, duration, pace, date) there is no new
parsing either.

A genuinely new *shape* of input is a second table, `format_specs`, beside the
first. A row there says how many digits the field holds and how they are
grouped from the least significant end — `{2, 2, 2}` weighted `{1, 60, 3600}`
is `HH:MM:SS`. The digits typed into a field are the only thing stored; the
string the field shows and the number the predicate compares are both derived
from them by `refresh_bound`, which is why they cannot drift apart.

### How a filter reaches the map

Editing a field applies at once. The keystroke writes the digits, and
`ui_filters_update` — the pass that runs before the layout, alongside the
statistics panel's — recomputes `visible_in_list` and marks the derived panels
stale. That part is a walk over the tracks and costs nothing.

Re-rasterising the heat overlay does not cost nothing, so it waits
`FILTER_SETTLE_SECONDS` for the next digit not to arrive. Typing a bound
rasterises the map once rather than once per keystroke, and a press with no
keystroke behind it — a type toggle, Clear All Filters — skips the wait.

What makes any of this affordable is that the point index spans **every** point
rather than the visible ones. Filtering it would mean sorting well over a
million entries on each change; instead the index is built once, off the main
thread, and whoever walks a range skips what is filtered out.

## ToDo

- More statistics in activities (heart-rate, speed, ...)
- Color overhaul
- Add watermark of tile provider to bottom right
- Add screenshots to README
- Export the current heatmap view as a PNG for sharing
- Interactive elevation profile on click instead of the current static image
- A settings file for defaults (start location/zoom, tile cache path) instead of hardcoded values in `config.h`
- A minimal config for map tile provider/API key setup instead of editing `api_key.h` by hand
- Auto-sync with Garmin Connect

## License
This project is licensed under the MIT License – see the [LICENSE](LICENSE) file for details.
