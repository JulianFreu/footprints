#include "map.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> // mkdir + stat
#include <unistd.h>

#include <SDL2/SDL_image.h>

#include "fifo.h"
#include "log.h"

// The Stadia Maps key is optional: it is only read when -stadiamaps is passed,
// and the default OpenStreetMap tiles need no key at all. Including the header
// unconditionally meant the build failed without it even for users who would
// never use it.
#if defined(__has_include)
#if __has_include("api_key.h")
#include "api_key.h"
#define HAVE_API_KEY 1
#endif
#endif

#ifndef HAVE_API_KEY
static const char *api_key = NULL;
#endif

bool use_osm_tiles = true;
_Atomic bool download_in_progress;

// Tiles asked for and not yet in hand: queued, in flight, or lately failed and
// cooling off.
//
// Without this a tile the provider will not give us -- a 404 above the
// provider's coverage, or anything at all while the network is down -- was
// re-queued on every frame for as long as it stayed on screen, because a
// failed download is deliberately not written to disk and so never stops
// looking missing. That is a request per tile per frame, indefinitely. It also
// meant download_in_progress never went false, so the window redrew at the
// frame rate forever and could not idle.
typedef struct PendingTile {
    MapTile tile;
    Uint32 retry_at_ms; // when it may be asked for again
} PendingTile;

static PendingTile pending[TILE_PENDING_MAX];
static int pending_count;

// Pushed by the download worker when a tile lands, so the main thread stops
// waiting on it. Registered on the main thread before the worker starts.
static Uint32 tile_ready_event = (Uint32)-1;

void map_init_events(void) {
    Uint32 type = SDL_RegisterEvents(1);
    if (type != (Uint32)-1)
        tile_ready_event = type;
}

static int pending_find(MapTile tile) {
    for (int i = 0; i < pending_count; i++)
        if (tile_key_equal(pending[i].tile, tile))
            return i;
    return -1;
}

static void pending_remove_at(int index) {
    pending[index] = pending[pending_count - 1];
    pending_count--;
}

// Whether this tile should be left alone this frame.
static bool pending_blocks(MapTile tile, Uint32 now_ms) {
    int index = pending_find(tile);
    if (index < 0)
        return false;

    if (now_ms >= pending[index].retry_at_ms) {
        pending_remove_at(index); // cooled off; let it be asked for again
        return false;
    }
    return true;
}

static void pending_add(MapTile tile, Uint32 now_ms) {
    if (pending_count >= TILE_PENDING_MAX) {
        // Full: drop whatever has been waiting longest rather than refuse to
        // record this one, so the set cannot wedge.
        int oldest = 0;
        for (int i = 1; i < pending_count; i++)
            if (pending[i].retry_at_ms < pending[oldest].retry_at_ms)
                oldest = i;
        pending_remove_at(oldest);
    }
    pending[pending_count++] = (PendingTile){
        .tile = tile, .retry_at_ms = now_ms + TILE_RETRY_SECONDS * 1000};
}

void map_handle_event(const SDL_Event *event) {
    if (tile_ready_event == (Uint32)-1 || event->type != tile_ready_event)
        return;

    MapTile tile = {.tile_x = (int)(intptr_t)event->user.data1,
                    .tile_y = (int)(intptr_t)event->user.data2,
                    .zoom = event->user.code};
    int index = pending_find(tile);
    if (index >= 0)
        pending_remove_at(index); // the file is there now; pick it up next frame
}

// Called on the download thread.
static void publish_tile_ready(MapTile tile) {
    if (tile_ready_event == (Uint32)-1)
        return;

    SDL_Event event = {0};
    event.type = tile_ready_event;
    event.user.code = tile.zoom;
    event.user.data1 = (void *)(intptr_t)tile.tile_x;
    event.user.data2 = (void *)(intptr_t)tile.tile_y;
    SDL_PushEvent(&event); // thread-safe
}

bool map_has_api_key(void) {
    return api_key != NULL && api_key[0] != '\0';
}

