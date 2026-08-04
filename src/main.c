#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "app.h"
#include "background.h"
#include "log.h"
#include "filters.h"
#include "gpx_parser.h"
#include "gpx_types.h"
#include "heat.h"
#include "map.h"
#include "settings.h"
#include "tracks.h"
#include "ui.h"

static bool sdl_initialize(struct application *appl);
static void appl_cleanup(struct application *appl, GpxCollection *collection);
static void dispatch_event(struct application *appl, GpxCollection *collection,
                           SDL_Event event);
static void handle_events(struct application *appl, GpxCollection *collection);

// Everything that happens between one frame and the next: input, and whatever
// is still moving. Each part asks for a redraw itself, so the loop below never
// has to enumerate what the program can animate.
static void app_update(struct application *appl, GpxCollection *collection) {
    // Neither is latched -- they are true for as long as the work lasts, so
    // they are asked rather than remembered.
    if (background_busy(&appl->background) || download_in_progress)
        app_request_redraw(appl);

    map_update(appl, appl->delta_time);
    ui_update(appl, collection);
}

int main(int argc, char *argv[]) {
    // Before anything reads a tunable. A missing file is the first run and
    // leaves the compiled-in defaults standing, so this cannot fail in a way
    // worth stopping for.
    settings_load(&settings, SETTINGS_FILE);
    settings_seed_legacy_api_key(&settings);

    map_set_api_key(settings.stadia_api_key);
    map_set_provider(settings.provider);

    if (argc > 1) {
        // The provider lives in the settings now. This stays as a way to
        // start on Stadia for one run without changing what is saved.
        if (argc == 2 && strcmp(argv[1], "-stadiamaps") == 0) {
            if (!map_has_api_key()) {
                fprintf(stderr,
                        "-stadiamaps needs an API key, and none is set.\n"
                        "Open the settings panel and paste one in, or put it in\n"
                        "%s as  stadia_api_key = <your key>\n",
                        SETTINGS_FILE);
                return EXIT_FAILURE;
            }
            printf("using stadiamaps\n");
            map_set_provider(MAP_PROVIDER_STADIA_TERRAIN);
        } else {
            fprintf(stderr, "The only supported argument is \"-stadiamaps\"\n");
            return EXIT_FAILURE;
        }
    }
    struct application appl = {
        .window = NULL,
        .renderer = NULL,
        .window_width = SCREEN_WIDTH,
        .window_height = SCREEN_HEIGHT,
        .zoom = settings.start_zoom,
        .world_x = settings.start_world_x,
        .world_y = settings.start_world_y,
        .running = 1,
        .dragging = 0,
        .left_mouse_button_pressed = false,
        .selected_track = -1,
        .rendered_overlay_track = -1,
        .overlay_key = {.track = -1},
        .download_queue.read_p = 0,
        .download_queue.write_p = 0,
        .download_queue.tile_in_dl = {.tile_x = -1, .tile_y = -1, .zoom = -1},
        .show_heat = true,
        .redraw_requested = true,
        .map_transform = MAP_TRANSFORM_IDENTITY,
    };
    download_in_progress = false;

    GpxCollection collection = {0};
    appl.collection = &collection;

    if (sdl_initialize(&appl)) {
        appl_cleanup(&appl, &collection);
        return EXIT_FAILURE;
    }

    SDL_GetWindowSize(appl.window, &appl.window_width,
                      &appl.window_height);
    clay_init(&appl);
    ui_load_icons(&appl);
    SDL_RenderPresent(appl.renderer);

    reset_filters(&collection.filters);

    // Reading the library and shading it are the two things slow enough to be
    // worth watching, so they run on a worker while the window stays live.
    background_start_load(&appl.background, &collection);

    // The tile downloader. It sleeps on the queue's condvar until the frame
    // loop puts a missing tile there, so an idle map costs nothing.
    map_init_events();
    pthread_mutex_init(&appl.download_queue.lock, NULL);
    pthread_cond_init(&appl.download_queue.cond, NULL);
    if (pthread_create(&appl.download_thread, NULL, download_tiles, (void *)&(appl.download_queue))) {
        fprintf(stderr, "Failed to create download thread\n");
        appl_cleanup(&appl, &collection);
        return EXIT_FAILURE;
    }
    appl.download_thread_started = true;

    // Fixed for the life of the process, so it is asked for once rather than
    // on every frame.
    const double counter_frequency = (double)SDL_GetPerformanceFrequency();
    appl.last_counter = SDL_GetPerformanceCounter();

    // Main-Loop
    while (appl.running) {
        handle_events(&appl, &collection);

        Uint64 now = SDL_GetPerformanceCounter();
        appl.delta_time = (float)((double)(now - appl.last_counter) / counter_frequency);
        appl.last_counter = now;
        if (appl.delta_time > MAX_FRAME_DELTA_SECONDS)
            appl.delta_time = MAX_FRAME_DELTA_SECONDS;

        // The worker owns the collection until it says otherwise; adopting the
        // results is the main thread's job, and has to happen between frames
        // rather than in the middle of one.
        if (background_collect(&appl.background)) {
            apply_filter_values(&collection);
            // The whole tracks array may have moved, so anything holding
            // numbers derived from it is stale.
            ui_invalidate_derived();
            // Only the tiles: the worker rebuilt the point index itself, on
            // the far side of the parse that moved the tracks.
            tracks_invalidate_filtered_view(&collection);
            LOG_DEBUG("Maximum heat is %d\n", collection.max_heat);
            app_request_redraw(&appl);
        }

        app_update(&appl, &collection);

        bool busy = background_busy(&appl.background);

        // Cleared before the frame rather than after it, so anything that asks
        // for a redraw while drawing is honoured on the next one instead of
        // being thrown away here.
        if (appl.redraw_requested) {
            appl.redraw_requested = false;
            SDL_RenderClear(appl.renderer);

            // Layer order lives here, where the frame is composed, rather
            // than inside whichever module happens to draw first.
            VisibleTile tiles[MAX_VISIBLE_TILES];
            int tile_count = map_visible_tiles(&appl, tiles, MAX_VISIBLE_TILES);
            map_draw_tiles(&appl, tiles, tile_count);

            // The map is safe to draw at any time; anything derived from the
            // tracks is not, while the worker still has them.
            if (!busy) {
                update_track_info_graphs(&appl, &collection);
                update_selected_track_overlay(&appl, &collection);
                tracks_draw_heat_tiles(&appl, &collection, tiles, tile_count);
                tracks_draw_selected_overlay(&appl);
            }

            clay_draw_ui(&appl, &collection);

            // Paced by the display: with vsync on, this is what makes a frame
            // take a frame.
            SDL_RenderPresent(appl.renderer);
        }

        // Applies to every iteration, not just the drawing ones: a frame the
        // loop declined to draw would otherwise spin. A renderer that waited
        // for the display has already spent the frame and this does nothing --
        // the cap is the floor under the frame rate, not the mechanism for
        // hitting it.
        Uint32 spent = (Uint32)((double)(SDL_GetPerformanceCounter() - appl.last_counter) *
                                1000.0 / counter_frequency);
        if (spent < FRAME_DELAY_MS)
            SDL_Delay(FRAME_DELAY_MS - spent);
    }

    appl_cleanup(&appl, &collection);

    return EXIT_SUCCESS;
}

