#ifndef filters_h
#define filters_h

#include <time.h>
#include "filter_types.h"
#include "gpx_types.h"

void apply_filter_values(GpxCollection *c);
void reset_filters(FilterSettings *filter);
void save_filter_values(FilterSettings *filter);
#endif