void conv_pixel_to_tile_and_offset(int pixel_x, int pixel_y, int source_zoom, int target_zoom,
                                   int *tile_x, int *tile_y,
                                   int *pixel_in_tile_x, int *pixel_in_tile_y) {
    int zoom_diff = source_zoom - target_zoom;

    int scaled_x = pixel_x >> zoom_diff;
    int scaled_y = pixel_y >> zoom_diff;

    *tile_x = scaled_x / TILE_SIZE;
    *tile_y = scaled_y / TILE_SIZE;

    *pixel_in_tile_x = scaled_x % TILE_SIZE;
    *pixel_in_tile_y = scaled_y % TILE_SIZE;

    // C's % takes the sign of the dividend, so a world coordinate left of the
    // origin lands on a negative offset into its tile.
    if (*pixel_in_tile_x < 0)
        *pixel_in_tile_x += TILE_SIZE;
    if (*pixel_in_tile_y < 0)
        *pixel_in_tile_y += TILE_SIZE;
}

int map_world_per_pixel(const struct application *appl) {
    return map_world_per_pixel_at(appl->zoom);
}

// Both directions work in double and take the window centre as an integer.
// A float cannot hold the product: a world span of 2^28 against a 24-bit
// mantissa loses whole pixels at the low zooms, which showed up as clicks
// selecting the wrong track. The integer centre is what the shifts did -- an
// odd window is half a screen pixel off centre, and at zoom 5 half a screen
// pixel is sixteen thousand world pixels.
void map_world_to_screen(const struct application *appl, int world_x, int world_y,
                         float *screen_x, float *screen_y) {
    const double per_pixel = (double)map_world_per_pixel(appl);
    *screen_x = (float)((double)(world_x - appl->world_x) / per_pixel + appl->window_width / 2);
    *screen_y = (float)((double)(world_y - appl->world_y) / per_pixel + appl->window_height / 2);
}

void map_screen_to_world(const struct application *appl, float screen_x, float screen_y,
                         int *world_x, int *world_y) {
    const double per_pixel = (double)map_world_per_pixel(appl);
    *world_x = appl->world_x + (int)(((double)screen_x - appl->window_width / 2) * per_pixel);
    *world_y = appl->world_y + (int)(((double)screen_y - appl->window_height / 2) * per_pixel);
}

static void ensure_directory(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        mkdir(path, 0755);
    }
}

// Appends one chunk of a curl transfer to the growable buffer behind `userp`.
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *new_memory = (char *)realloc(mem->memory, mem->size + realsize + 1); // +1 for null termination
    if (new_memory == NULL)
        return 0; // realloc failed

    mem->memory = new_memory;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';

    return realsize;
}

