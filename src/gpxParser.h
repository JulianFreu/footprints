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
#include "structs.h"
#include "map.h"

#define EARTH_RADIUS_METERS 6371000.0


bool gpxParser_ParseAllFiles(GpxCollection *collection);
int gpxParser_CountGpxFiles();

#endif