static void appl_cleanup(struct application *appl, GpxCollection *collection) {
    LOG_DEBUG("Clean threads...\n");
    // Asks the worker to give up and waits for it, so nothing below frees
    // memory it is still reading.
    background_stop(&appl->background);
    // And the import helper, which is a process rather than a thread: without
    // this, quitting mid-import would wait for the download it was on.
    garmin_stop(&appl->garmin);
    // Wake the download worker out of its wait and wait for it to return before
    // tearing down the mutex and condvar it is blocked on.
    if (appl->download_thread_started) {
        download_thread_stop(&appl->download_queue);
        pthread_join(appl->download_thread, NULL);
        appl->download_thread_started = false;
        pthread_mutex_destroy(&appl->download_queue.lock);
        pthread_cond_destroy(&appl->download_queue.cond);
    }
    LOG_DEBUG("Clean textures...\n");
    tile_cache_free(&appl->tile_cache);
    tracks_free_collection_cache(collection);
    SDL_DestroyTexture(appl->selected_track_overlay);
    tracks_free_scratch(appl);
    LOG_DEBUG("Clean tracks...\n");
    for (int i = 0; i < collection->total_tracks; i++)
        free(collection->tracks[i].points);
    free(collection->tracks);
    free(collection->list_order);
    LOG_DEBUG("Clean UI...\n");
    ui_free_icons(appl);
    clay_free_memory();
    LOG_DEBUG("Clean renderer...\n");
    SDL_DestroyRenderer(appl->renderer);
    LOG_DEBUG("Clean window...\n");
    SDL_DestroyWindow(appl->window);
    LOG_DEBUG("Clean parser...\n");
    gpx_parser_cleanup();
    LOG_DEBUG("Clean SDL...\n");
    for (size_t i = 0; i < sizeof(appl->fonts) / sizeof(appl->fonts[0]); i++) {
        TTF_CloseFont(appl->fonts[i].font);
        appl->fonts[i].font = NULL;
    }
    TTF_Quit();
    SDL_Quit();
    IMG_Quit();
    LOG_DEBUG("exit...\n");
}

