#include "gpxParser.h"

// Cross-platform ISO8601 parser: "YYYY-MM-DDTHH:MM:SS[.sss]Z"
time_t parse_iso8601_utc(const char *timestr)
{
    struct tm tm = {0};
    int year, month, day, hour, min, sec;

    // Parse time up to seconds — ignore milliseconds and 'Z'
    if (sscanf(timestr, "%4d-%2d-%2dT%2d:%2d:%2d",
               &year, &month, &day, &hour, &min, &sec) != 6)
    {
        return (time_t)-1;
    }

    tm.tm_year = year - 1900; // years since 1900
    tm.tm_mon = month - 1;    // months since January [0-11]
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = 0; // not daylight saving time

#if defined(_WIN32) || defined(_WIN64)
    // Windows: mktime() is local time, so correct for UTC manually
    time_t local = mktime(&tm);
    if (local == (time_t)-1)
        return (time_t)-1;

    // Convert to UTC manually
    struct tm *gmt = gmtime(&local);
    if (!gmt)
        return (time_t)-1;

    time_t local_as_utc = mktime(gmt);
    time_t offset = difftime(local_as_utc, local);

    return local - offset;
#else
    // Linux/macOS: use timegm() if available
    return timegm(&tm);
#endif
}

bool format_iso8601_display_strings(const char *iso8601, char *out_date, size_t date_size, char *out_time, size_t time_size)
{
    struct tm tm = {0};
    int year, month, day, hour, min, sec;

    // Parse: "YYYY-MM-DDTHH:MM:SS"
    if (sscanf(iso8601, "%4d-%2d-%2dT%2d:%2d:%2d",
               &year, &month, &day, &hour, &min, &sec) != 6)
    {
        return false;
    }

    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;

    // Format date and time strings
    strftime(out_date, date_size, "%d.%m.%Y", &tm);
    strftime(out_time, time_size, "%H:%M", &tm);

    return true;
}

static double haversine_distance(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;

    lat1 = lat1 * M_PI / 180.0;
    lat2 = lat2 * M_PI / 180.0;

    double a = sin(dlat / 2) * sin(dlat / 2) +
               sin(dlon / 2) * sin(dlon / 2) * cos(lat1) * cos(lat2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));

    return EARTH_RADIUS_METERS * c;
}

void gpxTrack_CalculateDistance(GpxTrack *track)
{
    track->distance = 0.0;

    if (track->total_points < 2)
        return;

    track->points[0].partial_distance = 0;
    for (int i = 1; i < track->total_points; i++)
    {
        GpxPoint *p1 = &track->points[i - 1];
        GpxPoint *p2 = &track->points[i];

        double dist = haversine_distance(p1->lat, p1->lon, p2->lat, p2->lon);
        track->distance += dist;
        track->points[i].partial_distance = track->distance;
    }
    track->distance = track->distance / 1000; // meters to kilometers
}

bool gpxParser_ExtractTime(xmlNode *node, GpxTrack *track, bool *found_start_time)
{
    bool found_time = false;

    for (xmlNode *cur_node = node; cur_node; cur_node = cur_node->next)
    {
        if (cur_node->type == XML_ELEMENT_NODE &&
            xmlStrcmp(cur_node->name, (const xmlChar *)"trkpt") == 0)
        {
            for (xmlNode *child = cur_node->children; child; child = child->next)
            {
                if (child->type == XML_ELEMENT_NODE &&
                    xmlStrcmp(child->name, (const xmlChar *)"time") == 0)
                {
                    xmlChar *time_content = xmlNodeGetContent(child);
                    if (time_content)
                    {
                        if (!*found_start_time)
                        {
                            strncpy(track->start_time_raw, (const char *)time_content, sizeof(track->start_time_raw) - 1);
                            track->start_time_raw[sizeof(track->start_time_raw) - 1] = '\0';
                            *found_start_time = true;
                        }

                        // Always update end time with the latest <time>
                        strncpy(track->end_time_raw, (const char *)time_content, sizeof(track->end_time_raw) - 1);
                        track->end_time_raw[sizeof(track->end_time_raw) - 1] = '\0';

                        xmlFree(time_content);
                        found_time = true;
                    }
                }
            }
        }

        // Recurse into children
        if (gpxParser_ExtractTime(cur_node->children, track, found_start_time))
            found_time = true;
    }

    return found_time;
}

void latLonToPixel(double lat, double lon, int zoom, int *x, int *y)
{
    double lat_rad = lat * (double)M_PI / 180.0f;
    double siny = sinf(lat_rad);

    // Clamp siny to prevent overflow in log
    if (siny > 0.9999f)
        siny = 0.9999f;
    if (siny < -0.9999f)
        siny = -0.9999f;

    int scale = TILE_SIZE << zoom;

    double world_x = (lon + 180.0f) / 360.0f;
    double world_y = 0.5f - logf((1 + siny) / (1 - siny)) / (4.0f * (double)M_PI);

    *x = (int)(world_x * scale);
    *y = (int)(world_y * scale);
}

