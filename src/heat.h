#ifndef heat_h
#define heat_h

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "gpx_types.h"
#include "heat_types.h"

bool calculate_heatmap(GpxCollection *collection);

#endif