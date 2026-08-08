#include "gpx_parser.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "log.h"
#include "progress.h"
#include "settings.h"
#include "time_util.h"
#include "track_splits.h"

static double haversine_distance(double lat1, double lon1, double lat2, double lon2) {
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;

    lat1 = lat1 * M_PI / 180.0;
    lat2 = lat2 * M_PI / 180.0;

    double a = sin(dlat / 2) * sin(dlat / 2) +
               sin(dlon / 2) * sin(dlon / 2) * cos(lat1) * cos(lat2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));

    return EARTH_RADIUS_METERS * c;
}

static void track_calculate_distance(GpxTrack *track) {
    track->distance = 0.0;

    if (track->total_points < 2)
        return;

    track->points[0].partial_distance = 0;
    for (int i = 1; i < track->total_points; i++) {
        GpxPoint *p1 = &track->points[i - 1];
        GpxPoint *p2 = &track->points[i];

        double dist = haversine_distance(p1->lat, p1->lon, p2->lat, p2->lon);
        track->distance += dist;
        track->points[i].partial_distance = track->distance;
    }
    track->distance = track->distance / 1000; // meters to kilometers
}

// Timestamps for the track being parsed, one slot per point in track->points
// and grown alongside it. Scratch for the scan only: what survives on the track
// is the pair of time_t and the split times derived from these, so nothing
// per-point is carried into the million-point array the collection keeps.
//
// Reused across files rather than allocated per file, so the buffer ends up the
// size of the longest single track instead of being taken and given back a
// thousand times.
typedef struct TrackTimes {
    time_t *at; // (time_t)-1 where a <trkpt> carried no usable <time>
    int count;  // slots filled; always equal to track->total_points
    int capacity;
} TrackTimes;

static bool track_times_reserve(TrackTimes *times, int count) {
    if (count <= times->capacity)
        return true;

    // Doubling from the same 256 the points grow from, so the two arrays take
    // the same number of reallocations to fill.
    int grown_capacity = times->capacity == 0 ? 256 : times->capacity;
    while (grown_capacity < count)
        grown_capacity *= 2;

    time_t *temp = (time_t *)realloc(times->at, (size_t)grown_capacity * sizeof(time_t));
    if (temp == NULL) {
        fprintf(stderr, "Memory reallocation for track times failed.\n");
        return false;
    }

    times->at = temp;
    times->capacity = grown_capacity;
    return true;
}

// Web Mercator. The result is in world pixels at `zoom`, which for MAX_ZOOM is
// a 268-million-pixel square -- well past what a float's 24-bit mantissa can
// address, so every step here stays in double precision.
static void lat_lon_to_pixel(double lat, double lon, int zoom, int *x, int *y) {
    // Web Mercator is only defined up to the latitude that makes the projected
    // world square; past it the log runs away and the point lands outside the
    // map entirely. Clamping the latitude, rather than the sine of it, is what
    // puts the limit in the right place: the old bound of 0.9999 on sin(lat)
    // corresponds to about 89.2 degrees and still projected to a negative
    // world_y.
    if (lat > MERCATOR_MAX_LATITUDE)
        lat = MERCATOR_MAX_LATITUDE;
    if (lat < -MERCATOR_MAX_LATITUDE)
        lat = -MERCATOR_MAX_LATITUDE;
    if (lon > 180.0)
        lon = 180.0;
    if (lon < -180.0)
        lon = -180.0;

    double siny = sin(lat * M_PI / 180.0);

    int scale = TILE_SIZE << zoom;

    double world_x = (lon + 180.0) / 360.0;
    double world_y = 0.5 - log((1 + siny) / (1 - siny)) / (4.0 * M_PI);

    *x = (int)(world_x * scale);
    *y = (int)(world_y * scale);

    // The clamps above put both fractions in [0, 1], and 1 lands one pixel past
    // the last addressable column or row.
    if (*x >= scale)
        *x = scale - 1;
    if (*y >= scale)
        *y = scale - 1;
    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
}

