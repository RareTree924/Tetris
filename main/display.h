#pragma once
// ============================================================
//  DISPLAY — 480x480 ST7701 RGB panel. Everything draws straight
//  into the panel's own framebuffer (in PSRAM); the panel scans it
//  out continuously, so whatever you draw shows up immediately.
//  Colours are RGB565.
// ============================================================
#include <stdint.h>

#define LCD_H_RES 480
#define LCD_V_RES 480

// ---- RGB565 colours ----
#define COLOR_RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_ORANGE  COLOR_RGB565(255, 140, 0)
#define COLOR_GREY    COLOR_RGB565(128, 128, 128)

// Sends the panel its init sequence and starts the RGB scan-out. Call once at boot.
void display_begin(void);

// The live framebuffer: LCD_H_RES * LCD_V_RES pixels, row-major.
uint16_t *display_framebuffer(void);

// ---- shapes (everything is clipped to the screen) ----
void fill_screen(uint16_t color);
void draw_pixel(int x, int y, uint16_t color);
void draw_hline(int x, int y, int w, uint16_t color);
void draw_vline(int x, int y, int h, uint16_t color);
void draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void fill_rect(int x, int y, int w, int h, uint16_t color);
void draw_rect(int x, int y, int w, int h, uint16_t color);
void fill_circle(int cx, int cy, int r, uint16_t color);
void draw_circle(int cx, int cy, int r, uint16_t color);
// Rectangles with corners rounded to radius r (r is capped at half the box).
void draw_round_rect(int x, int y, int w, int h, int r, uint16_t color);
void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);

// ---- images: w*h RGB565 pixels, row-major ----
void draw_image(int x, int y, int w, int h, const uint16_t *pixels);
// Same, but pixels equal to `transparent` are skipped.
void draw_image_keyed(int x, int y, int w, int h, const uint16_t *pixels, uint16_t transparent);

// ---- text: built-in 5x7 font, ASCII 32..126. Each font pixel is scale x scale. ----
void draw_char(int x, int y, char c, int scale, uint16_t color);
void draw_text(int x, int y, const char *s, int scale, uint16_t color);
int  text_width(const char *s, int scale);   // pixels draw_text() would cover
void draw_num(int x, int y, int value, int scale, uint16_t color);   // draw_text(), but for an int