static bool sdl_initialize(struct application *appl) {
    if (SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Error initializing SDL: %s\n", SDL_GetError());
        return true;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "Error: could not initialize TTF: %s\n", TTF_GetError());
        return true;
    }
    TTF_Font *font = TTF_OpenFont("resources/Roboto-Regular.ttf", 16);
    if (!font) {
        fprintf(stderr, "Error: could not load font: %s\n", TTF_GetError());
        return true;
    }

    appl->fonts[0] = (SDL2_Font){
        .font_id = 0,
        .font = font,
    };

    // Sampled when a texture is created, not when one is drawn, so this has to
    // be set before the renderer and before anything is loaded. Without it the
    // map is point-sampled, and every tile drawn at anything but its own scale
    // comes out blocky.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    int img_init = IMG_Init(IMG_INIT_PNG);
    if ((img_init & IMG_INIT_PNG) != IMG_INIT_PNG) {
        fprintf(stderr, "Error initializing SDL_Image: %s\n", IMG_GetError());
        return true;
    }

    appl->window = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!appl->window) {
        fprintf(stderr, "Error creating window: %s\n", SDL_GetError());
        return true;
    }

    // Vsync is what paces the drawing frames. Not every driver offers it, so a
    // renderer without it is still worth having -- but then the loop has to
    // pace itself, or it presents as fast as the machine allows.
    appl->renderer = SDL_CreateRenderer(appl->window, -1,
                                        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!appl->renderer) {
        LOG_DEBUG("No accelerated vsync renderer (%s), falling back\n", SDL_GetError());
        appl->renderer = SDL_CreateRenderer(appl->window, -1, 0);
    }
    if (!appl->renderer) {
        fprintf(stderr, "Error creating renderer: %s\n", SDL_GetError());
        return true;
    }

    return false;
}

