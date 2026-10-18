#include "bz_tokens.h"

/* device.css:4-6 (dark) and :13-15 (light); glass constants from shaders.js:194 and app.js:243.
 * Status colours are Catalyst Console's (styles.css:102-109) in dark; light mode gets its own darker
 * set that holds 4.5:1 on surface1 rather than an inversion. */
const bz_palette_t BZ_PAL_DARK = {
    .ground = 0x0B0C0E, .surface1 = 0x16181B, .surface2 = 0x1F2226, .surface3 = 0x2A2E33,
    .track = 0x1F2226, .meter = 0x24272C,
    .ink = 0xEEF0F2, .dim = 0x9AA1A8, .faint = 0x5D646B,
    .signal = 0xFF5B1F, .on_signal = 0x1A0A02,
    .ice = 0xCFE0EE, .on_ice = 0x0E2233,
    .amber = 0xF4C542, .on_amber = 0x2A2000, .amber_container = 0x3B3218, .amber_ink = 0xF4C542,
    .leaf = 0x9FD68B, .leaf_container = 0x22361D,
    .ok = 0x30D158, .warn = 0xFFB340, .fault = 0xFF453A, .stale = 0x5D646B,
    .glass_even = 0x2B2B2B, .glass_even_amt = 0.22f,
    .rim = 0xFFF7ED, .rim_gain = 1.0f, .shadow_gain = 1.0f,
};

const bz_palette_t BZ_PAL_LIGHT = {
    .ground = 0xE8EAED, .surface1 = 0xFBFBFC, .surface2 = 0xEEF0F3, .surface3 = 0xE1E4E8,
    .track = 0xFBFBFC, .meter = 0xE4E7EB,
    .ink = 0x15181B, .dim = 0x5B636C, .faint = 0xA3AAB2,
    .signal = 0xEA5A1A, .on_signal = 0xFFFFFF,
    .ice = 0xCDE1F3, .on_ice = 0x0C2436,
    .amber = 0xFFD24A, .on_amber = 0x2B2100, .amber_container = 0xF6E3A4, .amber_ink = 0x8A5F00,
    .leaf = 0x2F6B27, .leaf_container = 0xD3EBC6,
    .ok = 0x1F9D45, .warn = 0xB86E00, .fault = 0xD42A20, .stale = 0xA3AAB2,
    .glass_even = 0xEDEDED, .glass_even_amt = 0.38f,
    .rim = 0xFFFFFF, .rim_gain = 0.85f, .shadow_gain = 0.8f,
};

const bz_palette_t *bz_pal = &BZ_PAL_DARK;