void *download_tiles(void *arg) {
    struct fifo *download_queue = (struct fifo *)arg;
    MapTile next_tile = {0};

    // One handle for the lifetime of the thread. A fresh easy handle per tile
    // threw away the connection and its TLS session, so every tile paid for a
    // new handshake against the same host.
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize CURL\n");
        curl_global_cleanup();
        return NULL;
    }

    while (true) {
        download_in_progress = false;
        pthread_mutex_lock(&download_queue->lock);
        while (fifo_is_empty(download_queue) && !download_queue->stop)
            pthread_cond_wait(&download_queue->cond, &download_queue->lock);

        if (download_queue->stop) {
            pthread_mutex_unlock(&download_queue->lock);
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            return NULL;
        }

        download_in_progress = true;
        if (!fifo_read_data(download_queue, &next_tile))
            fprintf(stderr, "Something went wrong while reading from download queue\n");

        // Publish what we are about to fetch, still under the lock. The tile is
        // no longer in the queue but its file does not exist yet, so without
        // this the main loop cannot tell it is already being fetched and
        // re-queues it on every frame until the download lands.
        download_queue->tile_in_dl = next_tile;

        pthread_mutex_unlock(&download_queue->lock);

        char tile_path[TILE_PATH_MAX];
        tile_cache_path(tile_path, sizeof(tile_path), next_tile);

        LOG_DEBUG("Start download for: %s\n", tile_path);
        char zoom_dir[TILE_PATH_MAX], x_dir[TILE_PATH_MAX];
        snprintf(zoom_dir, sizeof(zoom_dir), "%s/%d", TILE_CACHE_DIR, next_tile.zoom);
        snprintf(x_dir, sizeof(x_dir), "%s/%d/%d", TILE_CACHE_DIR, next_tile.zoom, next_tile.tile_x);
        ensure_directory(TILE_CACHE_DIR);
        ensure_directory(zoom_dir);
        ensure_directory(x_dir);

        struct MemoryStruct image_data;
        char url[TILE_PATH_MAX];
        struct curl_slist *list = NULL;

        image_data.memory = (char *)malloc(1);
        image_data.size = 0;

        // Options set on the previous tile do not carry over to this one.
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &image_data);

        if (use_osm_tiles) {
            snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png",
                     next_tile.zoom, next_tile.tile_x, next_tile.tile_y);
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, TILE_USER_AGENT);
        } else // use stadiamaps; requires api key
        {
            snprintf(url, sizeof(url), "https://tiles.stadiamaps.com/tiles/stamen_terrain/%d/%d/%d.png",
                     next_tile.zoom, next_tile.tile_x, next_tile.tile_y);
            char auth[TILE_PATH_MAX];
            // main() refuses -stadiamaps without a key, so this cannot be
            // reached with a null one -- but the compiler cannot see that, and
            // a null here would be a format-string crash rather than a
            // failed download.
            snprintf(auth, sizeof(auth), "Authorization: Stadia-Auth %s",
                     map_has_api_key() ? api_key : "");
            list = curl_slist_append(list, auth);

            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        }

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(list);

        // Only cache the tile when the transfer actually succeeded. Writing a
        // failed or empty response would make file_exists() report the tile as
        // present, so it would render blank forever and never be retried.
        if (res != CURLE_OK) {
            fprintf(stderr, "Tile download failed (%s): %s\n", tile_path, curl_easy_strerror(res));
        } else if (image_data.size == 0) {
            fprintf(stderr, "Tile download returned no data: %s\n", tile_path);
        } else {
            FILE *f = fopen(tile_path, "wb");
            if (f) {
                fwrite(image_data.memory, 1, image_data.size, f);
                fclose(f);
                // Tell the main thread rather than leaving it to notice: it
                // stops looking at a tile once it has asked for it, so nothing
                // else would go back for this one until its retry came round.
                publish_tile_ready(next_tile);
            } else {
                fprintf(stderr, "Could not write tile to cache: %s\n", tile_path);
            }
        }
        free(image_data.memory);

        // Clear the marker: either the file now exists, or the transfer failed
        // and the tile should be eligible for queueing again.
        pthread_mutex_lock(&download_queue->lock);
        download_queue->tile_in_dl = (MapTile){.tile_x = -1, .tile_y = -1, .zoom = -1};
        pthread_mutex_unlock(&download_queue->lock);
    }
}

// Asks the download worker to return from its wait. The caller joins the
// thread afterwards; the queue's mutex and condvar must outlive that join.
void download_thread_stop(struct fifo *download_queue) {
    pthread_mutex_lock(&download_queue->lock);
    download_queue->stop = true;
    pthread_cond_signal(&download_queue->cond);
    pthread_mutex_unlock(&download_queue->lock);
}

bool tile_key_equal(MapTile a, MapTile b) {
    return a.tile_x == b.tile_x && a.tile_y == b.tile_y && a.zoom == b.zoom;
}

// Hands back the entry for `key`, or NULL. Touching it marks it as the most
// recently used, which is what keeps eviction honest.
TileTexture *tile_cache_find(TileTextureCache *cache, MapTile key) {
    for (int i = 0; i < cache->size; i++) {
        if (tile_key_equal(cache->entries[i].key, key)) {
            cache->entries[i].last_used = ++cache->clock;
            return &cache->entries[i];
        }
    }
    return NULL;
}

// The same question asked by a caller that only wants to draw the tile as it
// is, with no interest in how far along its fade is.
SDL_Texture *tile_cache_lookup(TileTextureCache *cache, MapTile key) {
    TileTexture *entry = tile_cache_find(cache, key);
    return entry ? entry->texture : NULL;
}

