#ifndef GPX_PARSER_H
#define GPX_PARSER_H

#include <stdbool.h>

#include "gpx_types.h"
#include "progress.h"

#define EARTH_RADIUS_METERS 6371000.0

// Reads every .gpx in GPX_INPUT_DIR into the collection. `progress` may be
// NULL; when it is not, the scan reports how far it has got and stops early if
// asked.
bool gpx_parse_all_files(GpxCollection *collection, const Progress *progress);
void gpx_parser_cleanup(void);

#endif