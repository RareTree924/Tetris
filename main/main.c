// Tetris — starting point. display.c / audio.c / touch.c are the hardware
// basics. This file has the grid: draw to any cell, read any cell, and
// spawn cells that fall from the top. Tap a column of the grid to drop one.
#include "display.h"
#include "audio.h"
#include "touch.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GRID_COLS  20
#define GRID_ROWS  30
#define CELL       16                      // 30 rows x 16 px = 480 px, the full screen height
#define GRID_X     16                      // left edge on screen
#define GRID_Y     0                       // top edge
#define GRID_W     (GRID_COLS * CELL)      // 320 px
#define GRID_H     (GRID_ROWS * CELL)      // 480 px
#define COLOR_GRID COLOR_RGB565(0, 30 ,100)   // dark grey lines

#define FALL_MS    500                     // a falling cell drops one row this often
#define MAX_CELLS  32                      // most cells that can be falling at once

// ============================================================
//  DRAWING — pixels only, knows nothing about what's in the grid
// ============================================================

// Draws the grid lines. Call once after fill_screen().
static void draw_grid(void)
{
    for (int c = 0; c <= GRID_COLS; c++)
        draw_vline(GRID_X + c * CELL, GRID_Y, GRID_H, COLOR_GRID);
    for (int r = 0; r <= GRID_ROWS; r++)
        draw_hline(GRID_X, GRID_Y + r * CELL, GRID_W + 1, COLOR_GRID);
}

// Darkens an RGB565 colour: pct 100 = unchanged, 0 = black.
static uint16_t dim565(uint16_t c, int pct)
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return (uint16_t)(((r * pct / 100) << 11) | ((g * pct / 100) << 5) | (b * pct / 100));
}

// Lightens an RGB565 colour towards white: pct 0 = unchanged, 100 = white.
static uint16_t whiten565(uint16_t c, int pct)
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r += (31 - r) * pct / 100;
    g += (63 - g) * pct / 100;
    b += (31 - b) * pct / 100;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// One s x s neon block: a white-hot tube on the edge, full colour just
// inside it, fading to a dark tinted centre.
static void draw_neon_block(int x, int y, int s, uint16_t color)
{
    int r = s / 5;                                                 // corner radius scales with size
    fill_round_rect(x, y, s, s, r, dim565(color, 20));             // dark glassy inside
    draw_round_rect(x, y, s, s, r, whiten565(color, 45));          // hot outer tube
    draw_round_rect(x + 1, y + 1, s - 2, s - 2, r - 1, color);     // full-colour glow
    if (s >= 11)
        draw_round_rect(x + 2, y + 2, s - 4, s - 4, r - 2, dim565(color, 50));   // glow fading inward
}

// Colours one cell. (0,0) is the bottom-left cell: col 0..19 goes right,
// row 0..29 goes up. Fill with COLOR_BLACK to empty it again; the grid lines are left alone.
static void fill_cell(int col, int row, uint16_t color)
{
    if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) return;   // ignore cells off the grid
    int screen_row = GRID_ROWS - 1 - row;   // row 0 is the bottom line on screen
    int x = GRID_X + col * CELL + 1, y = GRID_Y + screen_row * CELL + 1;
    if (color == COLOR_BLACK)
        fill_rect(x, y, CELL - 1, CELL - 1, COLOR_BLACK);   // empty
    else
        draw_neon_block(x, y, CELL - 1, color);
}

// ============================================================
//  THE GRID — remembers the colour of every cell
// ============================================================

// grid[y][x] is the colour of cell (x, y). COLOR_BLACK (0) means empty,
// so a global array like this starts out all empty.
static uint16_t grid[GRID_ROWS][GRID_COLS];

static bool on_grid(int x, int y)
{
    return x >= 0 && x < GRID_COLS && y >= 0 && y < GRID_ROWS;
}

// Draw to a cell: remember the colour and put it on screen.
// set_cell(x, y, COLOR_BLACK) empties it.
static void set_cell(int x, int y, uint16_t color)
{
    if (!on_grid(x, y)) return;
    grid[y][x] = color;
    fill_cell(x, y, color);
}

// Read a cell: its colour, or COLOR_BLACK if it's empty or off the grid.
static uint16_t get_cell(int x, int y)
{
    if (!on_grid(x, y)) return COLOR_BLACK;
    return grid[y][x];
}

// True if something can move into (x, y). Off the grid counts as a wall,
// so the floor stops falling cells.
static bool cell_empty(int x, int y)
{
    return on_grid(x, y) && grid[y][x] == COLOR_BLACK;
}

