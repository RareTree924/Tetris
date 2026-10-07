#include "display.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;   // the panel driver's own buffer — draw here

// ============================================================
//  3-wire SPI (write-only, 9-bit), bit-banged — only used to send the
//  ST7701 its init sequence. SCK/SDA are the same pins as the SD card's
//  SPI bus; that's fine, the card has its own CS (see audio.c).
// ============================================================
#define PIN_TFT_CS   1
#define PIN_TFT_SCK  12
#define PIN_TFT_SDA  11

static void spi_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_TFT_CS) | (1ULL << PIN_TFT_SCK) | (1ULL << PIN_TFT_SDA),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_TFT_CS, 1);
    gpio_set_level(PIN_TFT_SCK, 0);
}

// 9-bit word: 1 D/C bit (0=cmd,1=data) + 8 data bits, MSB first.
static void write9(uint8_t dc, uint8_t val)
{
    gpio_set_level(PIN_TFT_CS, 0);
    gpio_set_level(PIN_TFT_SDA, dc);
    gpio_set_level(PIN_TFT_SCK, 1);
    gpio_set_level(PIN_TFT_SCK, 0);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_TFT_SDA, (val >> i) & 0x01);
        gpio_set_level(PIN_TFT_SCK, 1);
        gpio_set_level(PIN_TFT_SCK, 0);
    }
    gpio_set_level(PIN_TFT_CS, 1);
}

static void cmd(uint8_t c) { write9(0, c); }
static void dat(uint8_t d) { write9(1, d); }
static void cmd_data(uint8_t c, const uint8_t *d, size_t len)
{
    cmd(c);
    for (size_t i = 0; i < len; i++) dat(d[i]);
}

// ST7701 register sequence (from Arduino_GFX's st7701_type1_init_operations).
static void st7701_init_sequence(void)
{
    // Page 0x10: gamma
    cmd_data(0xFF, (uint8_t[]){0x77,0x01,0x00,0x00,0x10}, 5);
    cmd_data(0xC0, (uint8_t[]){0x3B,0x00}, 2);
    cmd_data(0xC1, (uint8_t[]){0x0D,0x02}, 2);
    cmd_data(0xC2, (uint8_t[]){0x31,0x05}, 2);
    cmd_data(0xCD, (uint8_t[]){0x08}, 1);
    cmd_data(0xB0, (uint8_t[]){0x00,0x11,0x18,0x0E,0x11,0x06,0x07,0x08,
                                0x07,0x22,0x04,0x12,0x0F,0xAA,0x31,0x18}, 16); // +V gamma
    cmd_data(0xB1, (uint8_t[]){0x00,0x11,0x19,0x0E,0x12,0x07,0x08,0x08,
                                0x08,0x22,0x04,0x11,0x11,0xA9,0x32,0x18}, 16); // -V gamma

    // Page 0x11: power + GIP timing
    cmd_data(0xFF, (uint8_t[]){0x77,0x01,0x00,0x00,0x11}, 5);
    cmd_data(0xB0, (uint8_t[]){0x60}, 1); // Vop
    cmd_data(0xB1, (uint8_t[]){0x32}, 1); // VCOM
    cmd_data(0xB2, (uint8_t[]){0x07}, 1); // VGH
    cmd_data(0xB3, (uint8_t[]){0x80}, 1);
    cmd_data(0xB5, (uint8_t[]){0x49}, 1); // VGL
    cmd_data(0xB7, (uint8_t[]){0x85}, 1);
    cmd_data(0xB8, (uint8_t[]){0x21}, 1); // AVDD/AVCL
    cmd_data(0xC1, (uint8_t[]){0x78}, 1);
    cmd_data(0xC2, (uint8_t[]){0x78}, 1);
    cmd_data(0xE0, (uint8_t[]){0x00,0x1B,0x02}, 3);
    cmd_data(0xE1, (uint8_t[]){0x08,0xA0,0x00,0x00,0x07,0xA0,0x00,0x00,
                                0x00,0x44,0x44}, 11);
    cmd_data(0xE2, (uint8_t[]){0x11,0x11,0x44,0x44,0xED,0xA0,0x00,0x00,
                                0xEC,0xA0,0x00,0x00}, 12);
    cmd_data(0xE3, (uint8_t[]){0x00,0x00,0x11,0x11}, 4);
    cmd_data(0xE4, (uint8_t[]){0x44,0x44}, 2);
    cmd_data(0xE5, (uint8_t[]){0x0A,0xE9,0xD8,0xA0,0x0C,0xEB,0xD8,0xA0,
                                0x0E,0xED,0xD8,0xA0,0x10,0xEF,0xD8,0xA0}, 16);
    cmd_data(0xE6, (uint8_t[]){0x00,0x00,0x11,0x11}, 4);
    cmd_data(0xE7, (uint8_t[]){0x44,0x44}, 2);
    cmd_data(0xE8, (uint8_t[]){0x09,0xE8,0xD8,0xA0,0x0B,0xEA,0xD8,0xA0,
                                0x0D,0xEC,0xD8,0xA0,0x0F,0xEE,0xD8,0xA0}, 16);
    cmd_data(0xEB, (uint8_t[]){0x02,0x00,0xE4,0xE4,0x88,0x00,0x40}, 7);
    cmd_data(0xEC, (uint8_t[]){0x3C,0x00}, 2);
    cmd_data(0xED, (uint8_t[]){0xAB,0x89,0x76,0x54,0x02,0xFF,0xFF,0xFF,
                                0xFF,0xFF,0xFF,0x20,0x45,0x67,0x98,0xBA}, 16);

    // Page 0x13: VAP/VAN trim
    cmd_data(0xFF, (uint8_t[]){0x77,0x01,0x00,0x00,0x13}, 5);
    cmd_data(0xE5, (uint8_t[]){0xE4}, 1);

    // Back to page 0: panel mode, color format, wake
    cmd_data(0xFF, (uint8_t[]){0x77,0x01,0x00,0x00,0x00}, 5);
    cmd(0x21);                            // inversion on (IPS panel)
    cmd_data(0x3A, (uint8_t[]){0x60}, 1); // RGB666
    cmd(0x11);                            // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x29);                            // display on
}

