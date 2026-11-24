"""Download new Garmin Connect activities as the .gpx files Footprints reads.

Spawned by the import panel rather than run by hand, so everything it says goes
to stdout one line at a time and in a shape src/import_job.c can parse:

    total <n>          how many activities are about to be downloaded
    progress <i>       i of them dealt with
    imported <n>       how many landed on disk
    error <message>    what went wrong

It talks to Garmin's Personal API, authenticated by a personal API key created
at connect.garmin.com/app/settings/personalApiToken. The key arrives as the
second of two lines on stdin -- the first is empty, Garmin having no user name
to go with it. Not on the command line, which ps shows to every user on the
machine, and not in the environment, which the whole process tree inherits.

The key is kept in the session directory, readable only by its owner: what it
is traded for is a bearer token good for a day, so every import has to trade it
again. A key Garmin refuses -- expired or revoked -- is deleted, which is what
puts the panel back to asking for one.

The Personal API has no GPX export. What it gives is the file the activity was
recorded as, zipped, so a .fit is converted here with the same code
convert_fit_to_gpx.py uses for a data export.
"""

import io
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from datetime import datetime

import convert_fit_to_gpx

GPX_NAMESPACE = "http://www.topografix.com/GPX/1/1"

# The <summary> element written into an export that has no path to measure, and
# read back by src/gpx_parser.c. Its own namespace because it is Footprints'
# invention rather than anything a GPX reader is expected to know.
SUMMARY_NAMESPACE = "https://github.com/JulianFreu/footprints/summary/1"

API_ROOT = "https://personal.connectapi.garmin.com"
TOKEN_PATH = "/personalapitoken-service/personal-api/personal-api-key/token"
LIST_PATH = "/activitylist-service/activities/search/activities"
DOWNLOAD_PATH = "/download-service/files/activity/{}"

# Where the key is kept, inside the session directory. Its existence is what
# tells the panel an import can run; see GARMIN_TOKEN_FILE in src/config.h.
KEY_FILE = "api_key"

# Long enough for a large original file on a slow line, short enough that a
# request nobody is answering does not hold the import forever.
TIMEOUT_SECONDS = 60

# Activities asked for per request. The list is only read to find out which ids
# are missing, so a larger page is fewer round trips for the same answer.
PAGE_SIZE = 50

# What the exit code tells src/import_job.c, which turns it into the stage the
# panel shows. Anything unexpected is EXIT_ERROR.
EXIT_OK = 0
EXIT_ERROR = 1
EXIT_NO_SESSION = 3

# The activity id, read back out of a name this script wrote. It is what makes
# "have I already got this one" a directory listing rather than a state file.
ID_PATTERN = re.compile(r"(?:^|_)garmin_(\d+)\.gpx$")


def say(message):
    """One line of the protocol above.

    Flushed every time: stdout is a pipe here, not a terminal, so Python would
    otherwise hold the progress back until the process ended -- which is exactly
    the wait the panel exists to fill.
    """
    print(message, flush=True)


class KeyRefused(Exception):
    """Garmin will not trade the key for a token, which no retry can change."""


# ----------------- HTTP -----------------
def request(path, *, headers, params=None, post=False):
    """One call to the Personal API, as the bytes of its response.

    urllib rather than requests: fitparse is the only package the Garmin import
    needs, and there is no reason for this to be a second.
    """
    url = API_ROOT + path
    if params:
        url += "?" + urllib.parse.urlencode(params)

    # An empty body rather than none on a POST, so it goes with a length of zero
    # -- which is what a server that insists on one is given by every other
    # client.
    call = urllib.request.Request(
        url,
        data=b"" if post else None,
        headers={"User-Agent": "footprints", **headers},
    )

    try:
        with urllib.request.urlopen(call, timeout=TIMEOUT_SECONDS) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        # Garmin says what it did not like in the body, which is a good deal
        # more use than "HTTP Error 400: Bad Request" on its own.
        detail = error.read().decode(errors="replace").strip()
        raise RuntimeError(
            f"{error.code} {error.reason}: {detail}" if detail else f"{error.code} {error.reason}"
        ) from error


