#pragma once

#include <stdint.h>

/** 256 glyphs × 5 columns (Adafruit/TFT_eSPI font 1). */
extern const uint8_t panel_font_glcd[1280];

/** Returns 5-byte glyph for ASCII byte c (space if out of table). */
const uint8_t *panel_font_glcd_glyph(uint8_t c);
