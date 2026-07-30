#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "app.h"
#include "filters.h"
#include "gpx_parser.h"
#include "gpx_types.h"
#include "heat.h"
#include "map.h"
#include "tracks.h"
#include "ui.h"

static bool sdl_initialize(struct application *appl);
static void appl_cleanup(struct application *appl, GpxCollection *collection);
static bool handle_events(struct application *appl, GpxCollection *collection);

static bool animation_in_progress(UIState ui) {
    bool animation_in_progress = false;
    if (ui.right_sidebar.opening == true || ui.right_sidebar.closing == true)
        animation_in_progress = true;
    if (ui.run_list.opening == true || ui.run_list.closing == true)
        animation_in_progress = true;
    if (ui.filters_animation.opening == true || ui.filters_animation.closing == true)
        animation_in_progress = true;

    return animation_in_progress;
}

static void append_to_input_buffer(UIState *ui, char c) {
    // Leave space for null terminator
    if (ui->text_input_length < sizeof(ui->text_input_buffer) - 1) {
        ui->text_input_buffer[ui->text_input_length++] = c;
        ui->text_input_buffer[ui->text_input_length] = '\0';
    }
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        if (argc == 2 && strcmp(argv[1], "-stadiamaps") == 0) {
            printf("using stadiamaps\n");
            use_osm_tiles = false;
        } else {
            printf("The only supported argument is \"-stadiamaps\"\n");
            exit(1);
        }
    }
    struct application appl = {
        .window = NULL,
        .renderer = NULL,
        .window_width = SCREEN_WIDTH,
        .window_height = SCREEN_HEIGHT,
        .zoom = START_ZOOM,
        .world_x = START_WORLD_X,
        .world_y = START_WORLD_Y,
        .running = 1,
        .dragging = 0,
        .left_mouse_button_pressed = false,
        .selected_track = -1,
        .download_queue.read_p = 0,
        .download_queue.write_p = 0,
        .download_queue.tile_in_dl = {.tile_x = -1, .tile_y = -1, .zoom = -1},
        .show_heat = true,
        .update_window = true,
    };
    download_in_progress = false;

    GpxCollection collection = {0};

    if (sdl_initialize(&appl)) {
        appl_cleanup(&appl, &collection);
        return EXIT_FAILURE;
    }

    SDL_GetWindowSize(appl.window, &appl.window_width,
                      &appl.window_height);
    clay_init(&appl);
    ui_load_icons(&appl);
    SDL_RenderPresent(appl.renderer);

    gpx_parse_all_files(&collection);

    reset_filters(&collection.filters);
    apply_filter_values(&collection);

    calculate_heatmap(&collection);
    printf("Maximum heat is %d\n", collection.max_heat);

    // start thread that will donwload missing tiles of the map
    // the thread will constantly check the download queue for missing tiles and download them
    pthread_mutex_init(&appl.download_queue.lock, NULL);
    pthread_cond_init(&appl.download_queue.cond, NULL);
    if (pthread_create(&appl.download_thread, NULL, download_tiles, (void *)&(appl.download_queue))) {
        fprintf(stderr, "Failed to create download thread\n");
        appl_cleanup(&appl, &collection);
        return EXIT_FAILURE;
    }
    appl.download_thread_started = true;

    appl.last_frame_time = SDL_GetTicks();

    Uint32 frame_time;
    int fps_counter = 0;
    float fps_timer = 0;

    // Main-Loop
    while (appl.running) {
        appl.last_frame_time = SDL_GetTicks();
        SDL_GetWindowSize(appl.window, &appl.window_width,
                          &appl.window_height);
        handle_events(&appl, &collection);

        if (appl.update_window || animation_in_progress(ui) || download_in_progress) {
            appl.update_window = false;
            SDL_RenderClear(appl.renderer);

            update_track_info_graphs(&appl, &collection);

            update_selected_track_overlay(&appl, &collection);

            get_map_background(&appl, &collection);

            clay_draw_ui(&appl, &collection);

            SDL_RenderPresent(appl.renderer);
        }

        // FPS counter
        fps_timer += get_delta_time(appl.last_frame_time);
        fps_counter++;

        if (fps_timer >= 1.0f) {
            appl.current_fps = fps_counter;
            fps_counter = 0;
            fps_timer = 0.0f;
        }

        frame_time = SDL_GetTicks() - appl.last_frame_time;
        if (frame_time < FRAME_DELAY_MS) {
            SDL_Delay(FRAME_DELAY_MS - frame_time);
        }
    }

    appl_cleanup(&appl, &collection);

    return EXIT_SUCCESS;
}