// ============================================================
//  FALLING CELLS
// ============================================================

typedef struct {
    int x, y;
    uint16_t color;
    bool active;     // true while it's still falling
} Cell;

static Cell cells[MAX_CELLS];

// Drops a new cell into column x at the top row. Returns false if it
// couldn't (top cell already full, or MAX_CELLS already falling).
static bool spawn_cell(int x, uint16_t color)
{
    int top = GRID_ROWS - 1;
    if (!cell_empty(x, top)) return false;

    for (int i = 0; i < MAX_CELLS; i++) {
        if (cells[i].active) continue;          // slot in use, try the next
        cells[i] = (Cell){ .x = x, .y = top, .color = color, .active = true };
        set_cell(x, top, color);
        return true;
    }
    return false;
}

// Moves every falling cell down one row. A cell that can't move has
// landed: it stays in the grid and stops falling.
static void fall_step(void)
{
    // Lowest cells first, so a cell sitting on another falling cell
    // sees the gap that one leaves instead of landing on it.
    for (int y = 0; y < GRID_ROWS; y++) {
        for (int i = 0; i < MAX_CELLS; i++) {
            Cell *c = &cells[i];
            if (!c->active || c->y != y) continue;

            if (cell_empty(c->x, c->y - 1)) {
                set_cell(c->x, c->y, COLOR_BLACK);   // clear the old spot
                c->y--;
                set_cell(c->x, c->y, c->color);      // draw the new one
            } else {
                c->active = false;                   // landed
            }
        }
    }
}
#define MAX_SHAPE  4                       // biggest shape is 4x4

typedef struct {
    uint8_t cells[MAX_SHAPE * MAX_SHAPE];  // RAM copy of the shape (flash is read-only, and rotating rewrites it)
    int w, h;              // shape size in cells
    int x, y;              // grid cell under the shape's top-left corner
    uint16_t color;
    bool active;           // true while it's falling
    const uint8_t *src;    // the original shape in flash, unturned...
    int src_w, src_h;      // ...and its size
    int rot;               // how many quarter turns clockwise from src (0..3)
} Piece;

static Piece piece;   // the one falling piece

// True if the shape has a block at (row, col) of its array.
static bool piece_has(const Piece *p, int row, int col)
{
    return p->cells[row * p->w + col] != 0;
}
// Colours every block of the piece at its current spot. COLOR_BLACK erases it.
static void draw_piece(const Piece *p, uint16_t color)
{
    for (int r = 0; r < p->h; r++)
        for (int c = 0; c < p->w; c++)
            if (piece_has(p, r, c))
                set_cell(p->x + c, p->y - r, color);   // array rows go down, grid rows go up
}
// True if every block would sit on an empty cell with the top-left corner
// at (x, y). Erase the piece first, or it bumps into itself.
static bool piece_fits(const Piece *p, int x, int y)
{
    for (int r = 0; r < p->h; r++)
        for (int c = 0; c < p->w; c++)
            if (piece_has(p, r, c) && !cell_empty(x + c, y - r))
                return false;
    return true;
}
// Drops a new piece with its left edge at column x. Returns false if it
// doesn't fit at the top (that's game over in real Tetris).
static bool spawn_piece(const uint8_t *shape, int w, int h, int x, uint16_t color)
{
    if (w > MAX_SHAPE || h > MAX_SHAPE) return false;
    if (x > GRID_COLS - w) x = GRID_COLS - w;   // keep the whole shape on the grid
    if (x < 0) x = 0;
    piece = (Piece){ .w = w, .h = h, .x = x, .y = GRID_ROWS - 1, .color = color, .active = true,
                     .src = shape, .src_w = w, .src_h = h, .rot = 0 };
    memcpy(piece.cells, shape, w * h);          // copy the shape out of flash
    if (!piece_fits(&piece, piece.x, piece.y)) { piece.active = false; return false; }
    draw_piece(&piece, color);
    return true;
}

// Moves the piece down one row, or lands it if it can't move.
static void piece_fall(void)
{
    if (!piece.active) return;
    draw_piece(&piece, COLOR_BLACK);              // lift it off the grid
    if (piece_fits(&piece, piece.x, piece.y - 1))
        piece.y--;
    else
        piece.active = false;                     // landed: it stays in the grid
    draw_piece(&piece, piece.color);              // put it back (one row lower, or where it was)
}