// Compared case-insensitively. The converter writes "Running", but files that
// came through other tools spell it "running", and those used to read as Other
// -- which would keep them out of every record that asks for a run.
static ActivityType activity_type_from_string(const xmlChar *type_str) {
    if (xmlStrcasecmp(type_str, (const xmlChar *)"running") == 0)
        return Run;
    if (xmlStrcasecmp(type_str, (const xmlChar *)"hiking") == 0)
        return Hike;
    if (xmlStrcasecmp(type_str, (const xmlChar *)"cycling") == 0)
        return Cycling;
    return Other;
}

static bool gpx_extract_act_type(xmlNode *node, GpxTrack *track) {
    for (xmlNode *cur_node = node; cur_node; cur_node = cur_node->next) {
        if (cur_node->type == XML_ELEMENT_NODE) {
            if (xmlStrcmp(cur_node->name, (const xmlChar *)"type") == 0) {
                if (cur_node->children && cur_node->children->content) {
                    track->act_type = activity_type_from_string(cur_node->children->content);
                    LOG_DEBUG("act_type %d\n", (int)track->act_type);
                }
                return true; // found <type>, done
            }
        }

        gpx_extract_act_type(cur_node->children, track);
    }
    return true;
}

// The heart rate carried by a <trkpt>'s <extensions>, or 0.
//
// Matched on the local name, which is what libxml2 puts in node->name: the
// element is spelled gpxtpx:hr inside a TrackPointExtension by Garmin and
// gpxdata:hr by other exporters, and both are the same number. The search is
// recursive because the wrapper element differs between them and is sometimes
// absent altogether.
static uint16_t extensions_heart_rate(xmlNode *node) {
    for (xmlNode *child = node; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;

        if (xmlStrcasecmp(child->name, (const xmlChar *)"hr") == 0) {
            xmlChar *content = xmlNodeGetContent(child);
            if (!content)
                continue;

            int bpm = atoi((const char *)content);
            xmlFree(content);
            // A reading outside this is a parse of something that is not a
            // heart rate; 0 is how a point without one is spelled.
            if (bpm > 0 && bpm <= UINT16_MAX)
                return (uint16_t)bpm;
            continue;
        }

        uint16_t nested = extensions_heart_rate(child->children);
        if (nested)
            return nested;
    }
    return 0;
}

// One numeric attribute of a <summary>, or 0 where it is absent.
static double summary_number(xmlNode *node, const char *name) {
    xmlChar *value = xmlGetProp(node, (const xmlChar *)name);
    if (!value)
        return 0.0;

    double number = atof((const char *)value);
    xmlFree(value);
    return number;
}

// The numbers an activity recorded without GPS was imported with, or false if
// the document carries none.
//
// A GPX has nowhere to say how far an activity went -- distance is a thing the
// parser derives from the path -- so an import that knows the totals for an
// activity that has no path writes them into <trk><extensions> as
//
//     <summary start="..." distance="..." duration="..." ascent="..."
//              descent="..."/>
//
// with the metres and seconds every other number in a GPX is spelled in.
// Matched on the local name and searched recursively for the same reason the
// heart rate above is: the prefix is whatever the writer chose.
static bool extract_summary(xmlNode *node, GpxTrack *track) {
    for (xmlNode *child = node; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;

        if (xmlStrcasecmp(child->name, (const xmlChar *)"summary") == 0) {
            xmlChar *start = xmlGetProp(child, (const xmlChar *)"start");
            if (!start)
                continue; // nothing to date it by, so nothing worth reading

            time_t start_utc = iso8601_to_utc((const char *)start);
            xmlFree(start);
            if (start_utc == (time_t)-1)
                continue;

            // Absent reads as zero, which is what a treadmill's ascent
            // genuinely is and what an unmeasured total is indistinguishable
            // from anyway.
            double distance = summary_number(child, "distance");
            double duration = summary_number(child, "duration");

            track->start_utc = start_utc;
            track->end_utc = start_utc + (time_t)duration;
            track->duration_secs = (float)duration;
            track->distance = (float)(distance / 1000.0); // metres to kilometres
            track->elev_up = (float)summary_number(child, "ascent");
            track->elev_down = (float)summary_number(child, "descent");
            track->secs_per_km = (track->distance > 0.0f)
                                     ? track->duration_secs / track->distance
                                     : 0.0f;
            return true;
        }

        if (extract_summary(child->children, track))
            return true;
    }
    return false;
}

