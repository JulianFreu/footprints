"""Convert Garmin .fit and .tcx exports into the .gpx files Footprints reads.

Each output track carries a <type> element naming the activity. That element is
not part of the GPX standard -- it is how Footprints tells a run from a ride --
so files converted here gain it and files from elsewhere are copied through
unchanged and fall back to "Other".

Only .fit files need a third-party package (fitparse); .tcx and .gpx are
handled with the standard library alone.
"""

import datetime
import os
import sys
import xml.etree.ElementTree as ET
from concurrent.futures import ProcessPoolExecutor, as_completed

SKIP_TYPES = {"unknown", "training", "swimming"}

# Semicircles to degrees: the .fit format stores coordinates as a signed 32-bit
# fraction of a half-turn.
SEMICIRCLE_TO_DEGREES = 180.0 / 2**31


# ----------------- Shared GPX construction -----------------
def build_gpx(name, activity_type):
    """An empty GPX document, returned with the <trkseg> its points go into."""
    gpx = ET.Element("gpx", version="1.1", creator="fit_tcx_to_gpx")
    metadata = ET.SubElement(gpx, "metadata")
    ET.SubElement(metadata, "time").text = utc_now_iso()

    trk = ET.SubElement(gpx, "trk")
    ET.SubElement(trk, "name").text = name
    ET.SubElement(trk, "type").text = activity_type
    return gpx, ET.SubElement(trk, "trkseg")


def add_trackpoint(trkseg, lat, lon, elevation=None, time_text=None):
    trkpt = ET.SubElement(trkseg, "trkpt", lat=f"{lat:.6f}", lon=f"{lon:.6f}")
    if elevation is not None:
        ET.SubElement(trkpt, "ele").text = f"{float(elevation):.1f}"
    if time_text:
        ET.SubElement(trkpt, "time").text = time_text
    return trkpt


def utc_now_iso():
    return (
        datetime.datetime.now(datetime.timezone.utc)
        .replace(microsecond=0, tzinfo=None)
        .isoformat()
        + "Z"
    )


def parse_timestamp(text):
    """An ISO8601 timestamp, or None if it is not one."""
    if not text:
        return None
    try:
        return datetime.datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None


def output_name(input_path, first_timestamp):
    """Prefixed with the start time so the directory sorts chronologically."""
    base_name = os.path.splitext(os.path.basename(input_path))[0]
    if first_timestamp is None:
        return f"{base_name}.gpx"
    return f"{first_timestamp.strftime('%Y-%m-%d-%H-%M-%S')}_{base_name}.gpx"


def write_gpx(gpx, input_path, first_timestamp, output_folder):
    out_name = output_name(input_path, first_timestamp)
    out_path = os.path.join(output_folder, out_name)
    os.makedirs(output_folder, exist_ok=True)

    ET.indent(gpx)  # pretty-print, stdlib equivalent of lxml's pretty_print
    ET.ElementTree(gpx).write(out_path, encoding="UTF-8", xml_declaration=True)
    return out_path


def skip(input_path, reason):
    print(f"skip {os.path.basename(input_path)} ({reason})")
    return None


# ----------------- FIT -----------------
def detect_fit_activity_type(fitfile):
    try:
        for record in fitfile.get_messages("session"):
            sport = record.get_value("sport")
            if sport:
                return str(sport).capitalize()
    except Exception:
        pass
    return "Unknown"


def fit_to_gpx(input_path, output_folder):
    # Imported here rather than at module scope so that converting .tcx and
    # .gpx files, which need nothing beyond the standard library, works
    # without fitparse installed.
    try:
        from fitparse import FitFile
    except ImportError:
        raise RuntimeError("reading .fit files needs fitparse: pip install fitparse")

    fitfile = FitFile(input_path)
    activity_type = detect_fit_activity_type(fitfile)
    if activity_type.lower() in SKIP_TYPES:
        return skip(input_path, f"activity type: {activity_type}")

    gpx, trkseg = build_gpx(os.path.basename(input_path), activity_type)

    first_timestamp = None
    valid_points = 0

    for record in fitfile.get_messages("record"):
        lat = record.get_value("position_lat")
        lon = record.get_value("position_long")
        if lat is None or lon is None:
            continue

        elevation = record.get_value("altitude")
        if elevation is None:
            elevation = record.get_value("enhanced_altitude")
        time = record.get_value("timestamp")

        add_trackpoint(
            trkseg,
            lat * SEMICIRCLE_TO_DEGREES,
            lon * SEMICIRCLE_TO_DEGREES,
            elevation,
            time.isoformat() + "Z" if time else None,
        )
        if time and first_timestamp is None:
            first_timestamp = time
        valid_points += 1

    if valid_points == 0:
        return skip(input_path, "no GPS points")

    out_path = write_gpx(gpx, input_path, first_timestamp, output_folder)
    print(f"ok   {os.path.basename(input_path)} -> {os.path.basename(out_path)} ({activity_type})")
    return out_path