// Sets out's cells/w/h to p's original shape turned `rot` quarter turns clockwise.
static void shape_turned(const Piece *p, int rot, Piece *out)
{
    out->w = p->src_w;
    out->h = p->src_h;
    memcpy(out->cells, p->src, out->w * out->h);

    for (int i = 0; i < rot; i++) {
        uint8_t old[MAX_SHAPE * MAX_SHAPE];
        int ow = out->w, oh = out->h;
        memcpy(old, out->cells, sizeof old);
        out->w = oh;                              // a 2x3 shape becomes 3x2
        out->h = ow;
        for (int r = 0; r < out->h; r++)
            for (int c = 0; c < out->w; c++)
                out->cells[r * out->w + c] = old[(oh - 1 - c) * ow + r];
    }
}

// Snaps the piece straight to orientation `rot` (0..3) if it has room. Against
// a wall or a block it also tries nudging 1 or 2 columns sideways ("wall kick").
static void piece_set_rotation(int rot)
{
    if (!piece.active || rot == piece.rot) return;

    Piece turned = piece;
    shape_turned(&piece, rot, &turned);
    turned.rot = rot;

    static const int kicks[] = { 0, -1, 1, -2, 2 };
    draw_piece(&piece, COLOR_BLACK);              // lift it off the grid
    for (int i = 0; i < (int)(sizeof kicks / sizeof kicks[0]); i++) {
        if (piece_fits(&turned, piece.x + kicks[i], piece.y)) {
            turned.x = piece.x + kicks[i];
            piece = turned;
            break;
        }
    }
    draw_piece(&piece, piece.color);              // turned, or unchanged if nothing fit
}

// ============================================================
//  ROTATE BUTTON — neon on dark blue, bottom-right corner
// ============================================================

#define ROT_X  352
#define ROT_Y  364
#define ROT_W  116
#define ROT_H  104
#define ROT_R  14                                 // corner radius

#define NEON_CORE   COLOR_RGB565(120, 240, 255)   // hot centre of the glow
#define NEON_CYAN   COLOR_RGB565(0, 200, 255)
#define NEON_BLUE   COLOR_RGB565(0, 110, 255)
#define NEON_DIM    COLOR_RGB565(0, 50, 150)
#define NEON_FAINT  COLOR_RGB565(0, 25, 80)
#define NAVY        COLOR_RGB565(0, 8, 32)
#define NAVY_LIT    COLOR_RGB565(0, 30, 90)       // behind the orientation you're in

// The panel is split 2x2. Each quarter previews one orientation; tap it to snap to it.
#define QUAD_W  ((ROT_W - 4) / 2)                 // 56 px
#define QUAD_H  ((ROT_H - 4) / 2)                 // 50 px
#define MINI    8                                 // preview block size in px

// Which orientation each quarter shows, in the order top-left, top-right,
// bottom-left, bottom-right. Going clockwise round the panel: 0, 90, 180, 270.
static const int quad_rot[4] = { 0, 1, 3, 2 };

static int quad_x(int q) { return ROT_X + 2 + (q % 2) * QUAD_W; }
static int quad_y(int q) { return ROT_Y + 2 + (q / 2) * QUAD_H; }

// Draws the piece turned `rot` times, small and centred in quarter q.
static void draw_preview(int q, int rot)
{
    Piece turned = piece;
    shape_turned(&piece, rot, &turned);

    // find the blocks' bounding box, so the empty rows/columns of the array don't push it off-centre
    int rmin = MAX_SHAPE, rmax = -1, cmin = MAX_SHAPE, cmax = -1;
    for (int r = 0; r < turned.h; r++)
        for (int c = 0; c < turned.w; c++)
            if (piece_has(&turned, r, c)) {
                if (r < rmin) rmin = r;
                if (r > rmax) rmax = r;
                if (c < cmin) cmin = c;
                if (c > cmax) cmax = c;
            }
    if (rmax < 0) return;   // empty shape

    int ox = quad_x(q) + (QUAD_W - (cmax - cmin + 1) * MINI) / 2;
    int oy = quad_y(q) + (QUAD_H - (rmax - rmin + 1) * MINI) / 2;
    for (int r = rmin; r <= rmax; r++)
        for (int c = cmin; c <= cmax; c++)
            if (piece_has(&turned, r, c))
                draw_neon_block(ox + (c - cmin) * MINI, oy + (r - rmin) * MINI, MINI - 1, piece.color);
}