def bearer_token(key):
    """The key traded for a token, good for the rest of this run."""
    try:
        return request(
            TOKEN_PATH, headers={"Accept": "*/*", "Authorization": f"ApiKey {key}"}, post=True
        ).decode().strip()
    except RuntimeError as error:
        cause = error.__cause__
        if isinstance(cause, urllib.error.HTTPError) and cause.code in (401, 403):
            raise KeyRefused("Garmin refused the API key; create a new one and connect again")
        raise


def api_get(token, path, params=None, accept="application/json"):
    return request(
        path, headers={"Accept": accept, "Authorization": f"Bearer {token}"}, params=params
    )


# ----------------- The key -----------------
def key_path(session_dir):
    return os.path.join(session_dir, KEY_FILE)


def save_key(session_dir, key):
    os.makedirs(session_dir, exist_ok=True)

    # Written through a descriptor opened 0600 rather than chmod'ed afterwards,
    # so it is never readable by anyone else, not even briefly.
    handle = os.open(key_path(session_dir), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(handle, "w") as out:
        out.write(key)


def load_key(session_dir):
    with open(key_path(session_dir)) as handle:
        return handle.read().strip()


def forget_key(session_dir):
    try:
        os.remove(key_path(session_dir))
    except OSError:
        pass  # already gone


# ----------------- Naming and activity types -----------------
def activity_type(type_key):
    """The word Footprints reads, or None to leave the file's own alone.

    gpx_parser.c matches "running", "hiking" and "cycling" and nothing else, so
    Garmin's narrower keys have to be widened here -- a trail_running export
    left as it came would read as "Other" and drop out of every record and
    filter that asks for a run.
    """
    key = (type_key or "").lower()
    if "running" in key:
        return "running"
    if "hiking" in key:
        return "hiking"
    if "cycling" in key or "biking" in key:
        return "cycling"
    return None


def parse_timestamp(text):
    """An ISO8601 timestamp, or None if it is not one."""
    if not text:
        return None
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None


def output_name(activity_id, start_time):
    """Prefixed with the start time so the directory sorts chronologically, and
    carrying the activity id so the next import can tell it is already here."""
    if start_time is None:
        return f"garmin_{activity_id}.gpx"
    return f"{start_time.strftime('%Y-%m-%d-%H-%M-%S')}_garmin_{activity_id}.gpx"


def existing_ids(output_folder):
    """The activity ids already downloaded into `output_folder`."""
    try:
        names = os.listdir(output_folder)
    except OSError:
        return set()  # nothing there yet; every activity is new

    found = set()
    for name in names:
        match = ID_PATTERN.search(name)
        if match:
            found.add(int(match.group(1)))
    return found


def local_name(element):
    """The tag without the namespace ElementTree brackets onto the front."""
    return element.tag.split("}")[-1]


def child_named(parent, name):
    """The first child of `parent` with that local name, or None."""
    return next((c for c in parent if local_name(c) == name), None)


def insert_before_segments(parent, element):
    """`element` added to `parent`, but ahead of any track segments.

    The parser finds it either way; only one of the two orders is a valid GPX
    document.
    """
    index = next(
        (i for i, c in enumerate(parent) if local_name(c) == "trkseg"), len(parent)
    )
    parent.insert(index, element)
    return element


def has_path(root):
    """Whether any trackpoint in the document carries both coordinates."""
    return any(
        local_name(e) == "trkpt" and e.get("lat") and e.get("lon") for e in root.iter()
    )


def summary_of(activity):
    """The totals for an activity Footprints cannot measure for itself, or None.

    Distance is something the parser derives from the path, and a GPX has
    nowhere to state it outright -- so a treadmill run, whose export has no
    path, would otherwise land as an undated row of zeroes that the statistics
    and records panels both pass over. Garmin knows every one of the numbers and
    the listing carrying them has already been fetched, so they are written into
    the file rather than dropped on the floor.

    Metres and seconds, which is what every other number in a GPX is spelled in.
    """
    start = parse_timestamp(activity.get("startTimeGMT"))
    if start is None:
        return None  # nothing to date it by, and a summary needs a date

    def number(key):
        value = activity.get(key)
        return f"{float(value):.2f}" if isinstance(value, (int, float)) else "0"

    return {
        "start": start.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "distance": number("distance"),
        "duration": number("duration"),
        "ascent": number("elevationGain"),
        "descent": number("elevationLoss"),
    }


def annotated(gpx_bytes, type_name, summary):
    """The GPX with <trk><type> set, and `summary` added where it has no path.

    <type> is not part of the GPX standard -- it is how Footprints tells a run
    from a ride -- so Garmin's own value is replaced rather than merely filled
    in. The summary is written only into an export with no trackpoints to
    measure, so an outdoor activity comes out exactly as Garmin sent it and
    nothing in the file can contradict what its path says. A document that will
    not parse is passed through untouched: this is not the place to repair one.
    """
    if type_name is None and summary is None:
        return gpx_bytes

    try:
        root = ET.fromstring(gpx_bytes)
    except ET.ParseError:
        return gpx_bytes

    namespace = root.tag.split("}")[0][1:] if root.tag.startswith("{") else ""

    def tag(name):
        return f"{{{namespace}}}{name}" if namespace else name

    if summary is not None and has_path(root):
        summary = None

    tracks = [e for e in root.iter() if local_name(e) == "trk"]
    if not tracks and summary is not None:
        # An export with nothing to plot sometimes carries no <trk> at all, and
        # a summary with nowhere to hang is an activity that stays invisible.
        tracks = [ET.SubElement(root, tag("trk"))]

    changed = False
    for track in tracks:
        if type_name is not None:
            element = child_named(track, "type")
            if element is None:
                element = insert_before_segments(track, ET.Element(tag("type")))
            if (element.text or "").strip().lower() != type_name:
                element.text = type_name
                changed = True

        if summary is not None:
            extensions = child_named(track, "extensions")
            if extensions is None:
                extensions = insert_before_segments(
                    track, ET.Element(tag("extensions"))
                )
            ET.SubElement(extensions, f"{{{SUMMARY_NAMESPACE}}}summary", summary)
            changed = True

    if not changed:
        return gpx_bytes

    # Without this the default namespace comes back as ns0:, which parses the
    # same but makes the file look nothing like the one Garmin sent.
    ET.register_namespace("", namespace or GPX_NAMESPACE)
    ET.register_namespace("footprints", SUMMARY_NAMESPACE)
    ET.register_namespace("gpxtpx", convert_fit_to_gpx.TRACKPOINT_EXTENSION_NS)
    return ET.tostring(root, encoding="UTF-8", xml_declaration=True)


def original_gpx(archive, activity_id):
    """The GPX for an activity, from the zipped file it was recorded as.

    A .fit is converted, and a .gpx -- what an activity uploaded from elsewhere
    may have been -- is taken as it is. Anything else, or an archive with
    nothing in it, comes out as a document with no path, which annotated() then
    gives Garmin's own totals rather than leaving the activity out.
    """
    name = f"garmin_{activity_id}"

    with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
        members = bundle.namelist()
        fit = next((m for m in members if m.lower().endswith(".fit")), None)
        gpx = next((m for m in members if m.lower().endswith(".gpx")), None)

        if fit is not None:
            fitfile = convert_fit_to_gpx.open_fit(io.BytesIO(bundle.read(fit)))
            document, _, _ = convert_fit_to_gpx.fit_document(
                fitfile, name, convert_fit_to_gpx.detect_fit_activity_type(fitfile)
            )
        elif gpx is not None:
            return bundle.read(gpx)
        else:
            document, _ = convert_fit_to_gpx.build_gpx(name, "Unknown")

    ET.indent(document)
    return ET.tostring(document, encoding="UTF-8", xml_declaration=True)


# ----------------- Listing -----------------
def new_activities(fetch_page, known, page_size=PAGE_SIZE):
    """The activities not yet on disk, newest first.

    `fetch_page(start, limit)` is passed in rather than called directly so the
    stopping rule can be tested without an account.

    The list comes back newest first, so a page with nothing new on it means
    everything older has been downloaded too and there is no reason to keep
    asking. A page that is only partly known does not stop the walk -- that is
    what a deleted file looks like, and it is worth fetching again.
    """
    found = []
    start = 0

    while True:
        page = fetch_page(start, page_size)
        if not page:
            break

        fresh = [a for a in page if int(a["activityId"]) not in known]
        if not fresh:
            break

        found.extend(fresh)
        start += page_size

    return found


# ----------------- Commands -----------------
def read_key():
    """The API key: the second of two lines on stdin, the first being empty."""
    lines = sys.stdin.read().split("\n")
    return lines[1].strip() if len(lines) > 1 else ""


def login(session_dir):
    key = read_key()
    if not key:
        say("error an API key is needed")
        return EXIT_ERROR

    # Traded once here only to find out whether Garmin takes it, so a key
    # pasted wrong is reported now rather than on the first import.
    try:
        bearer_token(key)
    except Exception as error:
        say(f"error {error}")
        return EXIT_ERROR

    save_key(session_dir, key)
    return EXIT_OK


def sync(session_dir, output_folder):
    try:
        import fitparse  # noqa: F401 -- asked once, rather than per activity
    except ImportError:
        say("error the Garmin import needs fitparse: pip install -r requirements.txt")
        return EXIT_ERROR

    try:
        token = bearer_token(load_key(session_dir))
    except OSError:
        say("error not connected: no API key saved")
        return EXIT_NO_SESSION
    except KeyRefused as error:
        forget_key(session_dir)
        say(f"error {error}")
        return EXIT_NO_SESSION
    except Exception as error:
        say(f"error could not sign in: {error}")
        return EXIT_ERROR

    def fetch_page(start, limit):
        body = api_get(token, LIST_PATH, params={"start": start, "limit": limit})
        return json.loads(body) if body else []

    try:
        pending = new_activities(fetch_page, existing_ids(output_folder))
    except Exception as error:
        say(f"error could not list activities: {error}")
        return EXIT_ERROR

    say(f"total {len(pending)}")

    os.makedirs(output_folder, exist_ok=True)
    imported = 0

    # Oldest first, so an import that is interrupted leaves a contiguous stretch
    # of history behind rather than a hole in the middle of it.
    for done, activity in enumerate(reversed(pending), 1):
        activity_id = int(activity["activityId"])
        try:
            archive = api_get(
                token,
                DOWNLOAD_PATH.format(activity_id),
                accept="application/zip, application/x-zip-compressed, application/octet-stream",
            )
            data = annotated(
                original_gpx(archive, activity_id),
                activity_type((activity.get("activityType") or {}).get("typeKey")),
                summary_of(activity),
            )
            name = output_name(activity_id, parse_timestamp(activity.get("startTimeGMT")))
            with open(os.path.join(output_folder, name), "wb") as out:
                out.write(data)
            imported += 1
        except Exception as error:
            # One activity Garmin will not part with should not abandon the
            # rest of the import, the same way one malformed export does not
            # abandon a conversion.
            say(f"error activity {activity_id}: {error}")

        say(f"progress {done}")

    say(f"imported {imported}")
    return EXIT_OK


def main(argv):
    name = os.path.basename(argv[0])
    if len(argv) < 3:
        print(
            f"Usage: python {name} login <session_dir>\n"
            f"       python {name} sync <session_dir> <output_folder>"
        )
        return EXIT_ERROR

    try:
        if argv[1] == "login":
            return login(argv[2])
        if argv[1] == "sync":
            if len(argv) < 4:
                say("error sync needs a session directory and an output folder")
                return EXIT_ERROR
            return sync(argv[2], argv[3])
    except Exception as error:
        say(f"error {error}")
        return EXIT_ERROR

    say(f"error unknown command: {argv[1]}")
    return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main(sys.argv))
