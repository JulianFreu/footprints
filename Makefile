-include config.mk

BIN      := footprints
SRC_DIR  := src
OBJ_DIR  := build
TEST_DIR := tests
TEST_BIN := run-tests

# clay_sdl.c is the one translation unit carrying Clay's implementation and the
# vendored SDL2 renderer; clay.h and clay_renderer_sdl.c are pulled in there and
# are not translation units of their own.
SOURCES  := $(SRC_DIR)/main.c $(SRC_DIR)/map.c $(SRC_DIR)/fifo.c \
            $(SRC_DIR)/gpx_parser.c $(SRC_DIR)/gpx_activity.c \
            $(SRC_DIR)/tracks.c $(SRC_DIR)/filters.c \
            $(SRC_DIR)/heat.c $(SRC_DIR)/ui.c \
            $(SRC_DIR)/point_index.c $(SRC_DIR)/track_sort.c $(SRC_DIR)/track_format.c \
            $(SRC_DIR)/track_splits.c $(SRC_DIR)/track_series.c \
            $(SRC_DIR)/background.c $(SRC_DIR)/anim.c $(SRC_DIR)/zoom.c \
            $(SRC_DIR)/clay_sdl.c $(SRC_DIR)/ui_filters.c \
            $(SRC_DIR)/ui_runlist.c $(SRC_DIR)/ui_settings.c \
            $(SRC_DIR)/import_job.c $(SRC_DIR)/ui_import.c \
            $(SRC_DIR)/ui_stats.c $(SRC_DIR)/stats.c \
            $(SRC_DIR)/ui_records.c $(SRC_DIR)/records.c \
            $(SRC_DIR)/time_util.c $(SRC_DIR)/settings.c \
            $(SRC_DIR)/platform.c $(SRC_DIR)/paths.c $(SRC_DIR)/subprocess.c \
            $(SRC_DIR)/render_cache.c \
            $(SRC_DIR)/profiler.c $(SRC_DIR)/ui_profiler.c
OBJECTS  := $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
DEPS     := $(OBJECTS:.o=.d)

# libxml2 headers live in a versioned include dir, so the path has to be asked
# for rather than assumed.
XML_CFLAGS := $(shell xml2-config --cflags 2>/dev/null || pkg-config --cflags libxml-2.0)
XML_LIBS   := $(shell xml2-config --libs   2>/dev/null || pkg-config --libs   libxml-2.0)

# -Wmissing-braces fires from inside clay.h's CLAY__INIT macro, which every
# CLAY() in the UI expands. Vendored code we do not edit, so it is suppressed
# here to keep real warnings visible.
WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces
OPT      ?= -O2

CFLAGS   ?= $(OPT) $(WARNINGS) $(XML_CFLAGS) -MMD -MP
LDLIBS   := -lSDL2 -lSDL2_image -lSDL2_ttf -lcurl $(XML_LIBS) -lm -lpthread

# Files that are vendored third-party code and must not be reformatted.
FORMAT_FILES := $(filter-out $(SRC_DIR)/clay.h $(SRC_DIR)/clay_renderer_sdl.c, \
                  $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*.h)) \
                $(wildcard $(TEST_DIR)/*.c) $(wildcard $(TEST_DIR)/*.h)

# Each test file #includes the module it covers, so the sources under test are
# not listed here -- they arrive through those includes and must not also be
# linked in, or every symbol would be defined twice.
TEST_SOURCES := $(wildcard $(TEST_DIR)/*.c)

.PHONY: all debug clean format check-format run test

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

# The vendored renderer warns about its own sign comparisons and precedence.
# Now that it compiles on its own rather than inside ui.c, those warnings can be
# silenced here instead of being scrolled past on every build.
$(OBJ_DIR)/clay_sdl.o: WARNINGS := -w

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

format:
	clang-format -i $(FORMAT_FILES)

check-format:
	clang-format --dry-run --Werror $(FORMAT_FILES)

run: $(BIN)
	./$(BIN)

# Built and run with the sanitizers on: these are the suites that cover the
# hand-rolled parsing and buffer arithmetic, which is exactly where an
# out-of-bounds read would hide.
test: $(TEST_SOURCES) $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*.h)
	$(CC) -O1 -g -fsanitize=address,undefined $(WARNINGS) $(XML_CFLAGS) \
	    $(TEST_SOURCES) -o $(TEST_BIN) $(XML_LIBS) -lm -lpthread
	./$(TEST_BIN)

clean:
	$(RM) -r $(OBJ_DIR) build-debug $(BIN) $(BIN)-debug $(TEST_BIN)

-include $(DEPS)