// A glowing neon box with a navy inside. The glow spills 3 px outside x/y/w/h.
static void draw_neon_frame(int x, int y, int w, int h, int r)
{
    // glow: rings that fade out the further they are from the edge
    draw_round_rect(x - 3, y - 3, w + 6, h + 6, r + 3, NEON_FAINT);
    draw_round_rect(x - 2, y - 2, w + 4, h + 4, r + 2, NEON_DIM);
    draw_round_rect(x - 1, y - 1, w + 2, h + 2, r + 1, NEON_BLUE);
    draw_round_rect(x,     y,     w,     h,     r,     NEON_CYAN);
    draw_round_rect(x + 1, y + 1, w - 2, h - 2, r - 1, NEON_BLUE);
    fill_round_rect(x + 2, y + 2, w - 4, h - 4, r - 2, NAVY);
}

// Draws the whole panel: neon frame, 4 quarters, and the piece's 4 orientations.
// With no piece falling the quarters are left empty.
static void draw_rotate_panel(void)
{
    draw_neon_frame(ROT_X, ROT_Y, ROT_W, ROT_H, ROT_R);

    // dividers between the quarters
    draw_vline(ROT_X + 2 + QUAD_W, ROT_Y + 8, ROT_H - 16, NEON_DIM);
    draw_hline(ROT_X + 8, ROT_Y + 2 + QUAD_H, ROT_W - 16, NEON_DIM);

    if (!piece.active) return;

    for (int q = 0; q < 4; q++) {
        if (quad_rot[q] == piece.rot) {   // light up the one it's in now
            fill_round_rect(quad_x(q) + 3, quad_y(q) + 3, QUAD_W - 6, QUAD_H - 6, 6, NAVY_LIT);
            draw_round_rect(quad_x(q) + 3, quad_y(q) + 3, QUAD_W - 6, QUAD_H - 6, 6, NEON_CYAN);
        }
        draw_preview(q, quad_rot[q]);
    }
}

// Redraws the panel only when what it shows has changed: a new piece,
// a new orientation, or the piece landing. Call once per frame.
static void update_rotate_panel(void)
{
    static bool drawn = false, was_active;
    static const uint8_t *was_src;
    static int was_rot;
    static uint16_t was_color;

    if (drawn && piece.active == was_active && piece.src == was_src &&
        piece.rot == was_rot && piece.color == was_color)
        return;

    draw_rotate_panel();
    drawn = true;
    was_active = piece.active;
    was_src = piece.src;
    was_rot = piece.rot;
    was_color = piece.color;
}

// Snaps the piece to whichever quarter was just tapped.
static void check_rotate_panel(void)
{
    for (int q = 0; q < 4; q++)
        if (tapped_rect(quad_x(q), quad_y(q), QUAD_W, QUAD_H))
            piece_set_rotation(quad_rot[q]);
}

// ============================================================
//  SPEED SLIDER — neon, top-right corner. Drag to set 0.1x .. 5x.
// ============================================================

#define SPD_X    352
#define SPD_Y    12
#define SPD_W    116
#define SPD_H    64
#define SPD_R    12

#define SPD_MIN  0.1f
#define SPD_MAX  5.0f

#define TRACK_X0 (SPD_X + 14)                     // left end of the track = 0.1x
#define TRACK_X1 (SPD_X + SPD_W - 15)             // right end = 5x
#define TRACK_Y  (SPD_Y + 44)
#define KNOB_R   7

// Game speed multiplier: pieces drop one row every FALL_MS / speed ms.
static float speed = 5.0f;

// The track is logarithmic: every step along it multiplies the speed by the
// same amount, so 0.1x..1x gets as much room as 1x..5x would need.
// pos 0 = left end of the track, 1 = right end.
static float speed_to_pos(float s) { return logf(s / SPD_MIN) / logf(SPD_MAX / SPD_MIN); }
static float pos_to_speed(float p) { return SPD_MIN * expf(p * logf(SPD_MAX / SPD_MIN)); }

static int pos_to_x(float p) { return TRACK_X0 + (int)lroundf(p * (TRACK_X1 - TRACK_X0)); }

