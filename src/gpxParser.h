#ifndef gpxParse_h
#define gpxParse_h

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "gpx_types.h"
#include "map.h"

#define EARTH_RADIUS_METERS 6371000.0

bool gpx_parse_all_files(GpxCollection *collection);
int gpx_count_files();
void gpx_parser_cleanup(void);

#endif