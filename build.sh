#!/bin/bash
set -e

# libxml2 headers live under a versioned include dir (/usr/include/libxml2 on
# Debian/Ubuntu), so the include path has to come from xml2-config or pkg-config
# rather than being assumed to sit on the default search path.
XML_CFLAGS=$(xml2-config --cflags 2>/dev/null || pkg-config --cflags libxml-2.0)
XML_LIBS=$(xml2-config --libs 2>/dev/null || pkg-config --libs libxml-2.0)

gcc -O3 $XML_CFLAGS \
    src/main.c src/map.c src/fifo.c src/gpxParser.c src/tracks.c src/filters.c src/heat.c src/ui.c \
    -o footprints \
    -lSDL2 -lSDL2_image -lSDL2_ttf -lcurl $XML_LIBS -lm -lpthread