static void draw_speed_slider(void)
{
    draw_neon_frame(SPD_X, SPD_Y, SPD_W, SPD_H, SPD_R);

    // title on the left, value on the right, e.g. "SPEED   1.5x"
    draw_text(SPD_X + 12, SPD_Y + 15, "SPEED", 1, NEON_CYAN);
    char buf[24];                                // roomy, so the compiler knows any int fits
    int tenths = (int)lroundf(speed * 10);       // whole tenths, so no float printf needed
    snprintf(buf, sizeof buf, "%d.%dx", tenths / 10, tenths % 10);
    int vx = SPD_X + SPD_W - 12 - text_width(buf, 2);
    draw_text(vx + 1, SPD_Y + 11, buf, 2, NEON_DIM);   // soft glow behind
    draw_text(vx,     SPD_Y + 10, buf, 2, NEON_CORE);

    // little tick under the track at 1x
    draw_vline(pos_to_x(speed_to_pos(1.0f)), TRACK_Y + 5, 4, NEON_DIM);

    // track: dim all the way, lit from the left end up to the knob
    int kx = pos_to_x(speed_to_pos(speed));
    fill_round_rect(TRACK_X0 - 2, TRACK_Y - 2, TRACK_X1 - TRACK_X0 + 5, 5, 2, NEON_FAINT);
    fill_round_rect(TRACK_X0 - 2, TRACK_Y - 2, kx - TRACK_X0 + 5, 5, 2, NEON_BLUE);
    draw_hline(TRACK_X0, TRACK_Y, kx - TRACK_X0, NEON_CORE);

    // knob: glowing ring with a hot dot in the middle
    draw_circle(kx, TRACK_Y, KNOB_R + 2, NEON_FAINT);
    draw_circle(kx, TRACK_Y, KNOB_R + 1, NEON_DIM);
    fill_circle(kx, TRACK_Y, KNOB_R, NAVY_LIT);
    draw_circle(kx, TRACK_Y, KNOB_R, NEON_CYAN);
    fill_circle(kx, TRACK_Y, 2, NEON_CORE);
}

// While a finger is on the slider panel, the knob follows it.
static void check_speed_slider(void)
{
    if (!touching_rect(SPD_X, SPD_Y, SPD_W, SPD_H)) return;

    float p = (float)(touch_x() - TRACK_X0) / (TRACK_X1 - TRACK_X0);
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    float s = roundf(pos_to_speed(p) * 10) / 10;  // snap to 0.1 steps
    if (s < SPD_MIN) s = SPD_MIN;

    if (s == speed) return;                       // same as before: nothing to redraw
    speed = s;
    draw_speed_slider();
}

// ============================================================
//  SHAPES — stored in flash. 1 = block, 0 = empty.
//  Row 0 of the array is the TOP of the shape, the way you'd draw it.
// ============================================================

static const uint8_t SHAPE_I[4][4] = {
    {0,0,0,0},
    {1,1,1,1},
    {0,0,0,0},
    {0,0,0,0},
};
static const uint8_t SHAPE_O[2][2] = {
    {1,1},
    {1,1},
};
static const uint8_t SHAPE_T[3][3] = {
    {1,1,1},
    {0,1,0},
    {0,0,0},
};
static const uint8_t SHAPE_S[3][3] = {
    {0,1,1},
    {1,1,0},
    {0,0,0},
};
static const uint8_t SHAPE_Z[3][3] = {
    {1,1,0},
    {0,1,1},
    {0,0,0},
};
static const uint8_t SHAPE_J[3][3] = {
    {0,0,1},
    {0,0,1},
    {0,1,1},
};
static const uint8_t SHAPE_L[3][3] = {
    {1,0,0},
    {1,0,0},
    {1,1,0},
};

// ---- extras: small pieces ----
static const uint8_t SHAPE_DOT[1][1] = {
    {1},
};
static const uint8_t SHAPE_DOMINO[1][2] = {
    {1,1},
};
static const uint8_t SHAPE_I3[1][3] = {
    {1,1,1},
};
static const uint8_t SHAPE_CORNER[2][2] = {
    {1,0},
    {1,1},
};

// ---- extras: 5-block pieces (pentominoes) ----
static const uint8_t SHAPE_PLUS[3][3] = {
    {0,1,0},
    {1,1,1},
    {0,1,0},
};
static const uint8_t SHAPE_U[2][3] = {
    {1,0,1},
    {1,1,1},
};
static const uint8_t SHAPE_V[3][3] = {
    {1,0,0},
    {1,0,0},
    {1,1,1},
};
static const uint8_t SHAPE_W[3][3] = {
    {1,0,0},
    {1,1,0},
    {0,1,1},
};
static const uint8_t SHAPE_BIG_T[3][3] = {
    {1,1,1},
    {0,1,0},
    {0,1,0},
};
static const uint8_t SHAPE_BIG_Z[3][3] = {
    {1,1,0},
    {0,1,0},
    {0,1,1},
};
static const uint8_t SHAPE_F[3][3] = {
    {0,1,1},
    {1,1,0},
    {0,1,0},
};
static const uint8_t SHAPE_P[3][2] = {
    {1,1},
    {1,1},
    {1,0},
};
static const uint8_t SHAPE_Y[2][4] = {
    {0,1,0,0},
    {1,1,1,1},
};
static const uint8_t SHAPE_N[2][4] = {
    {1,1,0,0},
    {0,1,1,1},
};

