"""Download new Strava activities as the .gpx files Footprints reads.

Spawned by the import panel rather than run by hand, so everything it says goes
to stdout one line at a time and in a shape src/import_job.c can parse:

    total <n>          how many activities are about to be downloaded
    progress <i>       i of them dealt with
    imported <n>       how many landed on disk
    message <text>     something to show that is not a failure
    error <message>    what went wrong

Credentials arrive as two lines on stdin -- the client id and the client secret
of a Strava API application. Not on the command line, which ps shows to every
user on the machine, and not in the environment, which the whole process tree
inherits.

Strava has no password to give an application: authorisation happens in a
browser, and this script catches the redirect on a loopback port of its own.
Only what comes back from that is kept, in the session directory, so the browser
step happens once rather than on every import.

Strava has no GPX export either. What the API gives is the recorded streams, so
the file is built here from the latitude, longitude, altitude and time of every
point -- which is exactly what gpx_parser.c reads back.
"""

import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
import xml.etree.ElementTree as ET
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer

GPX_NAMESPACE = "http://www.topografix.com/GPX/1/1"

AUTHORIZE_URL = "https://www.strava.com/oauth/authorize"
TOKEN_URL = "https://www.strava.com/oauth/token"
API_ROOT = "https://www.strava.com/api/v3"

# Everything Footprints draws needs the whole history, including the activities
# marked "only you", so this is the read scope rather than the narrower one.
SCOPE = "activity:read_all"

# Activities asked for per request. The list is only read to find out which ids
# are missing, so a larger page is fewer round trips for the same answer.
PAGE_SIZE = 100

# How long the browser has to come back before the login gives up. Long enough
# to find the window, log in and press Authorize; short enough that a login
# nobody finished does not sit in the panel forever.
AUTH_TIMEOUT_SECONDS = 300

# A token is refreshed slightly before Strava would refuse it, so an import that
# takes a while cannot have it expire underneath it.
TOKEN_MARGIN_SECONDS = 300

# What the exit code tells src/import_job.c, which turns it into the stage the
# panel shows. Anything unexpected is EXIT_ERROR.
EXIT_OK = 0
EXIT_ERROR = 1
EXIT_NO_SESSION = 3

# The activity id, read back out of a name this script wrote. It is what makes
# "have I already got this one" a directory listing rather than a state file.
ID_PATTERN = re.compile(r"(?:^|_)strava_(\d+)\.gpx$")


def say(message):
    """One line of the protocol above.

    Flushed every time: stdout is a pipe here, not a terminal, so Python would
    otherwise hold the progress back until the process ended -- which is exactly
    the wait the panel exists to fill.
    """
    print(message, flush=True)


class RateLimited(Exception):
    """Strava's fifteen-minute quota, which no retry inside this run can clear."""


# ----------------- Naming and activity types -----------------
def activity_type(sport_type):
    """The word Footprints reads, or None to leave the file without one.

    gpx_parser.c matches "running", "hiking" and "cycling" and nothing else, so
    Strava's narrower types have to be widened here -- a TrailRun left as it came
    would read as "Other" and drop out of every record and filter that asks for
    a run.
    """
    key = (sport_type or "").lower()
    if "run" in key:
        return "running"
    if "hike" in key:
        return "hiking"
    if "ride" in key or "bike" in key or "cycl" in key:
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
        return f"strava_{activity_id}.gpx"
    return f"{start_time.strftime('%Y-%m-%d-%H-%M-%S')}_strava_{activity_id}.gpx"


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


# ----------------- HTTP -----------------
def request_json(url, data=None, token=None):
    """One API call, as parsed JSON.

    urllib rather than requests: the Garmin import is the only thing here that
    needs a package installed, and there is no reason for this one to be a
    second.
    """
    body = urllib.parse.urlencode(data).encode() if data else None
    request = urllib.request.Request(url, data=body)
    if token:
        request.add_header("Authorization", f"Bearer {token}")

    try:
        with urllib.request.urlopen(request) as response:
            return json.loads(response.read().decode())
    except urllib.error.HTTPError as error:
        if error.code == 429:
            raise RateLimited(
                "rate limited by Strava; try again in about fifteen minutes"
            )
        # Strava says what it did not like in the body, which is a good deal
        # more use than "HTTP Error 400: Bad Request" on its own.
        detail = error.read().decode(errors="replace").strip()
        raise RuntimeError(f"{error.code} {error.reason}: {detail}" if detail
                           else f"{error.code} {error.reason}")