static void appl_cleanup(struct application *appl, GpxCollection *collection) {
    printf("Clean threads...\n");
    // Wake the download worker out of its wait and wait for it to return before
    // tearing down the mutex and condvar it is blocked on.
    if (appl->download_thread_started) {
        download_thread_stop(&appl->download_queue);
        pthread_join(appl->download_thread, NULL);
        appl->download_thread_started = false;
        pthread_mutex_destroy(&appl->download_queue.lock);
        pthread_cond_destroy(&appl->download_queue.cond);
    }
    printf("Clean textures...\n");
    free_tile_cache(&(appl->tile_cache));
    free_track_tile_cache(&collection->track_tile_cache);
    for (int zoom = 0; zoom <= MAX_ZOOM; zoom++)
        SDL_DestroyTexture(appl->selected_track_overlay[zoom]);
    tracks_free_scratch();
    printf("Clean tracks...\n");
    for (int i = 0; i < collection->total_tracks; i++)
        free(collection->tracks[i].points);
    free(collection->tracks);
    free(collection->list_order);
    printf("Clean UI...\n");
    ui_free_icons(appl);
    clay_free_memory();
    printf("Clean renderer...\n");
    SDL_DestroyRenderer(appl->renderer);
    printf("Clean window...\n");
    SDL_DestroyWindow(appl->window);
    printf("Clean parser...\n");
    gpx_parser_cleanup();
    printf("Clean SDL...\n");
    for (size_t i = 0; i < sizeof(appl->fonts) / sizeof(appl->fonts[0]); i++) {
        TTF_CloseFont(appl->fonts[i].font);
        appl->fonts[i].font = NULL;
    }
    TTF_Quit();
    SDL_Quit();
    IMG_Quit();
    printf("exit...\n");
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

    appl->renderer = SDL_CreateRenderer(appl->window, -1, 0);
    if (!appl->renderer) {
        fprintf(stderr, "Error creating renderer: %s\n", SDL_GetError());
        return true;
    }

    return false;
}

static bool handle_events(struct application *appl, GpxCollection *collection) {
    appl->wheel_y = 0; // reset to zero if no mousewheel action

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        appl->update_window = true;
        if (event.type == SDL_QUIT) {
            appl->running = 0;
        } else if (ui.text_input_mode) {
            if (event.type == SDL_KEYDOWN) {
                SDL_Keycode key = event.key.keysym.sym;

                if (key >= SDLK_0 && key <= SDLK_9) {
                    char digit = '0' + (key - SDLK_0);
                    append_to_input_buffer(&ui, digit);
                } else {
                    // Exit input mode on any non-digit key
                    ui.text_input_mode = false;
                    ui.active_filter_id = 0;
                    printf("%s\n", ui.text_input_buffer);
                    save_filter_values(&collection->filters);
                    apply_filter_values(collection);
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                // Exit input mode on any mouse click
                ui.text_input_mode = false;
                ui.active_filter_id = 0;
                printf("%s\n", ui.text_input_buffer);
                save_filter_values(&collection->filters);
                apply_filter_values(collection);
            }
        } else if (event.type == SDL_MOUSEWHEEL) {
            appl->wheel_y = event.wheel.y;

            if (!appl->mouse_over_ui) {
                int old_zoom = appl->zoom;

                appl->zoom += event.wheel.y;

                // Clamp zoom range
                if (appl->zoom < MIN_ZOOM)
                    appl->zoom = MIN_ZOOM;
                if (appl->zoom > MAX_ZOOM)
                    appl->zoom = MAX_ZOOM;

                if (appl->zoom != old_zoom) {
                    int shift_before = MAX_ZOOM - old_zoom;
                    int shift_after = MAX_ZOOM - appl->zoom;

                    // World coordinate under the mouse BEFORE zoom
                    int world_mouse_x = appl->world_x + (appl->mouse_x << shift_before) - (appl->window_width / 2 << shift_before);
                    int world_mouse_y = appl->world_y + (appl->mouse_y << shift_before) - (appl->window_height / 2 << shift_before);

                    // Recalculate world_x and world_y AFTER zoom to keep mouse on same map point
                    appl->world_x = world_mouse_x - (appl->mouse_x << shift_after) + (appl->window_width / 2 << shift_after);
                    appl->world_y = world_mouse_y - (appl->mouse_y << shift_after) + (appl->window_height / 2 << shift_after);
                }
            }
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
                int click_world_x = appl->world_x + (event.button.x << (MAX_ZOOM - appl->zoom)) - ((appl->window_width / 2) << (MAX_ZOOM - appl->zoom));
                int click_world_y = appl->world_y + (event.button.y << (MAX_ZOOM - appl->zoom)) - ((appl->window_height / 2) << (MAX_ZOOM - appl->zoom));
                appl->selected_track = find_track_near_click(collection, click_world_x, click_world_y, appl->zoom, 10);
            }
        } else if (event.type == SDL_MOUSEMOTION && appl->dragging) {
            appl->world_x -= (event.motion.xrel << (MAX_ZOOM - appl->zoom));
            appl->world_y -= (event.motion.yrel << (MAX_ZOOM - appl->zoom));
        } else if (event.type == SDL_MOUSEMOTION) {
            appl->mouse_x = event.motion.x;
            appl->mouse_y = event.motion.y;
        } else if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_TAB) {
                if (ui.run_list.animation > 0) {
                    ui.run_list.opening = false;
                    ui.run_list.closing = true;
                    ui.filters_animation.opening = false;
                    ui.filters_animation.closing = true;
                } else {
                    ui.run_list.opening = true;
                    ui.run_list.closing = false;
                    ui.filters_animation.opening = true;
                    ui.filters_animation.closing = false;
                }
            }
        }
    }
    return true;
}