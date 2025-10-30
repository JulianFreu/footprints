import os
import sys
import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed
from fitparse import FitFile
from lxml import etree

SKIP_TYPES = {"unknown", "training", "swimming"}


# ----------------- FIT Handling -----------------
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
    fitfile = FitFile(input_path)
    activity_type = detect_fit_activity_type(fitfile)

    if activity_type.lower() in SKIP_TYPES:
        print(f"⏭️  Skipping {os.path.basename(input_path)} (activity type: {activity_type})")
        return None

    gpx = etree.Element("gpx", version="1.1", creator="fit_tcx_to_gpx")
    metadata = etree.SubElement(gpx, "metadata")
    etree.SubElement(metadata, "time").text = datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"

    trk = etree.SubElement(gpx, "trk")
    etree.SubElement(trk, "name").text = os.path.basename(input_path)
    etree.SubElement(trk, "type").text = activity_type
    trkseg = etree.SubElement(trk, "trkseg")

    first_timestamp = None
    valid_points = 0

    for record in fitfile.get_messages("record"):
        lat = record.get_value("position_lat")
        lon = record.get_value("position_long")
        ele = record.get_value("altitude") or record.get_value("enhanced_altitude")
        time = record.get_value("timestamp")

        if lat is None or lon is None:
            continue

        lat_deg = lat * (180.0 / 2**31)
        lon_deg = lon * (180.0 / 2**31)

        trkpt = etree.SubElement(trkseg, "trkpt", lat=f"{lat_deg:.6f}", lon=f"{lon_deg:.6f}")
        if ele is not None:
            etree.SubElement(trkpt, "ele").text = f"{float(ele):.1f}"
        if time:
            etree.SubElement(trkpt, "time").text = time.isoformat() + "Z"
            if first_timestamp is None:
                first_timestamp = time

        valid_points += 1

    if valid_points == 0:
        print(f"⏭️  Skipping {os.path.basename(input_path)} (no GPS points)")
        return None

    # Determine output filename based on timestamp
    base_name = os.path.splitext(os.path.basename(input_path))[0]
    if first_timestamp:
        ts_str = first_timestamp.strftime("%Y-%m-%d-%H-%M-%S")
        out_name = f"{ts_str}_{base_name}.gpx"
    else:
        out_name = f"{base_name}.gpx"

    out_path = os.path.join(output_folder, out_name)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    with open(out_path, "wb") as f:
        f.write(etree.tostring(gpx, pretty_print=True, xml_declaration=True, encoding="UTF-8"))

    print(f"✅ {os.path.basename(input_path)} → {out_name} ({activity_type})")
    return out_path


# ----------------- TCX Handling -----------------
def get_default_namespace(root):
    ns = root.nsmap.get(None)
    return {"tcx": ns} if ns else {}


def detect_tcx_activity_type(tcx_root):
    NS = get_default_namespace(tcx_root)
    activity = tcx_root.find(".//tcx:Activity", namespaces=NS)
    if activity is not None and "Sport" in activity.attrib:
        return activity.attrib["Sport"].capitalize()
    return "Unknown"


def tcx_to_gpx(input_path, output_folder):
    tcx_tree = etree.parse(input_path)
    tcx_root = tcx_tree.getroot()
    NS = get_default_namespace(tcx_root)

    activity_type = detect_tcx_activity_type(tcx_root)
    if activity_type.lower() in SKIP_TYPES:
        print(f"⏭️  Skipping {os.path.basename(input_path)} (activity type: {activity_type})")
        return None

    gpx = etree.Element("gpx", version="1.1", creator="fit_tcx_to_gpx")
    metadata = etree.SubElement(gpx, "metadata")
    etree.SubElement(metadata, "time").text = datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"

    trk = etree.SubElement(gpx, "trk")
    etree.SubElement(trk, "name").text = os.path.basename(input_path)
    etree.SubElement(trk, "type").text = activity_type
    trkseg = etree.SubElement(trk, "trkseg")

    valid_points = 0
    first_timestamp = None

    for tp in tcx_root.findall(".//tcx:Trackpoint", namespaces=NS):
        pos = tp.find("tcx:Position", namespaces=NS)
        if pos is None:
            continue
        lat = pos.findtext("tcx:LatitudeDegrees", namespaces=NS)
        lon = pos.findtext("tcx:LongitudeDegrees", namespaces=NS)
        if lat is None or lon is None:
            continue

        trkpt = etree.SubElement(trkseg, "trkpt", lat=f"{float(lat):.6f}", lon=f"{float(lon):.6f}")
        ele = tp.findtext("tcx:AltitudeMeters", namespaces=NS)
        if ele:
            etree.SubElement(trkpt, "ele").text = f"{float(ele):.1f}"
        time_text = tp.findtext("tcx:Time", namespaces=NS)
        if time_text:
            etree.SubElement(trkpt, "time").text = time_text
            if first_timestamp is None:
                try:
                    first_timestamp = datetime.datetime.fromisoformat(time_text.replace("Z", "+00:00"))
                except Exception:
                    pass

        valid_points += 1

    if valid_points == 0:
        print(f"⏭️  Skipping {os.path.basename(input_path)} (no GPS points)")
        return None

    base_name = os.path.splitext(os.path.basename(input_path))[0]
    if first_timestamp:
        ts_str = first_timestamp.strftime("%Y-%m-%d-%H-%M-%S")
        out_name = f"{ts_str}_{base_name}.gpx"
    else:
        out_name = f"{base_name}.gpx"

    out_path = os.path.join(output_folder, out_name)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    with open(out_path, "wb") as f:
        f.write(etree.tostring(gpx, pretty_print=True, xml_declaration=True, encoding="UTF-8"))

    print(f"✅ {os.path.basename(input_path)} → {out_name} ({activity_type})")
    return out_path


# ----------------- Parallel Folder Conversion -----------------
def convert_single_file(fpath, output_folder):
    try:
        _, ext = os.path.splitext(fpath)
        ext = ext.lower()
        if ext == ".fit":
            return fit_to_gpx(fpath, output_folder)
        elif ext == ".tcx":
            return tcx_to_gpx(fpath, output_folder)
        else:
            print(f"⚠️  Unsupported file type: {os.path.basename(fpath)}")
    except Exception as e:
        print(f"❌ Error converting {os.path.basename(fpath)}: {e}")
    return None


def convert_folder(input_folder, output_folder, max_workers=None):
    os.makedirs(output_folder, exist_ok=True)

    # Collect all relevant files
    tasks = []
    for root_dir, _, files in os.walk(input_folder):
        for fname in files:
            if os.path.splitext(fname)[1].lower() in (".fit", ".tcx"):
                tasks.append(os.path.join(root_dir, fname))

    print(f"🧮 Found {len(tasks)} files to convert")

    # Parallel conversion
    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        futures = {executor.submit(convert_single_file, f, output_folder): f for f in tasks}
        for future in as_completed(futures):
            future.result()

    print("✅ All conversions complete.")


# ----------------- Command-line Interface -----------------
if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python convert_fit_to_gpx.py <input_folder> <output_folder> [max_cores]")
        sys.exit(1)

    input_folder = sys.argv[1]
    output_folder = sys.argv[2]
    max_cores = int(sys.argv[3]) if len(sys.argv) >= 4 else None

    convert_folder(input_folder, output_folder, max_workers=max_cores)