# ----------------- The session -----------------
def save_session(session_dir, session):
    """The tokens, and the application they belong to.

    The client secret is in here rather than in settings.conf because it is
    needed to refresh a token and is a secret -- the same reason garth keeps its
    own token beside it rather than in the settings file.
    """
    os.makedirs(session_dir, exist_ok=True)
    path = os.path.join(session_dir, "token.json")

    # Written through a descriptor opened 0600 rather than chmod'ed afterwards,
    # so it is never readable by anyone else, not even briefly.
    handle = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(handle, "w") as out:
        json.dump(session, out)


def load_session(session_dir):
    with open(os.path.join(session_dir, "token.json")) as handle:
        return json.load(handle)


def fresh_token(session_dir, session):
    """The access token, refreshed first if it is close to running out."""
    if session.get("expires_at", 0) - time.time() > TOKEN_MARGIN_SECONDS:
        return session["access_token"]

    refreshed = request_json(TOKEN_URL, data={
        "client_id": session["client_id"],
        "client_secret": session["client_secret"],
        "grant_type": "refresh_token",
        "refresh_token": session["refresh_token"],
    })

    session.update({
        "access_token": refreshed["access_token"],
        "refresh_token": refreshed["refresh_token"],
        "expires_at": refreshed["expires_at"],
    })
    save_session(session_dir, session)
    return session["access_token"]


# ----------------- The browser step -----------------
class CallbackHandler(BaseHTTPRequestHandler):
    """Serves two things and nothing else: the redirect to Strava, and the page
    the redirect back lands on."""

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)

        # The panel shows a short local address rather than the authorisation
        # URL, which is far too long for the one line it has. Sending the
        # browser on from here is what makes the short one enough.
        if parsed.path == "/":
            self.send_response(302)
            self.send_header("Location", self.server.authorize_url)
            self.end_headers()
            return

        self.server.result = {
            key: value[0]
            for key, value in urllib.parse.parse_qs(parsed.query).items()
        }

        page = b"<html><body><p>Footprints has what it needs. " \
               b"You can close this tab.</p></body></html>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(page)))
        self.end_headers()
        self.wfile.write(page)

    def log_message(self, fmt, *args):
        pass  # stdout is the protocol, and stderr is nobody's here


def authorize(client_id):
    """Runs the browser step and returns what Strava redirected back with.

    The port is whatever the machine had free: Strava validates the callback
    domain rather than the port, so an application registered against
    "localhost" works without anything having to be reserved.
    """
    server = HTTPServer(("127.0.0.1", 0), CallbackHandler)
    server.timeout = 1.0
    server.result = None

    port = server.server_port
    server.authorize_url = AUTHORIZE_URL + "?" + urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": f"http://localhost:{port}/callback",
        "approval_prompt": "auto",
        "scope": SCOPE,
    })

    local_url = f"http://localhost:{port}/"
    say(f"message Waiting for the browser — open {local_url} if it did not")
    webbrowser.open(local_url)

    deadline = time.monotonic() + AUTH_TIMEOUT_SECONDS
    while server.result is None and time.monotonic() < deadline:
        server.handle_request()  # returns after server.timeout if nothing came

    server.server_close()
    return server.result


# ----------------- Listing -----------------
def new_activities(fetch_page, known, page_size=PAGE_SIZE):
    """The activities not yet on disk that have a track to download, newest
    first.

    `fetch_page(page, per_page)` is passed in rather than called directly so the
    stopping rule can be tested without an account.

    The list comes back newest first, so a page with nothing new on it means
    everything older has been downloaded too and there is no reason to keep
    asking. A page that is only partly known does not stop the walk -- that is
    what a deleted file looks like, and it is worth fetching again.

    An activity with no GPS -- a treadmill run, a manually entered ride -- still
    counts as seen for that rule but is not returned. Leaving it out of the
    count as well would let a page of them stop the walk short of the activities
    behind it.
    """
    found = []
    page = 1

    while True:
        activities = fetch_page(page, page_size)
        if not activities:
            break

        fresh = [a for a in activities if int(a["id"]) not in known]
        if not fresh:
            break

        found.extend(a for a in fresh if has_track(a))
        page += 1

    return found


def has_track(activity):
    """Whether Strava recorded a path for it, rather than only its numbers."""
    if activity.get("start_latlng"):
        return True
    return bool((activity.get("map") or {}).get("summary_polyline"))


