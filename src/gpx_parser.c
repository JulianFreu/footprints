#include "gpx_parser.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "log.h"
#include "progress.h"
#include "time_util.h"

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

// The first and last timestamps seen while walking the document. These are
// scratch for the parse only -- what survives is the pair of time_t on the
// track -- so they live here rather than on GpxTrack.
typedef struct TrackTimeRange {
    char start[64];
    char end[64];
    bool found_start;
} TrackTimeRange;

static bool gpx_extract_time(xmlNode *node, TrackTimeRange *range) {
    bool found_time = false;

    for (xmlNode *cur_node = node; cur_node; cur_node = cur_node->next) {
        if (cur_node->type == XML_ELEMENT_NODE &&
            xmlStrcmp(cur_node->name, (const xmlChar *)"trkpt") == 0) {
            for (xmlNode *child = cur_node->children; child; child = child->next) {
                if (child->type == XML_ELEMENT_NODE &&
                    xmlStrcmp(child->name, (const xmlChar *)"time") == 0) {
                    xmlChar *time_content = xmlNodeGetContent(child);
                    if (time_content) {
                        if (!range->found_start) {
                            snprintf(range->start, sizeof(range->start), "%s", (const char *)time_content);
                            range->found_start = true;
                        }

                        // Always update end time with the latest <time>
                        snprintf(range->end, sizeof(range->end), "%s", (const char *)time_content);

                        xmlFree(time_content);
                        found_time = true;
                    }
                }
            }
        }

        // Recurse into children
        if (gpx_extract_time(cur_node->children, range))
            found_time = true;
    }

    return found_time;
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

static bool gpx_extract_act_type(xmlNode *node, GpxTrack *track) {
    for (xmlNode *cur_node = node; cur_node; cur_node = cur_node->next) {
        if (cur_node->type == XML_ELEMENT_NODE) {
            if (xmlStrcmp(cur_node->name, (const xmlChar *)"type") == 0) {
                if (cur_node->children && cur_node->children->content) {
                    char *type_str = (char *)cur_node->children->content;

                    if (strcmp(type_str, "Running") == 0) {
                        track->act_type = Run;
                        LOG_DEBUG("run\n");
                    } else if (strcmp(type_str, "Hiking") == 0 || strcmp(type_str, "hiking") == 0) {
                        track->act_type = Hike;
                        LOG_DEBUG("hike\n");
                    } else if (strcmp(type_str, "Cycling") == 0) {
                        track->act_type = Cycling;
                        LOG_DEBUG("cycle\n");
                    } else
                        track->act_type = Other;
                }
                return true; // found <type>, done
            }
        }

        // Recurse into children
        gpx_extract_act_type(cur_node->children, track);
    }
    return true;
}

static bool gpx_extract_coords(xmlNode *node, GpxTrack *track) {
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

            xmlChar *s_lat = xmlGetProp(cur_node, (const xmlChar *)"lat");
            xmlChar *s_lon = xmlGetProp(cur_node, (const xmlChar *)"lon");

            double elevation = 0.0;
            bool elevation_found = false;

            for (xmlNode *child = cur_node->children; child; child = child->next) {
                if (child->type == XML_ELEMENT_NODE && xmlStrcmp(child->name, (const xmlChar *)"ele") == 0) {
                    xmlChar *ele_content = xmlNodeGetContent(child);
                    if (ele_content) {
                        elevation = atof((const char *)ele_content);
                        elevation_found = true;
                        xmlFree(ele_content);
                        break;
                    }
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

        gpx_extract_coords(cur_node->children, track);
    }
    return true;
}

static void track_calculate_mid_point(GpxTrack *track) {
    uint64_t mid_x = 0;
    uint64_t mid_y = 0;
    if (track->total_points <= 0)
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

    // Temporary array for smoothed elevation values
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

    // Compute gain/loss using smoothed values
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

static bool gpx_parse_file(char *filename, GpxTrack *track) {
    LOG_DEBUG("Parsing: %s\n", filename);
    xmlDocPtr doc;
    xmlNode *root_element;
    LIBXML_TEST_VERSION
    // Parse the XML file
    doc = xmlReadFile(filename, NULL, 0);

    if (doc == NULL) {
        fprintf(stderr, "Failed to read %s\n", filename);
        return false;
    }
    // Get the root element
    root_element = xmlDocGetRootElement(doc);

    // get activity type
    gpx_extract_act_type(root_element, track);

    if (gpx_extract_coords(root_element, track)) {
        track_calculate_mid_point(track);
        LOG_DEBUG("Mid_x: %d, Mid_y: %d\n", track->mid_x, track->mid_y);

        track_calculate_distance(track);
        LOG_DEBUG("Track distance: %.2f km\n", track->distance);

        track_calculate_elevation_gain_loss(track);
        LOG_DEBUG("Highest elevation: %.2f m\n", track->high_point);
        LOG_DEBUG("Lowest elevation: %.2f m\n", track->low_point);
        LOG_DEBUG("Total elevation up: %.2f m\n", track->elev_up);
        LOG_DEBUG("Total elevation down: %.2f m\n", track->elev_down);

        TrackTimeRange times = {0};
        if (gpx_extract_time(root_element, &times)) {
            time_t start = iso8601_to_utc(times.start);
            time_t end = iso8601_to_utc(times.end);
            track->start_utc = start;
            track->end_utc = end;

            if (start != (time_t)-1 && end != (time_t)-1 && end >= start) {
                track->duration_secs = difftime(end, start);
                track->secs_per_km = (track->distance > 0.0f)
                                         ? track->duration_secs / track->distance
                                         : 0.0f;
            } else {
                track->duration_secs = 0.0f;
                track->secs_per_km = 0.0f;
            }
            LOG_DEBUG("start_time: %s\n", times.start);
            LOG_DEBUG("end_time: %s\n", times.end);
            LOG_DEBUG("duration_secs: %f\n", track->duration_secs);
            LOG_DEBUG("distance: %f\n", track->distance);
            LOG_DEBUG("secs_per_km: %f\n", track->secs_per_km);
        }
    }

    // Free resources
    // xmlCleanupParser() is a once-per-process teardown call, not a per-document
    // one; it runs in gpx_parser_cleanup() after all files have been parsed.
    xmlFreeDoc(doc);
    return true;
}

// Counts the .gpx files in the folder, so progress has a denominator. A count
// that disagrees with what is actually parsed only makes the bar slightly
// wrong; nothing is sized from it.
static int count_gpx_files(const char *folder_path) {
    DIR *dir = opendir(folder_path);
    if (!dir)
        return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        if (strstr(entry->d_name, ".gpx") != NULL)
            count++;

    closedir(dir);
    return count;
}

bool gpx_parse_all_files(GpxCollection *collection, const Progress *progress) {
    const char *folder_path = GPX_INPUT_DIR;
    DIR *dir;
    struct dirent *entry;

    dir = opendir(folder_path);
    if (dir == NULL) {
        perror("opendir");
        return false;
    }

    collection->total_tracks = 0;
    collection->tracks = NULL;

    progress_set_total(progress, count_gpx_files(folder_path));
    progress_set_completed(progress, 0);

    while ((entry = readdir(dir)) != NULL) {
        if (progress_cancelled(progress))
            break;

        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // only read .gpx files
        if (strstr(entry->d_name, ".gpx") == NULL)
            continue;

        // Reallocate track array for new file
        collection->tracks = (GpxTrack *)realloc(
            collection->tracks,
            (collection->total_tracks + 1) * sizeof(GpxTrack));

        if (!collection->tracks) {
            perror("realloc");
            closedir(dir);
            return false;
        }

        // Construct full path
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", folder_path, entry->d_name);
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

        gpx_parse_file(full_path, current);
        LOG_DEBUG("Tracks %d has %d data points\n", collection->total_tracks, collection->tracks[collection->total_tracks].total_points);
        collection->total_tracks++;
        progress_add(progress, 1);
    }

    closedir(dir);

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
        // give list order initial values
        collection->list_order[i] = i;
    }
    LOG_DEBUG("Tracks: %d\n", collection->total_tracks);

    return true;
}

// Releases libxml2's global state. Call once, after all parsing is done.
void gpx_parser_cleanup(void) {
    xmlCleanupParser();
}
