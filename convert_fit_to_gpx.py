import os
import sys
import datetime
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

def fit_to_gpx(input_path, output_path):
    fitfile = FitFile(input_path)
    activity_type = detect_fit_activity_type(fitfile)

    # Skip unwanted activity types
    if activity_type.lower() in SKIP_TYPES:
        print(f"⏭️  Skipping {os.path.basename(input_path)} (activity type: {activity_type})")
        return False

    gpx = etree.Element("gpx", version="1.1", creator="fit_tcx_to_gpx")
    metadata = etree.SubElement(gpx, "metadata")
    etree.SubElement(metadata, "time").text = datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"

    trk = etree.SubElement(gpx, "trk")
    etree.SubElement(trk, "name").text = os.path.basename(input_path)
    etree.SubElement(trk, "type").text = activity_type
    trkseg = etree.SubElement(trk, "trkseg")

    valid_points = 0
    for record in fitfile.get_messages("record"):
        lat = record.get_value("position_lat")
        lon = record.get_value("position_long")
        ele = record.get_value("altitude")
        if ele is None:
            ele = record.get_value("enhanced_altitude")  # fallback

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

        valid_points += 1

    if valid_points == 0:
        print(f"⏭️  Skipping {os.path.basename(input_path)} (no GPS points)")
        return False

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(etree.tostring(gpx, pretty_print=True, xml_declaration=True, encoding="UTF-8"))

    print(f"✅ {os.path.basename(input_path)} → {os.path.basename(output_path)} ({activity_type})")
    return True

# ----------------- TCX Handling -----------------
def get_default_namespace(root):
    ns = root.nsmap.get(None)
    if ns:
        return {"tcx": ns}
    return {}

def detect_tcx_activity_type(tcx_root):
    NS = get_default_namespace(tcx_root)
    activity = tcx_root.find(".//tcx:Activity", namespaces=NS)
    if activity is not None and 'Sport' in activity.attrib:
        return activity.attrib['Sport'].capitalize()
    return "Unknown"

def tcx_to_gpx(input_path, output_path):
    tcx_tree = etree.parse(input_path)
    tcx_root = tcx_tree.getroot()
    NS = get_default_namespace(tcx_root)

    activity_type = detect_tcx_activity_type(tcx_root)
    if activity_type.lower() in SKIP_TYPES:
        print(f"⏭️  Skipping {os.path.basename(input_path)} (activity type: {activity_type})")
        return False

    gpx = etree.Element("gpx", version="1.1", creator="fit_tcx_to_gpx")
    metadata = etree.SubElement(gpx, "metadata")
    etree.SubElement(metadata, "time").text = datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"

    trk = etree.SubElement(gpx, "trk")
    etree.SubElement(trk, "name").text = os.path.basename(input_path)
    etree.SubElement(trk, "type").text = activity_type
    trkseg = etree.SubElement(trk, "trkseg")

    valid_points = 0
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

        valid_points += 1

    if valid_points == 0:
        print(f"⏭️  Skipping {os.path.basename(input_path)} (no GPS points)")
        return False

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(etree.tostring(gpx, pretty_print=True, xml_declaration=True, encoding="UTF-8"))

    print(f"✅ {os.path.basename(input_path)} → {os.path.basename(output_path)} ({activity_type})")
    return True

# ----------------- Recursive Folder Conversion -----------------
def convert_folder(input_folder, output_folder):
    os.makedirs(output_folder, exist_ok=True)  # ensure output folder exists

    for root_dir, _, files in os.walk(input_folder):
        for fname in files:
            fpath = os.path.join(root_dir, fname)
            name, ext = os.path.splitext(fname)
            ext = ext.lower()

            out_path = os.path.join(output_folder, f"{name}.gpx")  # all files in same folder

            try:
                if ext == ".fit":
                    fit_to_gpx(fpath, out_path)
                elif ext == ".tcx":
                    tcx_to_gpx(fpath, out_path)
                else:
                    print(f"⚠️  Unsupported file type: {fname}")
            except Exception as e:
                print(f"❌ Error converting {fname}: {e}")

# ----------------- Command-line Interface -----------------
if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python convert_fit_to_gpx.py <input_folder> <output_folder>")
        sys.exit(1)

    input_folder = sys.argv[1]
    output_folder = sys.argv[2]
    convert_folder(input_folder, output_folder)