// Walks the document for <trkpt> elements, appending a point for each and its
// timestamp to `times`. The timestamps used to be read by a second walk of the
// same nodes that kept only the first and the last; the split search needs all
// of them, and taking them here is what guarantees times->at[i] belongs to
// track->points[i].
static bool gpx_extract_coords(xmlNode *node, GpxTrack *track, TrackTimes *times) {
    for (xmlNode *cur_node = node; cur_node; cur_node = cur_node->next) {
        if (cur_node->type == XML_ELEMENT_NODE && xmlStrcmp(cur_node->name, (const xmlChar *)"trkpt") == 0) {
            int new_total = track->total_points + 1;
            if (new_total > track->points_capacity) {
                // Doubling, so a track of n points costs O(log n)
                // reallocations rather than one per point.
                int grown_capacity = track->points_capacity == 0 ? 256 : track->points_capacity * 2;
                GpxPoint *temp = (GpxPoint *)realloc(track->points, (size_t)grown_capacity * sizeof(GpxPoint));
                if (temp == NULL) {
                    fprintf(stderr, "Memory reallocation for track points failed.\n");
                    // The collection owns track->points, so it is left alone
                    // here and released with the rest of the collection.
                    return false;
                }
                track->points = temp;
                track->points_capacity = grown_capacity;
            }
            track->total_points = new_total;

            // realloc hands back uninitialised memory, and a <trkpt> missing
            // its coordinates skips the assignments further down, so the slot
            // starts from a known value rather than from whatever the heap had.
            track->points[new_total - 1] = (GpxPoint){.elapsed_secs = NAN};

            // Grown in lockstep with the points, so times->at[i] is always the
            // timestamp of track->points[i]. The slot is defaulted here rather
            // than after the lat/lon check below, so the two arrays cannot fall
            // out of step on a <trkpt> that is missing its coordinates.
            if (!track_times_reserve(times, new_total))
                return false;
            times->at[new_total - 1] = (time_t)-1;
            times->count = new_total;

            xmlChar *s_lat = xmlGetProp(cur_node, (const xmlChar *)"lat");
            xmlChar *s_lon = xmlGetProp(cur_node, (const xmlChar *)"lon");

            double elevation = 0.0;
            uint16_t heart_rate = 0;
            bool elevation_found = false;
            bool time_found = false;

            // One pass of the children for all three. This loop used to break
            // out as soon as it had the elevation, which would now leave the
            // timestamp behind on every point of every file. There is no
            // early-out left: the heart rate lives in <extensions>, which a
            // well-formed trkpt writes after both of the others.
            for (xmlNode *child = cur_node->children; child; child = child->next) {
                if (child->type != XML_ELEMENT_NODE)
                    continue;

                if (!elevation_found && xmlStrcmp(child->name, (const xmlChar *)"ele") == 0) {
                    xmlChar *ele_content = xmlNodeGetContent(child);
                    if (ele_content) {
                        elevation = atof((const char *)ele_content);
                        elevation_found = true;
                        xmlFree(ele_content);
                    }
                } else if (!time_found && xmlStrcmp(child->name, (const xmlChar *)"time") == 0) {
                    xmlChar *time_content = xmlNodeGetContent(child);
                    if (time_content) {
                        // The first <time> wins, as the first <ele> does. A
                        // well-formed trkpt only has the one.
                        times->at[new_total - 1] = iso8601_to_utc((const char *)time_content);
                        time_found = true;
                        xmlFree(time_content);
                    }
                } else if (heart_rate == 0 && xmlStrcmp(child->name, (const xmlChar *)"extensions") == 0) {
                    heart_rate = extensions_heart_rate(child->children);
                }
            }

            if (s_lat && s_lon) {
                double lat = atof((const char *)s_lat);
                double lon = atof((const char *)s_lon);
                int world_x, world_y;
                lat_lon_to_pixel(lat, lon, MAX_ZOOM, &world_x, &world_y);

                GpxPoint *pt = &track->points[new_total - 1];
                pt->lat = lat;
                pt->lon = lon;
                pt->world_x = world_x;
                pt->world_y = world_y;
                pt->track_id = track->track_id;
                pt->heat = 1;
                pt->heart_rate = heart_rate;
                track->has_path = true;

                if (elevation_found)
                    pt->elevation = elevation;
                else
                    pt->elevation = 0.0;
            }

            if (s_lat)
                xmlFree(s_lat);
            if (s_lon)
                xmlFree(s_lon);
        }

        if (!gpx_extract_coords(cur_node->children, track, times))
            return false;
    }
    return true;
}

