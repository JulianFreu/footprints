#ifndef RENDER_CACHE_H
#define RENDER_CACHE_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// Keeps the pictures the vendored Clay renderer would otherwise build from
// scratch on every frame: one rasterised surface per string, and one GPU
// texture per surface handed to it.
//
// The renderer rasterised every string with TTF_RenderUTF8_Blended, uploaded
// it, drew it, and destroyed it again, once per text element per frame -- an
// open run list is a couple of hundred of those, for text that is byte for byte
// what it was last frame. It did the same for every icon, and for the three
// sidebar graphs, which are a megabyte of upload between them.
//
// src/clay_renderer_sdl.c is vendored and not edited, so the functions below
// stand in for the SDL calls it makes. src/clay_sdl.c, the one translation unit
// that includes it, points its calls here with macros.
//
// The tables are fixed-size and static, and come to about 380 KB between them --
// half of that the scratch the rebuild after a removal re-places entries
// through. That is the trade: the memory is taken once, against a few hundred
// glyph rasterisations and GPU uploads per frame for as long as a panel is open.

// Hands back a rasterised surface for `text`, keyed by the string, the font and
// the size the font was last set to. The picture is white and opaque whatever
// colour is asked for: `color` is remembered and applied to the texture as a
// modulation instead, so that a panel fading in -- which scales the alpha of
// every colour in it, every frame -- reads from the cache rather than
// rasterising the whole panel again.
SDL_Surface *render_cache_text_surface(TTF_Font *font, const char *text, SDL_Color color);

// The texture for `surface`, created once and kept. Modulation is copied over
// on every call rather than baked in: from the colour above for text, and from
// the surface's own alpha and colour mod for an icon or a graph -- which is
// what SDL_CreateTextureFromSurface used to do implicitly, and so is what keeps
// ui_update's per-frame SDL_SetSurfaceAlphaMod working.
SDL_Texture *render_cache_texture(SDL_Renderer *renderer, SDL_Surface *surface);

// Stand in for SDL_DestroyTexture and SDL_FreeSurface. Anything the cache owns
// survives; anything it declined to cache is really given back.
void render_cache_release_texture(SDL_Texture *texture);
void render_cache_release_surface(SDL_Surface *surface);

// Records the size a font is set to, which is part of what a string is keyed
// by, and forwards to TTF_SetFontSize along with what it returned.
int render_cache_set_font_size(TTF_Font *font, int size);

// Ends the frame: drops what has not been drawn for a while. Called after the
// render pass, not before, so everything this frame drew is still counted.
void render_cache_end_frame(void);

// Forgets a surface the caller is about to free. Entries are keyed by pointer
// and the next allocation may land on the same address, so a surface that goes
// away has to say so -- otherwise the graph drawn for the next selection would
// be served the texture of the one before it.
void render_cache_forget_surface(SDL_Surface *surface);

void render_cache_free(void);

#endif
