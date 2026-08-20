-include config.mk

BIN      := footprints
SRC_DIR  := src
OBJ_DIR  := build
TEST_DIR := tests
TEST_BIN := run-tests
DIST_DIR := dist

# What the packaged build is called, and what the tile server is told we are.
VERSION  := $(shell sed -n 's/^#define FOOTPRINTS_VERSION "\(.*\)"/\1/p' $(SRC_DIR)/config.h)

# Windows builds go through MSYS2's mingw-w64 toolchain, which gives pthreads,
# an xml2-config and -lSDL2-style linking, so almost nothing below has to know
# which platform it is on. The exceptions are gathered here.
ifeq ($(OS),Windows_NT)
    HOST_WINDOWS := 1
endif

ifdef HOST_WINDOWS
    BIN_SUFFIX := .exe
    # No console window behind the application. Errors that used to go to a
    # terminal are shown in a message box instead; see main.c.
    PLATFORM_LDFLAGS := -mwindows
    # mingw-w64's gcc has no AddressSanitizer or UBSan, so the suites build
    # without them there. Linux is where the memory coverage actually runs.
    SAN ?=
    DIST_ARCHIVE := $(DIST_DIR)/footprints-$(VERSION)-windows-x86_64.zip
else
    BIN_SUFFIX :=
    PLATFORM_LDFLAGS :=
    SAN ?= -fsanitize=address,undefined
    DIST_ARCHIVE := $(DIST_DIR)/footprints-$(VERSION)-linux-x86_64.tar.gz
endif

EXE      := $(BIN)$(BIN_SUFFIX)

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

# SDL and curl are asked for the same way, because MSYS2 and the Debian and
# Fedora packages do not agree on where their headers sit or what the libraries
# alongside them are called. The literals are what this used to hardcode, and
# are still right on a distribution with no .pc files installed.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf libcurl 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs   sdl2 SDL2_image SDL2_ttf libcurl 2>/dev/null \
                       || echo "-lSDL2 -lSDL2_image -lSDL2_ttf -lcurl")

# -Wmissing-braces fires from inside clay.h's CLAY__INIT macro, which every
# CLAY() in the UI expands. Vendored code we do not edit, so it is suppressed
# here to keep real warnings visible.
WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces
OPT      ?= -O2

CFLAGS   ?= $(OPT) $(WARNINGS) $(SDL_CFLAGS) $(XML_CFLAGS) -MMD -MP
LDLIBS   := $(SDL_LIBS) $(XML_LIBS) -lm -lpthread

# Files that are vendored third-party code and must not be reformatted.
FORMAT_FILES := $(filter-out $(SRC_DIR)/clay.h $(SRC_DIR)/clay_renderer_sdl.c, \
                  $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*.h)) \
                $(wildcard $(TEST_DIR)/*.c) $(wildcard $(TEST_DIR)/*.h)

# Each test file #includes the module it covers, so the sources under test are
# not listed here -- they arrive through those includes and must not also be
# linked in, or every symbol would be defined twice.
TEST_SOURCES := $(wildcard $(TEST_DIR)/*.c)

# What a packaged build contains besides the binary. The resources are found
# beside the executable and the helper scripts are run from there, so staging is
# a copy rather than an install.
DIST_FILES := resources garmin_sync.py strava_sync.py convert_fit_to_gpx.py \
              requirements.txt README.md LICENSE

.PHONY: all debug clean format check-format run test dist version

all: $(EXE)

# Unoptimised build with the address and undefined-behaviour sanitizers. This is
# what to run when touching memory ownership or threading.
debug:
	$(MAKE) OPT="-O0 -g $(SAN)" \
	        LDFLAGS="$(SAN)" \
	        OBJ_DIR=build-debug BIN=$(BIN)-debug

$(EXE): $(OBJECTS)
	$(CC) $(LDFLAGS) $(PLATFORM_LDFLAGS) $^ -o $@ $(LDLIBS)

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

run: $(EXE)
	./$(EXE)

version:
	@echo $(VERSION)

# Built and run with the sanitizers on where the toolchain has them: these are
# the suites that cover the hand-rolled parsing and buffer arithmetic, which is
# exactly where an out-of-bounds read would hide.
test: $(TEST_SOURCES) $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*.h)
	$(CC) -O1 -g $(SAN) $(WARNINGS) $(XML_CFLAGS) \
	    $(TEST_SOURCES) -o $(TEST_BIN) $(XML_LIBS) -lm -lpthread
	./$(TEST_BIN)

# A tree that runs from wherever it is unpacked. The binary finds its resources
# beside itself and writes nothing into the folder, so this is the whole of what
# a package needs -- plus, on Windows, the libraries the exe was linked against,
# which are not on a machine that has never had MSYS2 installed.
dist: $(EXE)
	@rm -rf $(DIST_DIR)/$(BIN)
	@mkdir -p $(DIST_DIR)/$(BIN)
	cp $(EXE) $(DIST_DIR)/$(BIN)/
	cp -r $(DIST_FILES) $(DIST_DIR)/$(BIN)/
ifdef HOST_WINDOWS
	@echo "collecting the DLLs $(EXE) was linked against"
	@ldd $(EXE) | awk '/=> \/(mingw|ucrt)[0-9]*\//{print $$3}' \
	    | xargs -r -I{} cp -u {} $(DIST_DIR)/$(BIN)/
	cd $(DIST_DIR) && zip -qr $(notdir $(DIST_ARCHIVE)) $(BIN)
else
	tar czf $(DIST_ARCHIVE) -C $(DIST_DIR) $(BIN)
endif
	@echo "packaged $(DIST_ARCHIVE)"

clean:
	$(RM) -r $(OBJ_DIR) build-debug $(BIN) $(BIN)-debug $(BIN).exe \
	         $(BIN)-debug.exe $(TEST_BIN) $(TEST_BIN).exe $(DIST_DIR)

-include $(DEPS)