# ----------------- Building the GPX -----------------
def build_gpx(activity, streams):
    """The activity's streams as a GPX document.

    Elevation and time are written where Strava has them and left out where it
    does not, which is what a device without a barometer or a manually trimmed
    activity looks like. The <type> element is not part of the GPX standard --
    it is how Footprints tells a run from a ride.
    """
    points = (streams.get("latlng") or {}).get("data") or []
    if not points:
        raise RuntimeError("no GPS stream")

    altitude = (streams.get("altitude") or {}).get("data") or []
    offsets = (streams.get("time") or {}).get("data") or []
    started = parse_timestamp(activity.get("start_date"))

    ET.register_namespace("", GPX_NAMESPACE)
    root = ET.Element(f"{{{GPX_NAMESPACE}}}gpx",
                      {"version": "1.1", "creator": "footprints strava_sync.py"})
    track = ET.SubElement(root, f"{{{GPX_NAMESPACE}}}trk")

    name = ET.SubElement(track, f"{{{GPX_NAMESPACE}}}name")
    name.text = activity.get("name") or f"Strava activity {activity['id']}"

    type_name = activity_type(activity.get("sport_type") or activity.get("type"))
    if type_name:
        # Before the segment rather than after it: the parser finds it either
        # way, but only one of the two is a valid GPX document.
        ET.SubElement(track, f"{{{GPX_NAMESPACE}}}type").text = type_name

    segment = ET.SubElement(track, f"{{{GPX_NAMESPACE}}}trkseg")

    for i, (latitude, longitude) in enumerate(points):
        point = ET.SubElement(segment, f"{{{GPX_NAMESPACE}}}trkpt",
                              {"lat": f"{latitude:.7f}", "lon": f"{longitude:.7f}"})
        if i < len(altitude):
            ET.SubElement(point, f"{{{GPX_NAMESPACE}}}ele").text = f"{altitude[i]:.1f}"
        if started is not None and i < len(offsets):
            stamp = started.astimezone(timezone.utc) + timedelta(seconds=offsets[i])
            ET.SubElement(point, f"{{{GPX_NAMESPACE}}}time").text = \
                stamp.strftime("%Y-%m-%dT%H:%M:%SZ")

    return ET.tostring(root, encoding="UTF-8", xml_declaration=True)


# ----------------- Commands -----------------
def read_credentials():
    """The application's id and secret, one per line on stdin.

    The third line the panel writes is what Garmin's MFA code goes on; Strava
    has nothing to put there, and it is read and dropped so the two helpers can
    be fed the same way.
    """
    lines = sys.stdin.read().split("\n")
    while len(lines) < 2:
        lines.append("")
    return lines[0].strip(), lines[1].strip()


def login(session_dir):
    client_id, client_secret = read_credentials()
    if not client_id or not client_secret:
        say("error a client id and a client secret are needed")
        return EXIT_ERROR

    try:
        result = authorize(client_id)
    except OSError as error:
        say(f"error could not listen for the browser: {error}")
        return EXIT_ERROR

    if result is None:
        say("error the browser did not come back in time")
        return EXIT_ERROR
    if "code" not in result:
        # What pressing Cancel on Strava's page looks like.
        say(f"error {result.get('error', 'no authorisation code came back')}")
        return EXIT_ERROR
    if "activity:read" not in result.get("scope", ""):
        # Strava's page asks for each permission with a tick box of its own, and
        # one left unticked would fail later with a 401 per activity rather than
        # here, where it can still be explained.
        say("error the activity permission was not granted; connect again and allow it")
        return EXIT_ERROR

    try:
        token = request_json(TOKEN_URL, data={
            "client_id": client_id,
            "client_secret": client_secret,
            "grant_type": "authorization_code",
            "code": result["code"],
        })
    except Exception as error:
        say(f"error {error}")
        return EXIT_ERROR

    save_session(session_dir, {
        "client_id": client_id,
        "client_secret": client_secret,
        "access_token": token["access_token"],
        "refresh_token": token["refresh_token"],
        "expires_at": token["expires_at"],
    })
    return EXIT_OK


def sync(session_dir, output_folder):
    try:
        session = load_session(session_dir)
        token = fresh_token(session_dir, session)
    except Exception as error:
        say(f"error not logged in: {error}")
        return EXIT_NO_SESSION

    def fetch_page(page, per_page):
        query = urllib.parse.urlencode({"page": page, "per_page": per_page})
        return request_json(f"{API_ROOT}/athlete/activities?{query}", token=token) or []

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
        activity_id = int(activity["id"])
        try:
            query = urllib.parse.urlencode({"keys": "latlng,altitude,time",
                                            "key_by_type": "true"})
            streams = request_json(
                f"{API_ROOT}/activities/{activity_id}/streams?{query}", token=token)

            name = output_name(activity_id, parse_timestamp(activity.get("start_date")))
            with open(os.path.join(output_folder, name), "wb") as out:
                out.write(build_gpx(activity, streams))
            imported += 1
        except RateLimited as error:
            # Nothing later in the loop would fare any better, and the ones
            # already on disk are kept: the next import carries on from there.
            say(f"error {error}")
            break
        except Exception as error:
            # One activity Strava will not part with should not abandon the
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
