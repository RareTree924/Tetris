#pragma once
// ============================================================
//  TOUCH — GT911 capacitive touch panel over I2C (SDA 17, SCL 18,
//  RST 38, INT not wired, so it's polled). Coordinates are screen
//  pixels, 0..LCD_H_RES-1 / 0..LCD_V_RES-1.
// ============================================================
#include <stdbool.h>

// Brings up I2C and finds the GT911. Call once at boot. Returns false if it
// isn't found; touch_read() then retries a few times by itself.
bool touch_begin(void);

// Polls the panel. *touched = a finger is down right now; x/y = where (the
// last known spot while lifted). Call once per frame.
void touch_read(bool *touched, int *x, int *y);

// ---- Simple helpers -----------------------------------------------------
// Call touch_update() ONCE at the top of every frame, then ask as many
// questions as you like that frame:
//     if (touching(240, 160, 50)) ...   // finger within 50 px of (240,160)
//     if (tapped(240, 160, 50))   ...   // ...and it only just went down
void touch_update(void);
bool touching(int x, int y, int range);           // finger down within range px of (x,y)
bool tapped(int x, int y, int range);             // same, but only on the frame the press starts
bool touching_rect(int x, int y, int w, int h);   // finger down inside a rectangle (for buttons)
bool tapped_rect(int x, int y, int w, int h);
bool touch_down(void);                            // finger down anywhere
bool touch_pressed(void);                         // a press started this frame, anywhere
int  touch_x(void);                               // where the finger is (last spot if lifted)
int  touch_y(void);
