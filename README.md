# Footprints

Footprints turns your GPX tracks into an interactive personal heatmap.
Visualize all your runs, hikes, and rides in one place, explore your most frequented routes, and filter activities by distance, duration, pace and more.

## Features

- Generate a personal **heatmap** to see where you’ve spent the most time
- View **all your GPX tracks** together on one interactive map
- **Filter** activities by distance, pace, duration, elevation gain, and more
- **Click any track** for detailed stats and insights
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

Use the provided build script:

```bash
./build.sh
```

### 4. Run the application

After building, run the executable directly:

```bash
./footprints
```

## Usage

At startup, Footprints scans the `gpx_files/` directory and automatically loads all GPX files it finds there.
So your first step should be to copy your GPX files into that folder.

If you use a Garmin watch, you can request a full data export from Garmin.
The export will contain your recorded activities as `.fit` files, usually bundled in one or more ZIP archives.

You can use the included Python script to convert these `.fit` files to `.gpx` format.
During conversion, the script also injects the detected activity type into each GPX file.
This extra field is not part of the GPX standard but is required by Footprints to correctly identify the activity type.

```bash
python convert_fit_to_gpx.py <src_folder> gpx_files/
```

### Python script dependencies

To use the conversion script, install its dependencies first:

```bash
pip install fitparse lxml
```

## License
This project is licensed under the MIT License – see the [LICENSE](LICENSE) file for details.