static void track_calculate_mid_point(GpxTrack *track) {
    uint64_t mid_x = 0;
    uint64_t mid_y = 0;
    // Averaging the points of a track that never had coordinates would put its
    // middle at world pixel (0, 0), and the map would fly to Null Island for it.
    if (track->total_points <= 0 || !track->has_path)
        return;

    for (int i = 0; i < track->total_points; i++) {
        mid_x += track->points[i].world_x;
        mid_y += track->points[i].world_y;
    }
    track->mid_x = mid_x / track->total_points;
    track->mid_y = mid_y / track->total_points;
}

// Raw GPS elevation is noisy enough that summing consecutive differences
// wildly overstates the climb, so the series is smoothed with a centred moving
// average before the gain and loss are accumulated.
static void track_calculate_elevation_gain_loss(GpxTrack *track) {
    int total_points = track->total_points;

    if (total_points < 2)
        return;

    float *smoothed = (float *)malloc(total_points * sizeof(float));
    if (!smoothed) {
        fprintf(stderr, "Memory allocation failed in elevation smoothing.\n");
        return;
    }

    // Centred moving average over [i - ELEVATION_SMOOTHING_WINDOW,
    // i + ELEVATION_SMOOTHING_WINDOW], clamped at both ends of the track.
    //
    // The window is advanced by adding the point entering it and subtracting
    // the one leaving, so the whole pass is linear rather than one sum per
    // point. Every sample is read from track->points and every result is
    // written to smoothed[]: writing back inside this loop would feed each
    // average into the windows of the points after it, turning a symmetric
    // filter into a one-sided one that under-reports the descents.
    double sum = 0.0;
    int start = 0;
    int end = -1;
    for (int i = 0; i < total_points; i++) {
        int window_start = i - ELEVATION_SMOOTHING_WINDOW;
        int window_end = i + ELEVATION_SMOOTHING_WINDOW;
        if (window_start < 0)
            window_start = 0;
        if (window_end > total_points - 1)
            window_end = total_points - 1;

        while (end < window_end)
            sum += track->points[++end].elevation;
        while (start < window_start)
            sum -= track->points[start++].elevation;

        smoothed[i] = (float)(sum / (end - start + 1));
    }

    for (int i = 0; i < total_points; i++)
        track->points[i].elevation = smoothed[i];

    track->elev_up = 0.0;
    track->elev_down = 0.0;
    track->high_point = smoothed[0];
    track->low_point = smoothed[0];

    for (int i = 1; i < total_points; i++) {
        float diff = smoothed[i] - smoothed[i - 1];
        if (diff > 0)
            track->elev_up += diff;
        else
            track->elev_down += -diff;

        if (smoothed[i] > track->high_point)
            track->high_point = smoothed[i];
        if (smoothed[i] < track->low_point)
            track->low_point = smoothed[i];
    }

    free(smoothed);
}

