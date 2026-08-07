#include "render_cache.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Longest string kept by its bytes. Everything drawn comes through
// ui_frame_printf or ui_track_text and fits well inside this; a longer one is
// rasterised and thrown away the way everything used to be.
#define TEXT_MAX 128

// Both tables are open-addressed with linear probing over a power-of-two number
// of slots, so a lookup is a hash and a short walk. Nothing is tombstoned:
// entries only ever go away in the sweep, which rebuilds the table around what
// is left.
#define TEXT_SLOTS 1024
#define TEX_SLOTS 1024

// Above this many entries the table is swept early rather than waiting for the
// end of the frame, and cleared outright if the sweep did not free enough. A
// clear costs one frame of rasterising everything, which is what every frame
// used to cost.
#define LOAD_LIMIT(slots) ((slots) * 7 / 10)

// Frames an entry may go undrawn before the sweep drops it. Long enough that a
// panel shut and opened again keeps its text, short enough that a run list
// scrolled through a long library does not hold every row it has ever shown.
#define IDLE_FRAMES 90

typedef struct TextEntry {
    SDL_Surface *surface; // NULL in an empty slot
    TTF_Font *font;
    int size;
    uint32_t length;
    char bytes[TEXT_MAX];
} TextEntry;

typedef struct TexEntry {
    SDL_Surface *surface; // the key; NULL in an empty slot
    SDL_Texture *texture;
    SDL_Color mod;
    bool from_text;
} TexEntry;

static TextEntry text_slots[TEXT_SLOTS];
static uint32_t text_seen[TEXT_SLOTS]; // frame each slot was last drawn on
static int text_count;

static TexEntry tex_slots[TEX_SLOTS];
static uint32_t tex_seen[TEX_SLOTS];
static int tex_count;

static uint32_t frame_counter;

// Surfaces and textures the cache declined to keep, and so has to give back.
// Nothing the UI draws lands here -- it is the escape hatch for a string past
// TEXT_MAX -- so a short array walked linearly is the whole of what it needs.
#define TRANSIENT_MAX 16
static SDL_Surface *transient_surfaces[TRANSIENT_MAX];
static SDL_Texture *transient_textures[TRANSIENT_MAX];

// The size each font was last set to. Part of what a string is keyed by: the
// same font pointer at two sizes draws two different pictures, and the renderer
// sets the size just before it asks for one.
#define FONT_MAX 8
static struct {
    TTF_Font *font;
    int size;
} font_sizes[FONT_MAX];
static int font_size_count;

// --- hashing ---