static void dispatch_event(struct application *appl, GpxCollection *collection,
                           SDL_Event event) {
    app_request_redraw(appl);
    map_handle_event(&event);

    if (event.type == SDL_QUIT) {
        appl->running = 0;
        return;
    }
    if (event.type == SDL_WINDOWEVENT &&
        event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        // Asked for here rather than every iteration: the size only ever
        // changes on this event.
        SDL_GetWindowSize(appl->window, &appl->window_width, &appl->window_height);
        return;
    }

    // A focused field takes the keys it has a use for and leaves the rest.
    // Neither of these is a link in the chain below, because a click that
    // leaves a field still has to reach whatever it landed on -- when this
    // consumed the press outright, a field was focused by the second click on
    // it rather than the first.
    //
    // The settings panel is offered first, and focusing a field in either one
    // blurs the other, so only one caret is ever live.
    if (ui_settings_input_active()) {
        // Only the settings panel has fields that take whole words, so this is
        // the only consumer of the characters SDL assembles for us.
        if (event.type == SDL_TEXTINPUT) {
            ui_settings_handle_text(event.text.text);
            return;
        }
        if (event.type == SDL_KEYDOWN &&
            ui_settings_handle_key(event.key.keysym.sym,
                                   (event.key.keysym.mod & KMOD_SHIFT) != 0))
            return;
        if (event.type == SDL_MOUSEBUTTONDOWN)
            ui_settings_blur();
    }

    // The Garmin panel's boxes take whole words too, and are offered after the
    // settings panel's for the same reason: only one caret may be live, and
    // focusing a box in either one blurs the other.
    if (ui_garmin_input_active()) {
        if (event.type == SDL_TEXTINPUT) {
            ui_garmin_handle_text(event.text.text);
            return;
        }
        if (event.type == SDL_KEYDOWN &&
            ui_garmin_handle_key(event.key.keysym.sym,
                                 (event.key.keysym.mod & KMOD_SHIFT) != 0))
            return;
        if (event.type == SDL_MOUSEBUTTONDOWN)
            ui_garmin_blur();
    }

    if (ui_filters_input_active()) {
        if (event.type == SDL_KEYDOWN &&
            ui_filters_handle_key(collection, event.key.keysym.sym,
                                  (event.key.keysym.mod & KMOD_SHIFT) != 0))
            return;
        if (event.type == SDL_MOUSEBUTTONDOWN)
            ui_filters_blur();
    }

    if (event.type == SDL_MOUSEWHEEL) {
        // Accumulated, not assigned: several detents can be drained in one
        // iteration, and every one of them has to count.
        appl->wheel_y += event.wheel.y;

        // The model moves a whole level here; how the picture gets there is
        // the map's business.
        if (!appl->mouse_over_ui)
            map_zoom_by_wheel(appl, event.wheel.y);
    }

    else if (event.type == SDL_MOUSEBUTTONDOWN) {
        if (event.button.button == SDL_BUTTON_RIGHT) {
            if (!appl->mouse_over_ui)
                appl->dragging = 1;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            appl->left_mouse_button_pressed = true;
        }
    } else if (event.type == SDL_MOUSEBUTTONUP &&
               event.button.button == SDL_BUTTON_RIGHT) {
        appl->dragging = 0;
    } else if (event.type == SDL_MOUSEBUTTONUP &&
               event.button.button == SDL_BUTTON_LEFT) {
        appl->left_mouse_button_pressed = false;
        if (!appl->mouse_over_ui) {
            // A click landing while a zoom is still easing is on a picture the
            // model has already moved past, so it is put back into the model's
            // own space before being unprojected.
            float unscaled_x, unscaled_y;
            map_screen_untransform(appl, (float)event.button.x, (float)event.button.y,
                                   &unscaled_x, &unscaled_y);

            int click_world_x, click_world_y;
            map_screen_to_world(appl, unscaled_x, unscaled_y, &click_world_x, &click_world_y);
            appl->selected_track = find_track_near_click(collection, click_world_x, click_world_y, appl->zoom, 10);
        }
    } else if (event.type == SDL_MOUSEMOTION && appl->dragging) {
        // Divided by the scale for the same reason: on a frame drawn at half
        // size a screen pixel of drag covers two of the model's, and without
        // this the map would lag the cursor for as long as a zoom was easing.
        const double per_pixel = map_world_per_pixel(appl) / appl->map_transform.scale;
        appl->world_x -= (int)(event.motion.xrel * per_pixel);
        appl->world_y -= (int)(event.motion.yrel * per_pixel);
    } else if (event.type == SDL_MOUSEMOTION) {
        appl->mouse_x = event.motion.x;
        appl->mouse_y = event.motion.y;
    } else if (event.type == SDL_KEYDOWN) {
        if (event.key.keysym.sym == SDLK_TAB)
            ui_toggle_run_list();
    }
}

// Drains the event queue. Polled rather than waited on: a frame the loop
// declines to draw costs almost nothing, and SDL_WaitEventTimeout falls back to
// a one-millisecond polling loop on any driver whose backend cannot wait on a
// descriptor -- more wakeups than the frame cap it would replace, not fewer.
static void handle_events(struct application *appl, GpxCollection *collection) {
    appl->wheel_y = 0; // reset to zero if no mousewheel action

    SDL_Event event;
    while (SDL_PollEvent(&event))
        dispatch_event(appl, collection, event);
}