// ---- extras: big ones ----
static const uint8_t SHAPE_H[3][3] = {
    {1,0,1},
    {1,1,1},
    {1,0,1},
};
static const uint8_t SHAPE_DONUT[3][3] = {
    {1,1,1},
    {1,0,1},
    {1,1,1},
};

// Turns any 2D shape array into the 3 things spawn_piece() needs:
// a pointer to its first byte, its width and its height.
#define SHAPE(a)  &(a)[0][0], (int)sizeof (a)[0], (int)(sizeof (a) / sizeof (a)[0])

typedef struct {
    const uint8_t *cells;
    int w, h;
} ShapeDef;

// Every shape that can drop. New shape: define its array above, then add a line here.
static const ShapeDef all_shapes[] = {
    { SHAPE(SHAPE_I) },
    { SHAPE(SHAPE_O) },
    { SHAPE(SHAPE_T) },
    { SHAPE(SHAPE_S) },
    { SHAPE(SHAPE_Z) },
    { SHAPE(SHAPE_J) },
    { SHAPE(SHAPE_L) },

    { SHAPE(SHAPE_DOT) },
    { SHAPE(SHAPE_DOMINO) },
    { SHAPE(SHAPE_I3) },
    { SHAPE(SHAPE_CORNER) },

    { SHAPE(SHAPE_PLUS) },
    { SHAPE(SHAPE_U) },
    { SHAPE(SHAPE_V) },
    { SHAPE(SHAPE_W) },
    { SHAPE(SHAPE_BIG_T) },
    { SHAPE(SHAPE_BIG_Z) },
    { SHAPE(SHAPE_F) },
    { SHAPE(SHAPE_P) },
    { SHAPE(SHAPE_Y) },
    { SHAPE(SHAPE_N) },

    { SHAPE(SHAPE_H) },
    { SHAPE(SHAPE_DONUT) },
};
#define NUM_SHAPES  (int)(sizeof all_shapes / sizeof all_shapes[0])
// ============================================================

// Neon palette: bright, saturated, and none of them too close to the dark-blue background.
static const uint16_t spawn_colors[] = {
    COLOR_RGB565(255,  40, 200),   // hot pink
    COLOR_RGB565(  0, 255, 255),   // cyan
    COLOR_RGB565( 60, 255,  60),   // lime
    COLOR_RGB565(255, 240,  40),   // yellow
    COLOR_RGB565(255, 140,   0),   // orange
    COLOR_RGB565(170,  60, 255),   // purple
    COLOR_RGB565( 40, 140, 255),   // electric blue
    COLOR_RGB565(255,  50,  50),   // red
};

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    display_begin();
    touch_begin();
    sd_begin();      // after display_begin(): the panel init bit-bangs pins 11/12, then the SD's SPI bus takes them

    fill_screen(COLOR_BLACK);
    draw_grid();
    draw_speed_slider();

    unsigned long lastFall = 0;
    int nextColor = 0;

    while (1) {
        touch_update();   // once per frame, before any touching()/tapped()

        unsigned long now = (unsigned long)(esp_timer_get_time() / 1000);   // ms since boot

        if (touch_pressed()){
            if (touch_x() >= GRID_X && touch_x() < GRID_X + GRID_W && !piece.active) {
             int col = (touch_x() - GRID_X) / CELL;
             const ShapeDef *s = &all_shapes[esp_random() % NUM_SHAPES];   // pick one at random
             spawn_piece(s->cells, s->w, s->h, col, spawn_colors[nextColor]);
             nextColor = (nextColor + 1) % (sizeof spawn_colors / sizeof spawn_colors[0]);
            }

            printf("X: %d  Y: %d\n", touch_x(), touch_y());
        }

        check_rotate_panel();
        check_speed_slider();

        if (now - lastFall >= (unsigned long)(FALL_MS / speed)) {
          lastFall = now;
          fall_step();
         piece_fall();
        }

        update_rotate_panel();   // after anything that can change the piece


        vTaskDelay(pdMS_TO_TICKS(16));   // ~60 fps
    }
}
