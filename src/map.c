#include "map.h"
#include "api_key.h"

void conv_pixel_to_tile_and_offset(int pixel_x, int pixel_y, int source_zoom, int target_zoom,
                          int *tile_x, int *tile_y,
                          int *pixel_in_tile_x, int *pixel_in_tile_y)
{
    int zoom_diff = source_zoom - target_zoom;

    // Convert world coordinates to target zoom level by shifting right
    int scaled_x = pixel_x >> zoom_diff;
    int scaled_y = pixel_y >> zoom_diff;

    *tile_x = scaled_x / TILE_SIZE;
    *tile_y = scaled_y / TILE_SIZE;

    *pixel_in_tile_x = scaled_x % TILE_SIZE;
    *pixel_in_tile_y = scaled_y % TILE_SIZE;

    // Ensure offsets are positive (handle negative input)
    if (*pixel_in_tile_x < 0)
        *pixel_in_tile_x += TILE_SIZE;
    if (*pixel_in_tile_y < 0)
        *pixel_in_tile_y += TILE_SIZE;
}

void ensure_directory(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) == -1)
    {
        mkdir(path, 0755);
    }
}

// Callback für curl
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *new_memory = (char *)realloc(mem->memory, mem->size + realsize + 1); // +1 für Nullterminierung
    if (new_memory == NULL)
        return 0; // realloc failed

    mem->memory = new_memory;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0'; // null-terminieren, optional

    return realsize;
}

void *download_tiles(void *arg)
{
    struct fifo *download_queue = (struct fifo *)arg;
    MapTile next_tile = {0};
    while (true)
    {
        pthread_mutex_lock(&download_queue->lock);
        // Wait while the queue is empty
        while (fifo_is_empty(download_queue))
        {
            pthread_cond_wait(&download_queue->cond, &download_queue->lock);
        }

        if (!fifo_read_data(download_queue, &next_tile))
        {
            fprintf(stderr, "Something went wrong while reading from download queue\n");
        }
        pthread_mutex_unlock(&download_queue->lock);

        char tile_path[256];
        snprintf(tile_path, sizeof(tile_path), "tilecache/%d/%d/%d.png", next_tile.zoom,
                 next_tile.tile_x, next_tile.tile_y);

        printf("Start download for: %s\n", tile_path);
        // Ordner sicherstellen
        char zoom_dir[64], x_dir[64];
        snprintf(zoom_dir, sizeof(zoom_dir), "tilecache/%d", next_tile.zoom);
        snprintf(x_dir, sizeof(x_dir), "tilecache/%d/%d", next_tile.zoom, next_tile.tile_x);
        ensure_directory("tilecache");
        ensure_directory(zoom_dir);
        ensure_directory(x_dir);

        // Download starten
        struct MemoryStruct image_data;
        char url[256];
        snprintf(url, sizeof(url), "https://tiles.stadiamaps.com/tiles/stamen_terrain/%d/%d/%d.png",
                 next_tile.zoom, next_tile.tile_x, next_tile.tile_y);

        CURL *curl = curl_easy_init();
        if (!curl)
        {
            fprintf(stderr, "Fehler beim Initialisieren von CURL\n");
            continue; // oder break oder return
        }

        image_data.memory = (char *)malloc(1);
        image_data.size = 0;

        curl_easy_setopt(curl, CURLOPT_URL, url);

        struct curl_slist *list = NULL;
        char auth[256];
        sprintf(auth, "Authorization: Stadia-Auth %s", api_key);
        list = curl_slist_append(list, auth);

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &image_data);

        // snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png",
        //          dl->zoom, dl->tile_x, dl->tile_y);
        //    curl_easy_setopt(curl, CURLOPT_URL, url);
        //    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        //    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &image_data);
        //    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OSM-Viewer/1.0");

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(list); /* free the list */

        if (res != CURLE_OK)
        {
            fprintf(stderr, "Fehler beim Laden: %s\n", curl_easy_strerror(res));
        }

        // Tile speichern
        FILE *f = fopen(tile_path, "wb");
        if (f)
        {
            fwrite(image_data.memory, 1, image_data.size, f);
            fclose(f);
        }
        free(image_data.memory);
    }
}

bool tile_key_equal(MapTile a, MapTile b)
{
    return a.tile_x == b.tile_x && a.tile_y == b.tile_y && a.zoom == b.zoom;
}