static bool gpx_parse_file(char *filename, GpxTrack *track, TrackTimes *times) {
    LOG_DEBUG("Parsing: %s\n", filename);
    xmlDocPtr doc;
    xmlNode *root_element;
    LIBXML_TEST_VERSION
    doc = xmlReadFile(filename, NULL, 0);

    if (doc == NULL) {
        fprintf(stderr, "Failed to read %s\n", filename);
        return false;
    }
    root_element = xmlDocGetRootElement(doc);

    // The buffer carries over from the file before this one; the count does
    // not.
    times->count = 0;

    gpx_extract_act_type(root_element, track);

    if (gpx_extract_coords(root_element, track, times)) {
        track_calculate_mid_point(track);
        LOG_DEBUG("Mid_x: %d, Mid_y: %d\n", track->mid_x, track->mid_y);

        track_calculate_distance(track);
        LOG_DEBUG("Track distance: %.2f km\n", track->distance);

        track_calculate_elevation_gain_loss(track);
        LOG_DEBUG("Highest elevation: %.2f m\n", track->high_point);
        LOG_DEBUG("Lowest elevation: %.2f m\n", track->low_point);
        LOG_DEBUG("Total elevation up: %.2f m\n", track->elev_up);
        LOG_DEBUG("Total elevation down: %.2f m\n", track->elev_down);

        // The first and last <trkpt> that carried a usable time -- the same two
        // the retired second walk of the document picked out, now read off the
        // array the first walk has already filled. A track with no times at all
        // keeps the (time_t)-1 it was initialised with, so the four planned
        // routes in a library of recordings stay out of everything that is
        // dated.
        time_t start = (time_t)-1;
        time_t end = (time_t)-1;
        for (int i = 0; i < times->count; i++) {
            if (times->at[i] == (time_t)-1)
                continue;
            if (start == (time_t)-1)
                start = times->at[i];
            end = times->at[i];
        }

        if (start != (time_t)-1) {
            track->start_utc = start;
            track->end_utc = end;

            // Now that the first timed point is known, every point can say how
            // far into the run it is. A point that carried no time keeps the
            // NAN it was initialised with, which is what the graphs read as a
            // gap rather than as a moment at the start of the track.
            for (int i = 0; i < times->count && i < track->total_points; i++)
                if (times->at[i] != (time_t)-1)
                    track->points[i].elapsed_secs = (float)difftime(times->at[i], start);

            if (end >= start) {
                track->duration_secs = difftime(end, start);
                track->secs_per_km = (track->distance > 0.0f)
                                         ? track->duration_secs / track->distance
                                         : 0.0f;
            }
            LOG_DEBUG("duration_secs: %f\n", track->duration_secs);
            LOG_DEBUG("distance: %f\n", track->distance);
            LOG_DEBUG("secs_per_km: %f\n", track->secs_per_km);
        }

        track_splits_compute(track, times->at);
        LOG_DEBUG("5k: %f, 10k: %f\n", track->splits[SPLIT_5K],
                  track->splits[SPLIT_10K]);

        // Only where there is no path to derive them from. An outdoor track
        // keeps the numbers measured above, so a stray <summary> cannot
        // contradict what the trackpoints say. The high and low points and the
        // splits stay zero: those are shapes of a run, not totals, and there is
        // no series here to take them from.
        if (!track->has_path && extract_summary(root_element, track))
            LOG_DEBUG("summary: %.2f km in %.0f s\n", (double)track->distance,
                      (double)track->duration_secs);
    }

    // xmlCleanupParser() is a once-per-process teardown call, not a per-document
    // one; it runs in gpx_parser_cleanup() after all files have been parsed.
    xmlFreeDoc(doc);
    return true;
}

// Joins a directory and one of its entries. False when the result would not
// fit, which is also what keeps -Wformat-truncation quiet about a path built
// from a path.
static bool join_path(char *out, size_t size, const char *folder_path, const char *name) {
    int written = snprintf(out, size, "%s/%s", folder_path, name);
    return written > 0 && (size_t)written < size;
}

// Whether an entry is itself a directory. d_type is the cheap answer and the
// one nearly every filesystem gives; a few report DT_UNKNOWN for everything,
// which is what the stat is for.
static bool entry_is_dir(const char *full_path, const struct dirent *entry) {
    if (entry->d_type != DT_UNKNOWN)
        return entry->d_type == DT_DIR;

    struct stat info;
    return stat(full_path, &info) == 0 && S_ISDIR(info.st_mode);
}

// Every entry worth descending into or parsing. "." and ".." would walk the
// scan back up the tree, so they are what makes the recursion terminate as much
// as the depth limit is.
static bool entry_is_self_or_parent(const struct dirent *entry) {
    return strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0;
}