# ----------------- TCX -----------------
def tcx_namespace(root):
    """TCX puts everything in a default namespace; ElementTree needs it named."""
    if root.tag.startswith("{"):
        return {"tcx": root.tag[1:].split("}", 1)[0]}
    return {}


def detect_tcx_activity_type(root, ns):
    activity = root.find(".//tcx:Activity", ns) if ns else root.find(".//Activity")
    if activity is not None and "Sport" in activity.attrib:
        return activity.attrib["Sport"].capitalize()
    return "Unknown"


def tcx_to_gpx(input_path, output_folder):
    root = ET.parse(input_path).getroot()
    ns = tcx_namespace(root)

    def find_all(tag):
        return root.findall(f".//tcx:{tag}", ns) if ns else root.findall(f".//{tag}")

    def child(element, tag):
        return element.find(f"tcx:{tag}", ns) if ns else element.find(tag)

    activity_type = detect_tcx_activity_type(root, ns)
    if activity_type.lower() in SKIP_TYPES:
        return skip(input_path, f"activity type: {activity_type}")

    gpx, trkseg = build_gpx(os.path.basename(input_path), activity_type)

    first_timestamp = None
    valid_points = 0

    for trackpoint in find_all("Trackpoint"):
        position = child(trackpoint, "Position")
        if position is None:
            continue
        lat = child(position, "LatitudeDegrees")
        lon = child(position, "LongitudeDegrees")
        if lat is None or lon is None or not lat.text or not lon.text:
            continue

        elevation = child(trackpoint, "AltitudeMeters")
        time_element = child(trackpoint, "Time")
        time_text = time_element.text if time_element is not None else None

        add_trackpoint(
            trkseg,
            float(lat.text),
            float(lon.text),
            elevation.text if elevation is not None and elevation.text else None,
            time_text,
        )
        if first_timestamp is None:
            first_timestamp = parse_timestamp(time_text)
        valid_points += 1

    if valid_points == 0:
        return skip(input_path, "no GPS points")

    out_path = write_gpx(gpx, input_path, first_timestamp, output_folder)
    print(f"ok   {os.path.basename(input_path)} -> {os.path.basename(out_path)} ({activity_type})")
    return out_path


# ----------------- GPX (copy and rename) -----------------
def copy_gpx_file(input_path, output_folder):
    """Copies a GPX file through, renaming it by its first timestamp.

    The contents are not touched, so a file from another tool keeps whatever it
    had -- including no <type>, which Footprints reads as "Other".
    """
    root = ET.parse(input_path).getroot()

    first_timestamp = None
    for element in root.iter():
        if element.tag.split("}")[-1] == "time":
            first_timestamp = parse_timestamp(element.text)
            if first_timestamp:
                break

    if first_timestamp is None:
        first_timestamp = datetime.datetime.fromtimestamp(os.path.getmtime(input_path))

    out_name = output_name(input_path, first_timestamp)
    out_path = os.path.join(output_folder, out_name)
    os.makedirs(output_folder, exist_ok=True)

    with open(input_path, "rb") as src, open(out_path, "wb") as dst:
        dst.write(src.read())

    print(f"copy {os.path.basename(input_path)} -> {out_name}")
    return out_path


# ----------------- Driver -----------------
CONVERTERS = {".fit": fit_to_gpx, ".tcx": tcx_to_gpx, ".gpx": copy_gpx_file}


def convert_single_file(input_path, output_folder):
    try:
        convert = CONVERTERS.get(os.path.splitext(input_path)[1].lower())
        if convert is None:
            return skip(input_path, "unsupported file type")
        return convert(input_path, output_folder)
    except Exception as error:
        # One malformed export should not abandon the rest of the batch.
        print(f"FAIL {os.path.basename(input_path)}: {error}", file=sys.stderr)
        return None


def convert_folder(input_folder, output_folder, max_workers=None):
    os.makedirs(output_folder, exist_ok=True)

    tasks = [
        os.path.join(root_dir, name)
        for root_dir, _, files in os.walk(input_folder)
        for name in files
        if os.path.splitext(name)[1].lower() in CONVERTERS
    ]
    print(f"Found {len(tasks)} files to convert")

    converted = 0
    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        futures = {executor.submit(convert_single_file, f, output_folder): f for f in tasks}
        for future in as_completed(futures):
            if future.result():
                converted += 1

    print(f"Converted {converted} of {len(tasks)} files into {output_folder}")
    return converted


def main(argv):
    if len(argv) < 3:
        print(f"Usage: python {os.path.basename(argv[0])} <input_folder> <output_folder> [max_cores]")
        return 1

    max_cores = int(argv[3]) if len(argv) >= 4 else None
    convert_folder(argv[1], argv[2], max_workers=max_cores)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