static uint32_t hash_bytes(const void *data, size_t length, uint32_t seed) {
    const unsigned char *bytes = (const unsigned char *)data;
    uint32_t hash = seed;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t hash_text(TTF_Font *font, int size, const char *text, uint32_t length) {
    uint32_t hash = hash_bytes(&font, sizeof(font), 2166136261u);
    hash = hash_bytes(&size, sizeof(size), hash);
    return hash_bytes(text, length, hash);
}

static uint32_t hash_pointer(const void *pointer) {
    return hash_bytes(&pointer, sizeof(pointer), 2166136261u);
}

// --- fonts ---

static int font_size_of(TTF_Font *font) {
    for (int i = 0; i < font_size_count; i++)
        if (font_sizes[i].font == font)
            return font_sizes[i].size;
    // Never set through us. TTF_FontHeight is not the size, but it is derived
    // from it and only has to tell two fonts apart that we have not been told
    // about -- which, with the renderer setting the size before every draw, is
    // a case that does not arise.
    return -TTF_FontHeight(font);
}

int render_cache_set_font_size(TTF_Font *font, int size) {
    for (int i = 0; i < font_size_count; i++) {
        if (font_sizes[i].font == font) {
            font_sizes[i].size = size;
            return TTF_SetFontSize(font, size);
        }
    }
    if (font_size_count < FONT_MAX) {
        font_sizes[font_size_count].font = font;
        font_sizes[font_size_count].size = size;
        font_size_count++;
    }
    return TTF_SetFontSize(font, size);
}

// --- the texture table ---

static int tex_find_slot(SDL_Surface *surface) {
    uint32_t mask = TEX_SLOTS - 1;
    uint32_t slot = hash_pointer(surface) & mask;
    for (uint32_t probe = 0; probe < TEX_SLOTS; probe++) {
        int at = (int)((slot + probe) & mask);
        if (!tex_slots[at].surface || tex_slots[at].surface == surface)
            return at;
    }
    return -1; // full, which the load limit is there to prevent
}

// Re-inserts every surviving entry into a fresh slot array. Linear probing
// cannot have an entry simply removed from the middle of it -- the walk that
// found the ones after it went through the hole.
static void tex_rebuild(void) {
    static TexEntry moved[TEX_SLOTS];
    static uint32_t moved_seen[TEX_SLOTS];
    int count = 0;

    for (int i = 0; i < TEX_SLOTS; i++) {
        if (tex_slots[i].surface) {
            moved[count] = tex_slots[i];
            moved_seen[count] = tex_seen[i];
            count++;
        }
    }

    memset(tex_slots, 0, sizeof(tex_slots));
    tex_count = 0;

    for (int i = 0; i < count; i++) {
        int at = tex_find_slot(moved[i].surface);
        tex_slots[at] = moved[i];
        tex_seen[at] = moved_seen[i];
        tex_count++;
    }
}

// Gives up one entry's texture. The surface is only ours to free when it is one
// we rasterised: an icon's belongs to UiIcons.
static void tex_drop_at(int slot) {
    if (tex_slots[slot].texture)
        SDL_DestroyTexture(tex_slots[slot].texture);
    tex_slots[slot].surface = NULL;
    tex_slots[slot].texture = NULL;
    tex_count--;
}

static void tex_forget(SDL_Surface *surface) {
    int slot = tex_find_slot(surface);
    if (slot >= 0 && tex_slots[slot].surface) {
        tex_drop_at(slot);
        tex_rebuild();
    }
}

// --- the text table ---

static int text_find_slot(TTF_Font *font, int size, const char *text, uint32_t length) {
    uint32_t mask = TEXT_SLOTS - 1;
    uint32_t slot = hash_text(font, size, text, length) & mask;
    for (uint32_t probe = 0; probe < TEXT_SLOTS; probe++) {
        int at = (int)((slot + probe) & mask);
        const TextEntry *entry = &text_slots[at];
        if (!entry->surface)
            return at;
        if (entry->font == font && entry->size == size && entry->length == length &&
            memcmp(entry->bytes, text, length) == 0)
            return at;
    }
    return -1;
}

static void text_rebuild(void) {
    static TextEntry moved[TEXT_SLOTS];
    static uint32_t moved_seen[TEXT_SLOTS];
    int count = 0;

    for (int i = 0; i < TEXT_SLOTS; i++) {
        if (text_slots[i].surface) {
            moved[count] = text_slots[i];
            moved_seen[count] = text_seen[i];
            count++;
        }
    }

    memset(text_slots, 0, sizeof(text_slots));
    text_count = 0;

    for (int i = 0; i < count; i++) {
        int at = text_find_slot(moved[i].font, moved[i].size, moved[i].bytes,
                                moved[i].length);
        text_slots[at] = moved[i];
        text_seen[at] = moved_seen[i];
        text_count++;
    }
}

// The surface and whatever was uploaded from it go together: the texture is
// keyed by the surface's address, and the next allocation may well be handed
// the same one.
static void text_drop_at(int slot) {
    tex_forget(text_slots[slot].surface);
    SDL_FreeSurface(text_slots[slot].surface);
    text_slots[slot].surface = NULL;
    text_count--;
}

// Drops everything that has not been drawn for a while. `keep_within` is how
// many frames back still counts as recent.
static void sweep(uint32_t keep_within) {
    bool text_removed = false;
    for (int i = 0; i < TEXT_SLOTS; i++) {
        if (text_slots[i].surface && frame_counter - text_seen[i] > keep_within) {
            text_drop_at(i);
            text_removed = true;
        }
    }
    if (text_removed)
        text_rebuild();

    bool tex_removed = false;
    for (int i = 0; i < TEX_SLOTS; i++) {
        if (tex_slots[i].surface && frame_counter - tex_seen[i] > keep_within) {
            tex_drop_at(i);
            tex_removed = true;
        }
    }
    if (tex_removed)
        tex_rebuild();
}

static void clear_all(void) {
    for (int i = 0; i < TEXT_SLOTS; i++) {
        if (text_slots[i].surface) {
            SDL_FreeSurface(text_slots[i].surface);
            text_slots[i].surface = NULL;
        }
    }
    text_count = 0;

    for (int i = 0; i < TEX_SLOTS; i++) {
        if (tex_slots[i].texture)
            SDL_DestroyTexture(tex_slots[i].texture);
        tex_slots[i].surface = NULL;
        tex_slots[i].texture = NULL;
    }
    tex_count = 0;
}

// Room for one more entry in either table, made by dropping what is stale and
// by starting over if that was not enough.
static void make_room(void) {
    if (text_count < LOAD_LIMIT(TEXT_SLOTS) && tex_count < LOAD_LIMIT(TEX_SLOTS))
        return;

    sweep(IDLE_FRAMES);
    if (text_count >= LOAD_LIMIT(TEXT_SLOTS) || tex_count >= LOAD_LIMIT(TEX_SLOTS))
        sweep(0); // anything not drawn on this frame
    if (text_count >= LOAD_LIMIT(TEXT_SLOTS) || tex_count >= LOAD_LIMIT(TEX_SLOTS))
        clear_all();
}

// --- transient surfaces and textures ---

static bool transient_add(void **table, void *pointer) {
    for (int i = 0; i < TRANSIENT_MAX; i++) {
        if (!table[i]) {
            table[i] = pointer;
            return true;
        }
    }
    return false;
}

static bool transient_take(void **table, void *pointer) {
    for (int i = 0; i < TRANSIENT_MAX; i++) {
        if (table[i] == pointer) {
            table[i] = NULL;
            return true;
        }
    }
    return false;
}

// --- the wrappers ---

SDL_Surface *render_cache_text_surface(TTF_Font *font, const char *text, SDL_Color color) {
    const size_t length = strlen(text);
    const int size = font_size_of(font);

    // Rendered white and opaque so that the colour can be a modulation on the
    // texture instead of part of the key. Alpha moves every frame a panel is
    // sliding, and that is the frame that can least afford a miss.
    const SDL_Color white = {255, 255, 255, 255};

    if (length >= TEXT_MAX) {
        // Past what a key holds. Rasterised in its own colour and given back on
        // release, which is what the renderer did with every string before this.
        SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
        if (surface && !transient_add((void **)transient_surfaces, surface)) {
            SDL_FreeSurface(surface); // no room to remember it, so do not leak it
            return NULL;
        }
        return surface;
    }

    make_room();

    int slot = text_find_slot(font, size, text, (uint32_t)length);
    if (slot < 0)
        return TTF_RenderUTF8_Blended(font, text, color); // cannot happen; leaks nothing it owns

    if (!text_slots[slot].surface) {
        SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, white);
        if (!surface)
            return NULL;

        text_slots[slot].surface = surface;
        text_slots[slot].font = font;
        text_slots[slot].size = size;
        text_slots[slot].length = (uint32_t)length;
        memcpy(text_slots[slot].bytes, text, length);
        text_count++;
    }
    text_seen[slot] = frame_counter;

    // The colour goes on the texture rather than the surface, so it is recorded
    // against the picture the texture will be made from.
    SDL_Surface *surface = text_slots[slot].surface;
    int tex_slot = tex_find_slot(surface);
    if (tex_slot >= 0) {
        if (!tex_slots[tex_slot].surface) {
            tex_slots[tex_slot].surface = surface;
            tex_slots[tex_slot].texture = NULL;
            tex_count++;
        }
        tex_slots[tex_slot].from_text = true;
        tex_slots[tex_slot].mod = color;
        tex_seen[tex_slot] = frame_counter;
    }
    return surface;
}

