#ifndef heat_h
#define heat_h

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "structs.h"

bool calculate_heatmap(GpxCollection *collection);

#endif