void append_to_tile_cache(TileTextureCache *cache, TileTexture entry)
{
    if (cache->size >= cache->capacity)
    {
        cache->capacity = cache->capacity == 0 ? 64 : cache->capacity * 2;
        cache->entries = realloc(cache->entries, cache->capacity * sizeof(TileTexture));
    }
    cache->entries[cache->size++] = entry;
}

void free_tile_cache(TileTextureCache *cache)
{
    printf("tile cache capacity before cleanup: %d\n", cache->capacity);
    for (int i = 0; i < cache->size; i++)
    {
        if (cache->entries[i].texture)
            SDL_DestroyTexture(cache->entries[i].texture);
    }
    free(cache->entries);
    cache->entries = NULL;
    cache->size = 0;
    cache->capacity = 0;
}

SDL_Texture *get_cached_texture(struct application *appl, MapTile key, const char *path)
{
    // Try to find the texture in the cache
    for (int i = 0; i < appl->tile_cache.size; i++)
    {
        if (tile_key_equal(appl->tile_cache.entries[i].key, key))
        {
            return appl->tile_cache.entries[i].texture;
        }
    }

    // Load it from disk
    SDL_Surface *surface = IMG_Load(path);
    if (!surface)
        return NULL;

    SDL_Texture *texture = SDL_CreateTextureFromSurface(appl->renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture)
        return NULL;

    // Store in cache
    TileTexture entry = {key, texture};
    append_to_tile_cache(&(appl->tile_cache), entry); // You implement this

    return texture;
}

int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

bool get_map_background(struct application *appl, GpxCollection *collection)
{
    // Wie viele Tiles brauchen wir?
    int tiles_x = appl->window_width / TILE_SIZE + 2;
    int tiles_y = appl->window_height / TILE_SIZE + 2;

    //  Berechne die aktuelle Tile-Koordinate im Zentrum

    int center_tile_x, center_tile_y;
    int tile_offset_x, tile_offset_y;

    conv_pixel_to_tile_and_offset(appl->world_x, appl->world_y, MAX_ZOOM, appl->zoom,
                         &center_tile_x, &center_tile_y,
                         &tile_offset_x, &tile_offset_y);

    for (int dx = -tiles_x / 2; dx <= tiles_x / 2; dx++)
    {
        for (int dy = -tiles_y / 2; dy <= tiles_y / 2; dy++)
        {
            int tile_x = center_tile_x + dx;
            int tile_y = center_tile_y + dy;
            //            int tile_x = tile_offset_x + dx;
            //            int tile_y = tile_offset_y + dy;

            if (tile_x < 0 || tile_y < 0 || tile_x >= (1 << appl->zoom) ||
                tile_y >= (1 << appl->zoom))
                continue;

            char tile_path[256];
            snprintf(tile_path, sizeof(tile_path), "tilecache/%d/%d/%d.png", appl->zoom,
                     tile_x, tile_y);

            if (!file_exists(tile_path))
            {
                // add tile to download queue if it is not in there already
                MapTile tile2queue = {
                    .tile_x = tile_x,
                    .tile_y = tile_y,
                    .zoom = appl->zoom,
                };
                if (!fifo_search_data(&(appl->download_queue), tile2queue))
                {
                    printf("adding to queue\n");
                    fifo_write_data(&(appl->download_queue), tile2queue);
                }
            }

            MapTile key = {tile_x, tile_y, appl->zoom};
            SDL_Texture *texture = get_cached_texture(appl, key, tile_path);
            int screen_x = (tile_x - center_tile_x) * TILE_SIZE - tile_offset_x + appl->window_width / 2;
            int screen_y = (tile_y - center_tile_y) * TILE_SIZE - tile_offset_y + appl->window_height / 2;

            if (texture)
            {
                SDL_Rect dest = {screen_x, screen_y, TILE_SIZE, TILE_SIZE};
                SDL_RenderCopy(appl->renderer, texture, NULL, &dest);
            }

            SDL_Texture *trackTex = get_or_render_track_tile(appl, collection, key);
            if (trackTex)
            {
                SDL_Rect dst = {
                    .x = screen_x,
                    .y = screen_y,
                    .w = 256,
                    .h = 256};
                SDL_RenderCopy(appl->renderer, trackTex, NULL, &dst);
            }
        }
    }
    if (appl->selected_track_overlay[appl->zoom])
    {
        SDL_RenderCopy(appl->renderer, appl->selected_track_overlay[appl->zoom], NULL, NULL);
    }
    return true;
}