// Drops the least recently used entry. Called only when the cache is full, so
// the linear scan costs nothing next to the texture upload it makes room for.
static void evict_least_recently_used(TileTextureCache *cache) {
    int oldest = 0;
    for (int i = 1; i < cache->size; i++)
        if (cache->entries[i].last_used < cache->entries[oldest].last_used)
            oldest = i;

    SDL_DestroyTexture(cache->entries[oldest].texture);
    cache->entries[oldest] = cache->entries[cache->size - 1];
    cache->size--;
}

// Takes ownership of `texture` on success and hands back the entry it went
// into, or NULL if it could not be stored -- the caller destroys it then. The
// pointer is good only until the next insert: growing reallocs the array and
// eviction moves the last entry into the hole.
TileTexture *tile_cache_insert(TileTextureCache *cache, MapTile key, SDL_Texture *texture) {
    // Each entry is a TILE_SIZE-square RGBA texture -- a quarter of a megabyte
    // of video memory. Growing without a bound meant a long panning session
    // consumed as much of it as the session was long.
    if (cache->size >= TILE_CACHE_MAX_ENTRIES)
        evict_least_recently_used(cache);

    if (cache->size >= cache->capacity) {
        int grown_capacity = (cache->capacity == 0) ? 64 : cache->capacity * 2;
        if (grown_capacity > TILE_CACHE_MAX_ENTRIES)
            grown_capacity = TILE_CACHE_MAX_ENTRIES;
        TileTexture *grown = realloc(cache->entries, (size_t)grown_capacity * sizeof(TileTexture));
        if (!grown) {
            // The old array is still valid and still owned by the cache, so a
            // failure here leaves the cache exactly as it was.
            fprintf(stderr, "Could not grow tile cache\n");
            return NULL;
        }
        cache->entries = grown;
        cache->capacity = grown_capacity;
    }

    TileTexture *entry = &cache->entries[cache->size++];
    *entry = (TileTexture){.key = key, .texture = texture, .last_used = ++cache->clock};
    // Fully shown unless the caller says otherwise. The designated initialiser
    // above zeroes the fade, and a zeroed Anim reads as an invisible tile.
    anim_set(&entry->fade, 1.0f);
    return entry;
}

// Advances every entry's fade and reports whether any of them moved, which is
// what asks for the next frame. Bounded by TILE_CACHE_MAX_ENTRIES, so the walk
// costs nothing next to the drawing it is keeping up with.
bool tile_cache_tick_fades(TileTextureCache *cache, float dt) {
    bool moved = false;
    for (int i = 0; i < cache->size; i++)
        moved |= anim_tick(&cache->entries[i].fade, dt);
    return moved;
}

void tile_cache_free(TileTextureCache *cache) {
    for (int i = 0; i < cache->size; i++) {
        if (cache->entries[i].texture)
            SDL_DestroyTexture(cache->entries[i].texture);
    }
    free(cache->entries);
    cache->entries = NULL;
    cache->size = 0;
    cache->capacity = 0;
    cache->clock = 0;
}

static int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

// The one place the on-disk tile layout is spelled out. The download thread and
// the render loop have to agree on it exactly, or tiles are fetched forever and
// never found.
void tile_cache_path(char *out, size_t size, MapTile tile) {
    snprintf(out, size, "%s/%d/%d/%d.png", TILE_CACHE_DIR, tile.zoom, tile.tile_x, tile.tile_y);
}

// Decodes a tile PNG off disk and hands it to the cache, which takes ownership.
//
// Returns the cache entry rather than the texture because the entry is what
// carries the fade, and because looking it up again afterwards would mean
// holding a pointer the insert had just invalidated.
static TileTexture *load_tile_texture(struct application *appl, MapTile key, const char *path) {
    SDL_Surface *surface = IMG_Load(path);
    if (!surface)
        return NULL;

    SDL_Texture *texture = SDL_CreateTextureFromSurface(appl->renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture)
        return NULL;

    // Set rather than inherited. What SDL_CreateTextureFromSurface leaves here
    // depends on whether the decoded PNG had alpha, and a tile drawn with an
    // alpha modulation needs it either way.
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    TileTexture *entry = tile_cache_insert(&appl->tile_cache, key, texture);
    if (!entry) {
        SDL_DestroyTexture(texture);
        return NULL;
    }

    // Up from nothing rather than straight to full: the flat colour standing
    // in for this tile is still drawn underneath while the fade runs, so what
    // the window shows is a cross-fade rather than a cut.
    anim_set(&entry->fade, 0.0f);
    anim_to(&entry->fade, 1.0f, TILE_FADE_SECONDS, ANIM_EASE_OUT);
    return entry;
}

