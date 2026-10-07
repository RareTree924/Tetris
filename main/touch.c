#include "touch.h"
#include "display.h"   // LCD_H_RES / LCD_V_RES
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

// Bus speed is 100 kHz: the ESP32's internal pull-ups are too weak for
// 400 kHz if the board has no external ones, and the GT911 then never answers.
#define TOUCH_I2C_SDA 17
#define TOUCH_I2C_SCL 18
#define TOUCH_RST_PIN 38
#define TOUCH_I2C_HZ  100000
#define TOUCH_I2C_TIMEOUT_MS 50   // i2c_master_* timeouts are plain milliseconds, not ticks
#define GT911_ADDR_1  0x5D   // the two addresses GT911 boards ship with;
#define GT911_ADDR_2  0x14   // we probe both since INT isn't wired to pick one
#define TOUCH_RETRY_MS    1000   // not found at boot: probe again this often...
#define TOUCH_RETRY_COUNT 5      // ...this many times (a slow-booting GT911), then give up
#define TOUCH_FLIP_X 0   // set so touches line up with what display.c draws:
#define TOUCH_FLIP_Y 0   // the top-left of what's drawn reads as (0,0)

// If the GT911 reports nothing new for this long while we think a finger
// is down, treat it as released, so a missed "0 points" report can't leave
// the touch stuck on.
#define TOUCH_STALE_MS 150

static i2c_master_bus_handle_t s_touch_bus;
static i2c_master_dev_handle_t s_touch_dev;
static bool                    s_touch_ok = false; // false until a GT911 answers
static int                     s_touch_max_x = LCD_H_RES, s_touch_max_y = LCD_V_RES; // GT911's own resolution

static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_touch_dev, reg_buf, sizeof(reg_buf), buf, len, TOUCH_I2C_TIMEOUT_MS);
}

static esp_err_t gt911_write_reg(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_touch_dev, buf, sizeof(buf), TOUCH_I2C_TIMEOUT_MS);
}

// Tries to talk to a GT911 at `addr` by reading its product-ID register
// (should read back ASCII "911"). On success leaves s_touch_dev attached.
static bool gt911_probe_addr(uint8_t addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = TOUCH_I2C_HZ,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(s_touch_bus, &dev_cfg, &dev) != ESP_OK) return false;

    uint8_t pid[4] = {0};
    uint8_t reg_buf[2] = {0x81, 0x40}; // GT911 product ID register
    esp_err_t err = i2c_master_transmit_receive(dev, reg_buf, sizeof(reg_buf), pid, sizeof(pid), TOUCH_I2C_TIMEOUT_MS);
    if (err == ESP_OK && pid[0] == '9') {
        s_touch_dev = dev;
        ESP_LOGI(TAG, "GT911 found at 0x%02X (id \"%c%c%c\")", addr, pid[0], pid[1], pid[2]);
        return true;
    }
    i2c_master_bus_rm_device(dev);
    return false;
}

// Pulses the GT911's reset line and probes both addresses.
static bool touch_probe(void)
{
    gpio_set_level(TOUCH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); // GT911 needs >50 ms after reset before it answers

    s_touch_ok = gt911_probe_addr(GT911_ADDR_1) || gt911_probe_addr(GT911_ADDR_2);
    if (!s_touch_ok) {
        ESP_LOGE(TAG, "no GT911 on 0x%02X or 0x%02X — check wiring/pins", GT911_ADDR_1, GT911_ADDR_2);
        return false;
    }
    // The GT911 reports in its OWN configured resolution (config
    // registers 0x8048..0x804B); scale from that instead of assuming 480.
    uint8_t res[4];
    if (gt911_read_reg(0x8048, res, sizeof(res)) == ESP_OK) {
        int mx = res[0] | (res[1] << 8), my = res[2] | (res[3] << 8);
        if (mx > 0 && mx <= 4096 && my > 0 && my <= 4096) { s_touch_max_x = mx; s_touch_max_y = my; }
    }
    ESP_LOGI(TAG, "panel reports %dx%d", s_touch_max_x, s_touch_max_y);
    return true;
}