SDL_Texture *render_cache_texture(SDL_Renderer *renderer, SDL_Surface *surface) {
    if (!surface)
        return NULL;

    // A surface the cache declined to keep gets a texture of the same standing.
    for (int i = 0; i < TRANSIENT_MAX; i++) {
        if (transient_surfaces[i] == surface) {
            SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
            if (texture && !transient_add((void **)transient_textures, texture)) {
                SDL_DestroyTexture(texture);
                return NULL;
            }
            return texture;
        }
    }

    make_room();

    int slot = tex_find_slot(surface);
    if (slot < 0)
        return SDL_CreateTextureFromSurface(renderer, surface);

    if (!tex_slots[slot].surface) {
        tex_slots[slot].surface = surface;
        tex_slots[slot].texture = NULL;
        tex_slots[slot].from_text = false;
        tex_count++;
    }

    TexEntry *entry = &tex_slots[slot];
    if (!entry->texture) {
        entry->texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (!entry->texture) {
            entry->surface = NULL;
            tex_count--;
            return NULL;
        }
        SDL_SetTextureBlendMode(entry->texture, SDL_BLENDMODE_BLEND);
    }
    tex_seen[slot] = frame_counter;

    if (!entry->from_text) {
        // An icon or a graph carries its fade on the surface, which is where
        // ui_update sets it. Copied over rather than baked in, which is what
        // SDL_CreateTextureFromSurface was doing every frame.
        SDL_GetSurfaceColorMod(surface, &entry->mod.r, &entry->mod.g, &entry->mod.b);
        SDL_GetSurfaceAlphaMod(surface, &entry->mod.a);
    }
    SDL_SetTextureColorMod(entry->texture, entry->mod.r, entry->mod.g, entry->mod.b);
    SDL_SetTextureAlphaMod(entry->texture, entry->mod.a);
    return entry->texture;
}

void render_cache_release_texture(SDL_Texture *texture) {
    if (texture && transient_take((void **)transient_textures, texture))
        SDL_DestroyTexture(texture);
}

void render_cache_release_surface(SDL_Surface *surface) {
    if (surface && transient_take((void **)transient_surfaces, surface))
        SDL_FreeSurface(surface);
}

void render_cache_end_frame(void) {
    frame_counter++;
    // Only walked when there is something worth walking for. The tables are a
    // thousand slots each, and a sweep on every frame would be the kind of
    // per-frame cost this file exists to remove.
    if ((frame_counter & 63) == 0)
        sweep(IDLE_FRAMES);
}

void render_cache_forget_surface(SDL_Surface *surface) {
    if (surface)
        tex_forget(surface);
}

void render_cache_free(void) {
    clear_all();
    for (int i = 0; i < TRANSIENT_MAX; i++) {
        if (transient_textures[i]) {
            SDL_DestroyTexture(transient_textures[i]);
            transient_textures[i] = NULL;
        }
        if (transient_surfaces[i]) {
            SDL_FreeSurface(transient_surfaces[i]);
            transient_surfaces[i] = NULL;
        }
    }
    font_size_count = 0;
}
