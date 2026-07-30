#ifndef GPX_PARSER_H
#define GPX_PARSER_H

#include <stdbool.h>

#include "gpx_types.h"

#define EARTH_RADIUS_METERS 6371000.0

bool gpx_parse_all_files(GpxCollection *collection);
void gpx_parser_cleanup(void);

#endif