bool gpxParser_ExtractActType(xmlNode *node, GpxTrack *track)
{
    for (xmlNode *cur_node = node; cur_node; cur_node = cur_node->next)
    {
        if (cur_node->type == XML_ELEMENT_NODE)
        {
            if (xmlStrcmp(cur_node->name, (const xmlChar *)"type") == 0)
            {
                if (cur_node->children && cur_node->children->content)
                {
                    char *type_str = (char *)cur_node->children->content;

                    if (strcmp(type_str, "Running") == 0){
                        track->act_type = Run;
                        printf("run\n");
                    }
                    else if (strcmp(type_str, "Hiking") == 0 || strcmp(type_str, "hiking") == 0){
                        track->act_type = Hike;
                        printf("hike\n");
                    }
                    else if (strcmp(type_str, "Cycling") == 0){
                        track->act_type = Cycling;
                        printf("cycle\n");
                    }
                    else
                        track->act_type = Other;
                }
                return true; // found <type>, done
            }
        }

        // Recurse into children
        gpxParser_ExtractActType(cur_node->children, track);
    }
    return true;
}

bool gpxParser_ExtractCoords(xmlNode *node, GpxTrack *track)
{
    for (xmlNode *cur_node = node; cur_node; cur_node = cur_node->next)
    {
        if (cur_node->type == XML_ELEMENT_NODE && xmlStrcmp(cur_node->name, (const xmlChar *)"trkpt") == 0)
        {
            int new_total = track->total_points + 1;
            GpxPoint *temp = (GpxPoint *)realloc(track->points, new_total * sizeof(GpxPoint));
            if (temp == NULL)
            {
                fprintf(stderr, "Memory reallocation for tracks failed.\n");
                free(track);
                return false;
            }
            track->total_points = new_total;
            track->points = temp;

            xmlChar *s_lat = xmlGetProp(cur_node, (const xmlChar *)"lat");
            xmlChar *s_lon = xmlGetProp(cur_node, (const xmlChar *)"lon");

            double elevation = 0.0;
            bool elevation_found = false;

            for (xmlNode *child = cur_node->children; child; child = child->next)
            {
                if (child->type == XML_ELEMENT_NODE && xmlStrcmp(child->name, (const xmlChar *)"ele") == 0)
                {
                    xmlChar *ele_content = xmlNodeGetContent(child);
                    if (ele_content)
                    {
                        elevation = atof((const char *)ele_content);
                        elevation_found = true;
                        xmlFree(ele_content);
                        break;
                    }
                }
            }

            if (s_lat && s_lon)
            {
                double lat = atof((const char *)s_lat);
                double lon = atof((const char *)s_lon);
                int world_x, world_y;
                latLonToPixel(lat, lon, MAX_ZOOM, &world_x, &world_y);

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

        gpxParser_ExtractCoords(cur_node->children, track);
    }
    return true;
}

void gpxTrack_CalculateMidPoint(GpxTrack *track)
{
    uint64_t mid_x = 0;
    uint64_t mid_y = 0;
    for (int i = 0; i < track->total_points; i++)
    {
        mid_x += track->points->world_x;
        mid_y += track->points->world_y;
    }
   track->mid_x = mid_x / track->total_points;
   track->mid_y = mid_y / track->total_points;
}

void gpxTrack_CalculateElevationGainLoss(GpxTrack *track)
{
    const int window_size = 10; // Adjust as needed
    int total_points = track->total_points;

    if (total_points < 9)
        return;

    // Temporary array for smoothed elevation values
    float *smoothed = (float *)malloc(total_points * sizeof(float));
    if (!smoothed)
    {
        fprintf(stderr, "Memory allocation failed in elevation smoothing.\n");
        return;
    }

    // Apply rolling average smoothing
    for (int i = 0; i < total_points; i++)
    {
        int start = i - window_size;
        int end = i + window_size;
        if (start < 0)
            start = 0;
        if (end >= total_points)
            end = total_points - 1;

        float sum = 0.0;
        int count = 0;

        for (int j = start; j <= end; j++)
        {
            sum += track->points[j].elevation;
            count++;
        }

        smoothed[i] = sum / count;
        track->points[i].elevation = smoothed[i]; // save smoothed array
    }

    // Compute gain/loss using smoothed values
    track->elev_up = 0.0;
    track->elev_down = 0.0;
    track->high_point = smoothed[0];
    track->low_point = smoothed[0];

    for (int i = 1; i < total_points; i++)
    {
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

void calculateStringsForUI(GpxTrack *track)
{
    // duration
    int h = (int)track->duration_secs / 3600;
    int m = ((int)(track->duration_secs) % 3600) / 60;
    int s = (int)track->duration_secs % 60;
    snprintf(track->duration_str, sizeof(track->duration_str), "%02d:%02d:%02d", h, m, s);

    // time + date
    format_iso8601_display_strings(
        track->start_time_raw,
        track->start_date_str, sizeof(track->start_date_str),
        track->start_time_str, sizeof(track->start_time_str));

    // pace
    m = ((int)(track->secs_per_km)) / 60;
    s = (int)track->secs_per_km % 60;
    snprintf(track->pace_str, sizeof(track->pace_str), "%d:%02d", m, s);

    // elevation
    snprintf(track->elev_up_str, sizeof(track->elev_up_str), "%d", (int)track->elev_up);
    snprintf(track->elev_down_str, sizeof(track->elev_down_str), "%d", (int)track->elev_down);
    snprintf(track->high_point_str, sizeof(track->high_point_str), "%d", (int)track->high_point);
    snprintf(track->low_point_str, sizeof(track->low_point_str), "%d", (int)track->low_point);

    // distance
    snprintf(track->distance_str, sizeof(track->distance_str), "%.2f", track->distance);
}

bool gpxParser_ParseFile(char *filename, GpxTrack *track)
{
    printf("Parsing: %s\n", filename);
    xmlDocPtr doc;
    xmlNode *root_element;
    LIBXML_TEST_VERSION
    // Parse the XML file
    doc = xmlReadFile(filename, NULL, 0);

    if (doc == NULL)
    {
        fprintf(stderr, "Failed to read %s\n", filename);
        return false;
    }
    // Get the root element
    root_element = xmlDocGetRootElement(doc);

    // get activity type
    gpxParser_ExtractActType(root_element, track);

    if (gpxParser_ExtractCoords(root_element, track))
    {

        gpxTrack_CalculateMidPoint(track);
        printf("Mid_x: %d, Mid_y: %d\n", track->mid_x, track->mid_y);

        gpxTrack_CalculateDistance(track);
        printf("Track distance: %.2f km\n", track->distance);

        gpxTrack_CalculateElevationGainLoss(track);
        printf("Highest elevation: %.2f m\n", track->high_point);
        printf("Lowest elevation: %.2f m\n", track->low_point);
        printf("Total elevation up: %.2f m\n", track->elev_up);
        printf("Total elevation down: %.2f m\n", track->elev_down);

        bool found_start_time = false;
        if (gpxParser_ExtractTime(root_element, track, &found_start_time))
        {
            time_t start = parse_iso8601_utc(track->start_time_raw);
            time_t end = parse_iso8601_utc(track->end_time_raw);

            if (start != (time_t)-1 && end != (time_t)-1 && end >= start)
            {
                track->duration_secs = difftime(end, start);
                track->secs_per_km = track->duration_secs / track->distance;
            }
            else
            {
                track->duration_secs = 0.0f;
                track->secs_per_km = 0.0f;
            }
            printf("start_time: %s\n", track->start_time_raw);
            printf("end_time: %s\n", track->end_time_raw);
            printf("duration_secs: %f\n", track->duration_secs);
            printf("distance: %f\n", track->distance);
            printf("secs_per_km: %f\n", track->secs_per_km);
        }
    }

    // Free resources
    xmlFreeDoc(doc);
    xmlCleanupParser();
    return true;
}

int gpxParser_CountGpxFiles(){
    const char *folder_path = "./gpx_files"; // Folder containing GPX files
    DIR *dir;
    struct dirent *entry;

    dir = opendir(folder_path);
    if (dir == NULL)
    {
        perror("opendir");
        return false;
    }
    int gpx_file_counter = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // only read .gpx files
        if (strstr(entry->d_name, ".gpx") == NULL)
            continue;

        gpx_file_counter++;
    }

    printf("Found gpx files: %d\n", gpx_file_counter);

    closedir(dir);
    return gpx_file_counter;

}

bool gpxParser_ParseAllFiles(GpxCollection *collection)
{
    const char *folder_path = "./gpx_files"; // Folder containing GPX files
    DIR *dir;
    struct dirent *entry;

    dir = opendir(folder_path);
    if (dir == NULL)
    {
        perror("opendir");
        return false;
    }

    collection->total_tracks = 0;
    collection->tracks = NULL;

    while ((entry = readdir(dir)) != NULL)
    {
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

        if (!collection->tracks)
        {
            perror("realloc");
            closedir(dir);
            return false;
        }

        // Construct full path
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", folder_path, entry->d_name);
        GpxTrack *current = &collection->tracks[collection->total_tracks];

        // Parse file and fill current track
        current->track_id = collection->total_tracks;
        current->points = NULL;
        current->total_points = 0;

        // Call your GPX parsing function here
        gpxParser_ParseFile(full_path, current);
        printf("Tracks %d has %d data points\n", collection->total_tracks, collection->tracks[collection->total_tracks].total_points);
        collection->total_tracks++;
    }

    for (int i = 0; i < collection->total_tracks; i++)
    {
        calculateStringsForUI(&collection->tracks[i]);

        // give list order initial values
        collection->list_order[i] = i;
    }
    printf("Tracks: %d\n", collection->total_tracks);

    closedir(dir);
    return true;
}
