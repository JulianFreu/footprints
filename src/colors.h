#ifndef COLORS_H
#define COLORS_H

#include "clay.h"

// Header-only palette. These are `static` so each translation unit that
// includes this file gets its own copy; as plain `const` at file scope they had
// external linkage, and a second includer would fail to link with
// "multiple definition of 'bg'".

// Dark medium palette
static const Clay_Color bg0 = {0x19, 0x1a, 0x1a, 0xff}; // #191a1aff
static const Clay_Color bg1 = {0x20, 0x21, 0x21, 0xff}; // #202121ff
static const Clay_Color bg = {0x29, 0x28, 0x28, 0xff};  // #292828ff
static const Clay_Color bg2 = {0x32, 0x30, 0x2f, 0xff}; // #32302fff
static const Clay_Color bg3 = {0x38, 0x34, 0x32, 0xff}; // #383432ff
static const Clay_Color bg4 = {0x3c, 0x38, 0x36, 0xff}; // #3c3836ff
static const Clay_Color bg5 = {0x45, 0x40, 0x3d, 0xff}; // #45403dff
static const Clay_Color bg6 = {0x50, 0x49, 0x45, 0xff}; // #504945ff
static const Clay_Color bg7 = {0x5a, 0x52, 0x4c, 0xff}; // #5a524cff
static const Clay_Color bg8 = {0x66, 0x5c, 0x54, 0xff}; // #665c54ff
static const Clay_Color bg9 = {0x7c, 0x6f, 0x64, 0xff}; // #7c6f64ff

static const Clay_Color grey0 = {0x7c, 0x6f, 0x64, 0xff}; // #7c6f64ff
static const Clay_Color grey1 = {0x92, 0x83, 0x74, 0xff}; // #928374ff
static const Clay_Color grey2 = {0xa8, 0x99, 0x84, 0xff}; // #a89984ff

static const Clay_Color fg0 = {0xe2, 0xcc, 0xa9, 0xff}; // #e2cca9ff
static const Clay_Color fg = {0xe2, 0xcc, 0xa9, 0xff};  // #e2cca9ff
static const Clay_Color fg1 = {0xc5, 0xb1, 0x8d, 0xff}; // #c5b18dff

static const Clay_Color red = {0xf2, 0x59, 0x4b, 0xff};    // #f2594bff
static const Clay_Color orange = {0xf2, 0x85, 0x34, 0xff}; // #f28534ff
static const Clay_Color yellow = {0xe9, 0xb1, 0x43, 0xff}; // #e9b143ff
static const Clay_Color green = {0xb0, 0xb8, 0x46, 0xff};  // #b0b846ff
static const Clay_Color aqua = {0x8b, 0xba, 0x7f, 0xff};   // #8bba7fff
static const Clay_Color blue = {0x80, 0xaa, 0x9e, 0xff};   // #80aa9eff
static const Clay_Color purple = {0xd3, 0x86, 0x9b, 0xff}; // #d3869bff

static const Clay_Color darkRed = {0xb8, 0x56, 0x51, 0xff};    // #b85651ff
static const Clay_Color darkOrange = {0xbd, 0x6f, 0x3e, 0xff}; // #bd6f3eff
static const Clay_Color darkYellow = {0xc1, 0x8f, 0x41, 0xff}; // #c18f41ff
static const Clay_Color darkGreen = {0x8f, 0x9a, 0x52, 0xff};  // #8f9a52ff
static const Clay_Color darkAqua = {0x72, 0x96, 0x6c, 0xff};   // #72966cff
static const Clay_Color darkBlue = {0x68, 0x94, 0x8a, 0xff};   // #68948aff
static const Clay_Color darkPurple = {0xab, 0x6c, 0x7d, 0xff}; // #ab6c7dff

static const Clay_Color selGreen = {0x3b, 0x44, 0x39, 0xff}; // #3b4439ff
static const Clay_Color selRed = {0x4c, 0x34, 0x32, 0xff};   // #4c3432ff
static const Clay_Color selBlue = {0x37, 0x41, 0x41, 0xff};  // #374141ff

static const Clay_Color diffGreen = {0x34, 0x38, 0x1b, 0xff}; // #34381bff
static const Clay_Color diffRed = {0x40, 0x21, 0x20, 0xff};   // #402120ff
static const Clay_Color diffBlue = {0x0e, 0x36, 0x3e, 0xff};  // #0e363eff

static const Clay_Color shadow = {0x00, 0x00, 0x00, 0x70}; // #00000070

static const Clay_Color bg_d = bg;
static const Clay_Color bg_l = bg3;

static const Clay_Color fg_d = fg1;
static const Clay_Color fg_l = fg;

static const Clay_Color accent_color = darkRed;
static const Clay_Color accent_color_hl = red;

static const Clay_Color bigButtonColor = fg1;
static const Clay_Color border_hl = darkOrange;
static const Clay_Color border = fg_l;
#endif
