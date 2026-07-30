#ifndef HEAT_H
#define HEAT_H

#include <stdbool.h>

#include "gpx_types.h"
#include "progress.h"

// Assigns every visible point its heat. `progress` may be NULL; when it is
// not, the calculation reports how far it has got and stops early if asked.
bool calculate_heatmap(GpxCollection *collection, const Progress *progress);

#endif