void display_begin(void)
{
    spi_gpio_init();
    st7701_init_sequence();

    // data_gpio_nums[0..15] = bit0..bit15 of RGB565: bits 0-4=B0-B4, 5-10=G0-G5, 11-15=R0-R4.
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 8,
            .hsync_back_porch = 50,
            .hsync_front_porch = 10,
            .vsync_pulse_width = 8,
            .vsync_back_porch = 20,
            .vsync_front_porch = 10,
            .flags.pclk_active_neg = false,
        },
        .data_width = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = 480 * 10,
        .dma_burst_size = 64,
        .hsync_gpio_num = 5,
        .vsync_gpio_num = 4,
        .de_gpio_num = 45,
        .pclk_gpio_num = 21,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            6, 7, 15, 16, 8,        // B0-B4
            0, 9, 14, 47, 48, 3,    // G0-G5
            39, 40, 41, 42, 2,      // R0-R4
        },
        .flags.fb_in_psram = true,
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, (void **)&s_fb));

    fill_screen(COLOR_BLACK);
    ESP_LOGI(TAG, "display up: %dx%d", LCD_H_RES, LCD_V_RES);
}

uint16_t *display_framebuffer(void)
{
    return s_fb;
}

// ============================================================
//  SHAPES
// ============================================================
void fill_screen(uint16_t color)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) s_fb[i] = color;
}

// No rotation: (0,0) is the top-left of the screen as you hold it,
// the same way up as the text.
void draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= LCD_H_RES || y >= LCD_V_RES) return;
    s_fb[y * LCD_H_RES + x] = color;
}

void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    // clip to the screen once, then fill row by row
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++) {
        uint16_t *row = &s_fb[j * LCD_H_RES + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

void draw_hline(int x, int y, int w, uint16_t color) { fill_rect(x, y, w, 1, color); }
void draw_vline(int x, int y, int h, uint16_t color) { fill_rect(x, y, 1, h, color); }

void draw_rect(int x, int y, int w, int h, uint16_t color)
{
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
}

// Bresenham: any direction, both ends included.
void draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fill_circle(int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;   // half-width of this row
        draw_hline(cx - dx, cy + dy, 2 * dx + 1, color);
    }
}

// Midpoint circle outline.
void draw_circle(int cx, int cy, int r, uint16_t color)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        draw_pixel(cx + x, cy + y, color); draw_pixel(cx - x, cy + y, color);
        draw_pixel(cx + x, cy - y, color); draw_pixel(cx - x, cy - y, color);
        draw_pixel(cx + y, cy + x, color); draw_pixel(cx - y, cy + x, color);
        draw_pixel(cx + y, cy - x, color); draw_pixel(cx - y, cy - x, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

// Keeps a corner radius from being bigger than half the box.
static int clamp_radius(int w, int h, int r)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    return r < 0 ? 0 : r;
}

// Rectangle outline with rounded corners: straight edges, plus a quarter
// of a midpoint circle in each corner.
void draw_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(w, h, r);

    draw_hline(x + r, y,         w - 2 * r, color);   // top
    draw_hline(x + r, y + h - 1, w - 2 * r, color);   // bottom
    draw_vline(x,         y + r, h - 2 * r, color);   // left
    draw_vline(x + w - 1, y + r, h - 2 * r, color);   // right

    // centres of the four corner circles
    int left = x + r, right = x + w - 1 - r, top = y + r, bottom = y + h - 1 - r;
    int px = r, py = 0, err = 1 - r;
    while (px >= py) {
        draw_pixel(right + px, top - py, color);    draw_pixel(right + py, top - px, color);      // top-right
        draw_pixel(left - px,  top - py, color);    draw_pixel(left - py,  top - px, color);      // top-left
        draw_pixel(right + px, bottom + py, color); draw_pixel(right + py, bottom + px, color);   // bottom-right
        draw_pixel(left - px,  bottom + py, color); draw_pixel(left - py,  bottom + px, color);   // bottom-left
        py++;
        if (err < 0) err += 2 * py + 1;
        else { px--; err += 2 * (py - px) + 1; }
    }
}