// Counts the .gpx files under the folder, so progress has a denominator. It
// descends the same way the scan does, because a count taken over one level
// while the scan reads several would leave the bar short by every file in a
// subfolder. A count that disagrees only makes the bar wrong; nothing is sized
// from it.
static int count_gpx_files(const char *folder_path, int depth) {
    DIR *dir = opendir(folder_path);
    if (!dir)
        return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry_is_self_or_parent(entry))
            continue;

        char full_path[GPX_PATH_MAX];
        if (!join_path(full_path, sizeof(full_path), folder_path, entry->d_name))
            continue;

        if (entry_is_dir(full_path, entry)) {
            if (depth + 1 < GPX_SCAN_MAX_DEPTH)
                count += count_gpx_files(full_path, depth + 1);
        } else if (strstr(entry->d_name, ".gpx") != NULL) {
            count++;
        }
    }

    closedir(dir);
    return count;
}

// Parses every .gpx file under `folder_path` into the collection, descending
// into subfolders so a library can be filed into them -- which is what the
// Garmin import's own folder is. False means the scan could not be finished:
// out of memory, or the library folder itself would not open.
static bool scan_dir(const char *folder_path, GpxCollection *collection,
                     TrackTimes *times, const Progress *progress, int depth) {
    DIR *dir = opendir(folder_path);
    if (dir == NULL) {
        perror("opendir");
        // A subfolder that cannot be read costs only its own files. The library
        // folder not opening is the whole library missing, and the caller says
        // so.
        return depth > 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (progress_cancelled(progress))
            break;

        if (entry_is_self_or_parent(entry))
            continue;

        char full_path[GPX_PATH_MAX];
        if (!join_path(full_path, sizeof(full_path), folder_path, entry->d_name))
            continue;

        if (entry_is_dir(full_path, entry)) {
            if (depth + 1 < GPX_SCAN_MAX_DEPTH &&
                !scan_dir(full_path, collection, times, progress, depth + 1)) {
                closedir(dir);
                return false;
            }
            continue;
        }

        if (strstr(entry->d_name, ".gpx") == NULL)
            continue;

        GpxTrack *grown = (GpxTrack *)realloc(
            collection->tracks,
            (collection->total_tracks + 1) * sizeof(GpxTrack));

        if (!grown) {
            perror("realloc");
            closedir(dir);
            return false;
        }
        collection->tracks = grown;

        GpxTrack *current = &collection->tracks[collection->total_tracks];

        // realloc hands back uninitialised memory, so every field starts from a
        // known value. Only some of them are written unconditionally further
        // down: act_type needs a <type> element, and the elevation figures need
        // enough points for the smoothing window.
        *current = (GpxTrack){0};
        current->track_id = collection->total_tracks;
        current->act_type = Other;
        current->start_utc = (time_t)-1;
        current->end_utc = (time_t)-1;

        gpx_parse_file(full_path, current, times);
        LOG_DEBUG("Tracks %d has %d data points\n", collection->total_tracks, current->total_points);
        collection->total_tracks++;
        progress_add(progress, 1);
    }

    closedir(dir);
    return true;
}

bool gpx_parse_all_files(GpxCollection *collection, const Progress *progress) {
    const char *folder_path = settings.gpx_dir;

    collection->total_tracks = 0;
    collection->tracks = NULL;

    // One buffer for the whole scan, grown to whatever the longest file needs
    // and released at the end. Nothing on the collection points into it.
    TrackTimes times = {0};

    progress_set_total(progress, count_gpx_files(folder_path, 0));
    progress_set_completed(progress, 0);

    bool scanned = scan_dir(folder_path, collection, &times, progress, 0);
    free(times.at);
    if (!scanned)
        return false;

    // Sized from the tracks actually parsed. A second scan of the directory
    // could disagree with the first, and the loop below would then write past
    // the end of the array.
    free(collection->list_order);
    collection->list_order = NULL;
    if (collection->total_tracks > 0) {
        collection->list_order = malloc(collection->total_tracks * sizeof(int));
        if (!collection->list_order) {
            perror("malloc");
            return false;
        }
    }

    for (int i = 0; i < collection->total_tracks; i++) {
        collection->list_order[i] = i;
    }
    LOG_DEBUG("Tracks: %d\n", collection->total_tracks);

    return true;
}

// Releases libxml2's global state. Call once, after all parsing is done.
void gpx_parser_cleanup(void) {
    xmlCleanupParser();
}