SDL_FRect map_transform_rect(const struct application *appl,
                             float x, float y, float w, float h) {
    const MapTransform *t = &appl->map_transform;
    return (SDL_FRect){
        .x = t->anchor_x + (x - t->anchor_x) * t->scale,
        .y = t->anchor_y + (y - t->anchor_y) * t->scale,
        .w = w * t->scale,
        .h = h * t->scale};
}

void map_screen_untransform(const struct application *appl, float screen_x, float screen_y,
                            float *unscaled_x, float *unscaled_y) {
    const MapTransform *t = &appl->map_transform;
    *unscaled_x = t->anchor_x + (screen_x - t->anchor_x) / t->scale;
    *unscaled_y = t->anchor_y + (screen_y - t->anchor_y) / t->scale;
}

// The transform is only ever written from the transition, so the two cannot
// drift apart. Both the event that starts a zoom and the tick that advances it
// come through here, which is why neither has to run before the other.
static void map_adopt_transform(struct application *appl) {
    appl->map_transform = (MapTransform){
        .scale = zoom_scale(&appl->zoom_transition),
        .anchor_x = appl->zoom_transition.anchor_x,
        .anchor_y = appl->zoom_transition.anchor_y};
}

void map_zoom_by_wheel(struct application *appl, int steps) {
    if (zoom_step(&appl->zoom_transition, &appl->zoom, &appl->world_x, &appl->world_y, steps,
                  (float)appl->mouse_x, (float)appl->mouse_y,
                  appl->window_width, appl->window_height) == 0)
        return;

    map_adopt_transform(appl);
}

void map_update(struct application *appl, float dt) {
    if (zoom_tick(&appl->zoom_transition, dt)) {
        map_adopt_transform(appl);
        app_request_redraw(appl);
    }

    // Asked separately rather than behind the zoom: tiles arrive and fade in
    // while nothing is zooming at all.
    if (tile_cache_tick_fades(&appl->tile_cache, dt))
        app_request_redraw(appl);
}

// The tiles covering the window at the current zoom, with where each one lands
// on screen. Returned rather than drawn, so the caller decides what is layered
// over what -- this module used to reach into the track renderer to composite
// the heat overlay itself, which put the layer order in the wrong place and
// made the map depend on the tracks.
int map_visible_tiles(const struct application *appl, VisibleTile *out, int max_tiles) {
    // Scaled down, a tile covers less of the window and more of them are
    // needed to fill it.
    const float scale = appl->map_transform.scale;
    const float scaled_tile = (float)TILE_SIZE * scale;
    int tiles_x = (int)((float)appl->window_width / scaled_tile) + 2;
    int tiles_y = (int)((float)appl->window_height / scaled_tile) + 2;

    int center_tile_x, center_tile_y;
    int tile_offset_x, tile_offset_y;
    conv_pixel_to_tile_and_offset(appl->world_x, appl->world_y, MAX_ZOOM, appl->zoom,
                                  &center_tile_x, &center_tile_y,
                                  &tile_offset_x, &tile_offset_y);

    int tiles_at_zoom = 1 << appl->zoom;
    int count = 0;

    for (int dx = -tiles_x / 2; dx <= tiles_x / 2; dx++) {
        for (int dy = -tiles_y / 2; dy <= tiles_y / 2; dy++) {
            if (count >= max_tiles)
                return count;

            int tile_x = center_tile_x + dx;
            int tile_y = center_tile_y + dy;

            // Off the edge of the world at this zoom.
            if (tile_x < 0 || tile_y < 0 || tile_x >= tiles_at_zoom || tile_y >= tiles_at_zoom)
                continue;

            // Both are linear in the tile index, so after the transform two
            // neighbours still share an edge exactly and the grid has no
            // seams to show.
            float unscaled_x = (float)((tile_x - center_tile_x) * TILE_SIZE - tile_offset_x +
                                       appl->window_width / 2);
            float unscaled_y = (float)((tile_y - center_tile_y) * TILE_SIZE - tile_offset_y +
                                       appl->window_height / 2);
            SDL_FRect placed = map_transform_rect(appl, unscaled_x, unscaled_y,
                                                  (float)TILE_SIZE, (float)TILE_SIZE);

            out[count++] = (VisibleTile){
                .tile = {tile_x, tile_y, appl->zoom},
                .screen_x = placed.x,
                .screen_y = placed.y,
                .size = placed.w};
        }
    }
    return count;
}