bool touch_begin(void)
{
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_RST_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_touch_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        s_touch_bus = NULL;
        return false;
    }
    return touch_probe();
}

// The GT911 only raises "new data ready" at its own report rate, so plenty
// of polls land between two reports. Those polls repeat the LAST known
// state rather than reporting "released" — otherwise a held finger would
// flicker down/up/down.
void touch_read(bool *touched, int *x, int *y)
{
    static bool     s_down = false;
    static int      s_x = 0, s_y = 0;
    static uint32_t s_last_report_ms = 0;
    static uint32_t s_next_retry_ms = TOUCH_RETRY_MS;
    static int      s_retries_left = TOUCH_RETRY_COUNT;

    *touched = false;
    *x = s_x;
    *y = s_y;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (!s_touch_ok) {
        if (s_touch_bus && s_retries_left > 0 && (int32_t)(now - s_next_retry_ms) >= 0) {
            s_retries_left--;
            ESP_LOGI(TAG, "retrying (%d left)", s_retries_left);
            touch_probe();
            s_next_retry_ms = (uint32_t)(esp_timer_get_time() / 1000) + TOUCH_RETRY_MS;
        }
        return;
    }

    uint8_t status = 0;
    if (gt911_read_reg(0x814E, &status, 1) == ESP_OK && (status & 0x80)) { // top bit = "new data ready"
        uint8_t n_points = status & 0x0F;
        if (n_points > 0 && n_points <= 5) {
            // Point 1 starts at 0x814F: [track id][x lo][x hi][y lo][y hi][size lo][size hi]
            uint8_t pt[7];
            if (gt911_read_reg(0x814F, pt, sizeof(pt)) == ESP_OK) {
                int raw_x = (pt[1] | (pt[2] << 8)) * LCD_H_RES / s_touch_max_x;
                int raw_y = (pt[3] | (pt[4] << 8)) * LCD_V_RES / s_touch_max_y;
                s_x = TOUCH_FLIP_X ? (LCD_H_RES - 1) - raw_x : raw_x;
                s_y = TOUCH_FLIP_Y ? (LCD_V_RES - 1) - raw_y : raw_y;
                if (s_x < 0) s_x = 0; else if (s_x >= LCD_H_RES) s_x = LCD_H_RES - 1;
                if (s_y < 0) s_y = 0; else if (s_y >= LCD_V_RES) s_y = LCD_V_RES - 1;
                s_down = true;
            }
        } else {
            s_down = false;   // a fresh report with 0 points = finger lifted
        }
        s_last_report_ms = now;
        gt911_write_reg(0x814E, 0x00); // must clear this so the next poll gets fresh data
    } else if (s_down && (uint32_t)(now - s_last_report_ms) > TOUCH_STALE_MS) {
        s_down = false;
    }

    *touched = s_down;
    *x = s_x;
    *y = s_y;
}

// ---- Simple helpers -----------------------------------------------------
static bool s_cur_down = false, s_prev_down = false;
static int  s_cur_x = 0, s_cur_y = 0;

void touch_update(void)
{
    s_prev_down = s_cur_down;
    touch_read(&s_cur_down, &s_cur_x, &s_cur_y);
}

bool touch_down(void)    { return s_cur_down; }
bool touch_pressed(void) { return s_cur_down && !s_prev_down; }
int  touch_x(void)       { return s_cur_x; }
int  touch_y(void)       { return s_cur_y; }

bool touching(int x, int y, int range)
{
    if (!s_cur_down) return false;
    int dx = s_cur_x - x, dy = s_cur_y - y;
    return dx * dx + dy * dy <= range * range;
}

bool tapped(int x, int y, int range) { return touch_pressed() && touching(x, y, range); }

bool touching_rect(int x, int y, int w, int h)
{
    return s_cur_down && s_cur_x >= x && s_cur_x < x + w && s_cur_y >= y && s_cur_y < y + h;
}

bool tapped_rect(int x, int y, int w, int h) { return touch_pressed() && touching_rect(x, y, w, h); }