// Solid rectangle with rounded corners.
void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(w, h, r);

    fill_rect(x, y + r, w, h - 2 * r, color);   // the full-width middle band

    // the rows above and below it get shorter towards the corners
    for (int dy = 1; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;   // same test as fill_circle
        int inset = r - dx;
        draw_hline(x + inset, y + r - dy,         w - 2 * inset, color);
        draw_hline(x + inset, y + h - 1 - r + dy, w - 2 * inset, color);
    }
}

// ============================================================
//  IMAGES — pre-converted RGB565 arrays (same layout as the screen)
// ============================================================
void draw_image(int x, int y, int w, int h, const uint16_t *pixels)
{
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            draw_pixel(x + col, y + row, pixels[row * w + col]);
}

void draw_image_keyed(int x, int y, int w, int h, const uint16_t *pixels, uint16_t transparent)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t c = pixels[row * w + col];
            if (c != transparent) draw_pixel(x + col, y + row, c);
        }
    }
}

// ============================================================
//  TEXT
// ============================================================
// ---- 5x7 ASCII font (characters 32..126) --------------------------------
// One byte per column (5 columns per character), bit 0 = top row, bit 7 = the
// descender row (g j p q y ,). Used by draw_char() / draw_text().
static const uint8_t k_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14}, // ' ' ! " #
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x56,0x20,0x50}, {0x00,0x08,0x07,0x03,0x00}, // $ % & '
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x2A,0x1C,0x7F,0x1C,0x2A}, {0x08,0x08,0x3E,0x08,0x08}, // ( ) * +
    {0x00,0x80,0x70,0x30,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x00,0x60,0x60,0x00}, {0x20,0x10,0x08,0x04,0x02}, // , - . /
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x72,0x49,0x49,0x49,0x46}, {0x21,0x41,0x49,0x4D,0x33}, // 0 1 2 3
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x31}, {0x41,0x21,0x11,0x09,0x07}, // 4 5 6 7
    {0x36,0x49,0x49,0x49,0x36}, {0x46,0x49,0x49,0x29,0x1E}, {0x00,0x00,0x14,0x00,0x00}, {0x00,0x40,0x34,0x00,0x00}, // 8 9 : ;
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x59,0x09,0x06}, // < = > ?
    {0x3E,0x41,0x5D,0x59,0x4E}, {0x7C,0x12,0x11,0x12,0x7C}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22}, // @ A B C
    {0x7F,0x41,0x41,0x41,0x3E}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x41,0x51,0x73}, // D E F G
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, // H I J K
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x1C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E}, // L M N O
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x26,0x49,0x49,0x49,0x32}, // P Q R S
    {0x03,0x01,0x7F,0x01,0x03}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, // T U V W
    {0x63,0x14,0x08,0x14,0x63}, {0x03,0x04,0x78,0x04,0x03}, {0x61,0x59,0x49,0x4D,0x43}, {0x00,0x7F,0x41,0x41,0x41}, // X Y Z [
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x41,0x7F}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40}, // \ ] ^ _
    {0x00,0x03,0x07,0x08,0x00}, {0x20,0x54,0x54,0x78,0x40}, {0x7F,0x28,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x28}, // ` a b c
    {0x38,0x44,0x44,0x28,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x00,0x08,0x7E,0x09,0x02}, {0x18,0xA4,0xA4,0x9C,0x78}, // d e f g
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x40,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00}, // h i j k
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x78,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, // l m n o
    {0xFC,0x18,0x24,0x24,0x18}, {0x18,0x24,0x24,0x18,0xFC}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x24}, // p q r s
    {0x04,0x04,0x3F,0x44,0x24}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C}, // t u v w
    {0x44,0x28,0x10,0x28,0x44}, {0x4C,0x90,0x90,0x90,0x7C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, // x y z {
    {0x00,0x00,0x77,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x02,0x01,0x02,0x04,0x02},                               // | } ~
};

// One character, top-left at (x,y).
void draw_char(int x, int y, char c, int scale, uint16_t color)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = k_font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 8; row++) {
            if ((g[col] >> row) & 1) fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

int text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;      // 5 px glyph + 1 px gap, minus the trailing gap
}

void draw_text(int x, int y, const char *s, int scale, uint16_t color)
{
    for (int i = 0; s[i]; i++, x += 6 * scale) draw_char(x, y, s[i], scale, color);
}

void draw_num(int x, int y, int value, int scale, uint16_t color)
{
    char buf[12];   // enough for a 32-bit int incl. sign
    snprintf(buf, sizeof buf, "%d", value);
    draw_text(x, y, buf, scale, color);
}