// Queues a tile for download unless it is already queued or in flight. Reports
// whether the tile is now somebody's problem, which is what decides if it goes
// into the pending set: a tile that could not be enqueued must stay askable, or
// nothing would ever go back for it.
static bool queue_tile_download(struct application *appl, MapTile tile) {
    pthread_mutex_lock(&appl->download_queue.lock);

    bool already_queued = fifo_search_data(&appl->download_queue, tile);
    bool already_downloading = tile_key_equal(appl->download_queue.tile_in_dl, tile);
    bool accepted = already_queued || already_downloading ||
                    fifo_write_data(&appl->download_queue, tile);

    pthread_mutex_unlock(&appl->download_queue.lock);
    return accepted;
}

// Fills the area a missing tile would have covered with a flat colour.
//
// Something has to be under a tile that is not there: without this, panning
// into new ground leaves holes showing whatever the frame was cleared to until
// the download lands.
static void draw_fallback_tile(struct application *appl, const VisibleTile *visible) {
    SDL_FRect dest = {visible->screen_x, visible->screen_y, visible->size, visible->size};
    SDL_SetRenderDrawColor(appl->renderer, TILE_FALLBACK_COLOR_R, TILE_FALLBACK_COLOR_G,
                           TILE_FALLBACK_COLOR_B, 255);
    SDL_RenderFillRectF(appl->renderer, &dest);
}

void map_draw_tiles(struct application *appl, const VisibleTile *tiles, int count) {
    const Uint32 now_ms = SDL_GetTicks();
    int decodes_left = TILE_DECODES_PER_FRAME;

    for (int i = 0; i < count; i++) {
        MapTile key = tiles[i].tile;

        // Only a tile that is not already in video memory needs the filesystem
        // consulted at all.
        TileTexture *entry = tile_cache_find(&appl->tile_cache, key);
        if (!entry && !pending_blocks(key, now_ms)) {
            char tile_path[TILE_PATH_MAX];
            tile_cache_path(tile_path, sizeof(tile_path), key);

            if (file_exists(tile_path)) {
                // The decode and upload are synchronous, so they are rationed:
                // a pan that uncovers forty cached tiles at once would spend
                // the whole frame on them. The rest keep the flat colour for a
                // frame or two, which is only tolerable because it is there.
                if (decodes_left > 0) {
                    entry = load_tile_texture(appl, key, tile_path);
                    decodes_left--;
                } else {
                    app_request_redraw(appl); // come back for the rest
                }
            } else if (queue_tile_download(appl, key)) {
                pending_add(key, now_ms);
            }
        }

        const float opacity = entry ? anim_value(&entry->fade) : 0.0f;

        // The stand-in stays underneath until the tile is all the way up, so
        // what a fade is drawn over is the flat colour rather than whatever
        // the frame was cleared to.
        if (opacity < 1.0f)
            draw_fallback_tile(appl, &tiles[i]);

        if (entry) {
            SDL_FRect dest = {tiles[i].screen_x, tiles[i].screen_y,
                              tiles[i].size, tiles[i].size};
            SDL_SetTextureAlphaMod(entry->texture, (Uint8)(opacity * 255.0f));
            SDL_RenderCopyF(appl->renderer, entry->texture, NULL, &dest);
            // Put back rather than left: the modulation belongs to the texture
            // and outlives the draw, so anything that blits this texture
            // without setting it would inherit whatever a fade left behind.
            SDL_SetTextureAlphaMod(entry->texture, 255);
        }
    }
}
