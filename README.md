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
- **Import straight from Garmin Connect** — log in once in the Garmin panel and pull down every activity you have not already got
- Convert **Garmin `.fit` and `.tcx` files** to `.gpx` with the included Python tool

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/JulianFreu/footprints.git
cd footprints
```

### 2. Install build dependencies

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

Run the build with `make`

### 4. Run the application

After building, run the executable directly from the project root:

```bash
./footprints
```

Run it from the project root — `gpx_files/`, `tilecache/`, `resources/` and
`settings.conf` are all resolved relative to the working directory.

### Map tiles

Footprints uses OpenStreetMap tiles by default, which need no API key and no
extra setup.

Four Stadia Maps styles are available as well — Stamen Terrain, Stamen Toner
Lite, Alidade Smooth Dark and Outdoors. They need a key: open the settings
panel, paste it into **Stadia API key**, and the styles become selectable.
Both the key and the choice of provider are saved, so this is a one-time step.

Each provider caches into a directory of its own under `tilecache/`, so
switching between them does not mix one provider's tiles into another's.

TODO: delete the -stadiamaps option so it has to be selected in settings

`./footprints -stadiamaps` still works, and now means "start on Stamen Terrain
this once" without changing what is saved. It needs a key to already be set.

## Settings

Everything in the settings panel is written to `settings.conf` in the working
directory as soon as it changes, and read back at startup. The file is plain
text and hand-editing it is fine — anything unreadable or out of range falls
back to the default rather than stopping the program.

| Setting | What it does |
|---------|--------------|
| Heat colours | Six setpoints shaping where a given amount of heat lands on the colour ramp |
| Map | The tile provider, the Stadia API key, and a button to drop the cached tile textures |
| Heat calculation | The overlap radius the heat is calculated with, and the size of the square drawn per track point |
| Library | The folder scanned for `.gpx` files, and a button to read it again |
| Garmin Connect | In a panel of its own: the account to import from, and the button that does it |
| Startup view | Saves wherever the map is now as the view the window opens at |

### Heat colours

The heat colouring is a smooth 32-colour ramp from cold to hot, and stays one.
What the six setpoints change is *where along that ramp* a given amount of heat
lands. Each is a percentage of the maximum heat in your library; setpoint 1 is
where the coldest colour sits and setpoint 6 where the hottest is reached, with
the four between them spaced evenly along the ramp. The colours between two
setpoints are still interpolated — the setpoints bend the curve, they do not
band it.

The default `0,20,40,60,80,100` is a straight line. Setting the last one to 60
means everything at 60% of the maximum heat and above is drawn in the hottest
colour, which spreads the range over your busier routes; setting it to 90
reserves the hottest colour for the few places you go most. The gradient bar
above the fields previews the result.

A change here only re-draws the map. Changing the **overlap radius** is
different — it feeds the calculation itself, so the *Recalculate heat* button
lights up while the setting and the map disagree, and pressing it re-runs the
calculation.

TODO: delete this part also in code. The settings menu should be the only place to configure stadiamaps
If `src/api_key.h` is left over from before the settings panel existed, its key
is read once and carried into the settings; the header is no longer needed.

## Usage

At startup, Footprints scans the `gpx_files/` directory and automatically loads all GPX files it finds there.
So your first step should be to copy your GPX files into that folder.

Subfolders are scanned too, so a library can be filed by year, by activity, or
however else you like — and it is what lets the Garmin import keep to a folder
of its own.

### Importing from Garmin Connect

The **Grmn** button in the menu bar opens the Garmin panel. Enter the email
address and password of your Garmin Connect account and press **Log in**. If
your account uses two-factor authentication, Garmin sends a code, a **Code**
box appears, and pressing *Log in* again with it finishes the job.

That happens once. The password is traded for an OAuth token, which is saved to
`garmin_session/`, and every import after that runs off the token — the password
is never written anywhere, and is dropped from memory as soon as it has been
used.

**Import new activities** then downloads every activity that is not already on
disk, as GPX, into `gpx_files/garmin_import/`. The panel counts them down as
they arrive, and the library is read again as soon as they have landed. Running
it a second time fetches only what is new: each file is named after the activity
it came from, so the import can see what it already has without keeping a record
of its own.

Activities are downloaded through Garmin's own GPX export. The activity type it
reports is written into the `<type>` element described below, so a trail run
imports as a run rather than as `Other`.

This needs `garth-ng`, which is in `requirements.txt`:

```bash
pip install -r requirements.txt
```

### Converting a Garmin data export

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

Both Python tools are optional, and so are their packages — Footprints itself
reads `.gpx` files with no Python at all.

| Package | Needed for |
|---------|------------|
| `garth-ng` | The Garmin Connect import |
| `fitparse` | Converting `.fit` files (`.tcx` and `.gpx` need only the standard library) |

```bash
pip install -r requirements.txt
```

## ToDo

- Color overhaul
- Add watermark of tile provider to bottom right
- Add screenshots to README
- Export the current heatmap view as a PNG for sharing

## License
This project is licensed under the MIT License – see the [LICENSE](LICENSE) file for details.
