#!/bin/bash

gcc -O3 src/main.c src/map.c src/fifo.c src/gpxParser.c src/tracks.c src/filters.c src/heat.c src/ui.c -o footprints -lSDL2 -lSDL2_image -lSDL2_ttf -lcurl -lm -lxml2