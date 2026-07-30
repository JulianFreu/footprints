-include config.mk

BIN      := footprints
SRC_DIR  := src
OBJ_DIR  := build

# ui.c #includes clay_renderer_sdl.c, so that file is deliberately not listed
# here as a translation unit of its own.
SOURCES  := $(SRC_DIR)/main.c $(SRC_DIR)/map.c $(SRC_DIR)/fifo.c \
            $(SRC_DIR)/gpxParser.c $(SRC_DIR)/tracks.c $(SRC_DIR)/filters.c \
            $(SRC_DIR)/heat.c $(SRC_DIR)/ui.c \
            $(SRC_DIR)/time_util.c
OBJECTS  := $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
DEPS     := $(OBJECTS:.o=.d)

# libxml2 headers live in a versioned include dir, so the path has to be asked
# for rather than assumed.
XML_CFLAGS := $(shell xml2-config --cflags 2>/dev/null || pkg-config --cflags libxml-2.0)
XML_LIBS   := $(shell xml2-config --libs   2>/dev/null || pkg-config --libs   libxml-2.0)

# -Wmissing-braces fires ~44 times from inside clay.h's CLAY__INIT macro, which
# is vendored code we do not edit. Suppressed so real warnings stay visible.
WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces
OPT      ?= -O2

CFLAGS   ?= $(OPT) $(WARNINGS) $(XML_CFLAGS) -MMD -MP
LDLIBS   := -lSDL2 -lSDL2_image -lSDL2_ttf -lcurl $(XML_LIBS) -lm -lpthread

# Files that are vendored third-party code and must not be reformatted.
FORMAT_FILES := $(filter-out $(SRC_DIR)/clay.h $(SRC_DIR)/clay_renderer_sdl.c, \
                  $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*.h))

.PHONY: all debug clean format check-format run

all: $(BIN)

# Unoptimised build with the address and undefined-behaviour sanitizers. This is
# what to run when touching memory ownership or threading.
debug:
	$(MAKE) OPT="-O0 -g -fsanitize=address,undefined" \
	        LDFLAGS="-fsanitize=address,undefined" \
	        OBJ_DIR=build-debug BIN=$(BIN)-debug

$(BIN): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

$(SRC_DIR)/api_key.h:
	@echo "error: $(SRC_DIR)/api_key.h is missing."
	@echo "       cp $(SRC_DIR)/api_key.h.example $(SRC_DIR)/api_key.h"
	@echo "       then paste your Stadia Maps key into it (only needed for -stadiamaps)."
	@false

$(OBJ_DIR)/map.o: $(SRC_DIR)/api_key.h

format:
	clang-format -i $(FORMAT_FILES)

check-format:
	clang-format --dry-run --Werror $(FORMAT_FILES)

run: $(BIN)
	./$(BIN)

clean:
	$(RM) -r $(OBJ_DIR) build-debug $(BIN) $(BIN)-debug

-include $(DEPS)
