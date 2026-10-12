/**
 * Clock face on the 466 circle. The center word is THE CLOCK.
 * Ticks from 12 up to the current minute stay on, a step below white.
 * The hour numeral, the minute-arc endpoint, and the second tick share
 * one brighter white. The second steps once a second; it does not blink.
 */

#include "screens_lvgl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_timer.h"

/* Cool mid-gray. Dark gray (#5C656E) sinks into the black panel;
 * this stays clearly off next to white, and still readable. */
#define DIM_COLOR   0x7D868F
/* Arc from 12 to the minute, short of the endpoint.
 * #808080 is half of white, but this panel is RGB565 and the unlit
 * gray #7D868F already sits on the same code, so the two looked alike.
 * #B0B8C0 is the next step the panel can actually show: above the unlit
 * marks, and still well under the white endpoint. */
#define SPAN_COLOR  0xB0B8C0
/* Hour numeral, minute endpoint, and second. Same white, above the arc. */
#define HI_COLOR    0xFFFFFF
#define LIT_COLOR   HI_COLOR
#define SUN_COLOR   0xFFD54F
#define MOON_COLOR  0x64B5F6
/* Outer tip of every tick. Screen radius is 233. */
#define TICK_OUTER  228
/* Ink center of the numerals on amount_03.jpg, scaled by 466/900.
 * Digit boxes there are ~45px tall at radius ~313; font 34 draws 24px. */
#define NUM_R       162
#define TICK_MIN_H  12
#define TICK_HOUR_H 16

static void polar(int minute, int radius, int *x, int *y)
{
    float rad = (float)minute * 0.1047197551f; /* 6 degrees */
    *x = (int)lroundf(sinf(rad) * (float)radius);
    *y = (int)lroundf(-cosf(rad) * (float)radius);
}

/* Official marks are about 4px wide; the numeral marks are a little heavier. */
#define TICK_MIN_W  4
#define TICK_HOUR_W 5

static uint8_t s_min_mask[TICK_MIN_W * TICK_MIN_H];
static uint8_t s_hour_mask[TICK_HOUR_W * TICK_HOUR_H];
static lv_image_dsc_t s_min_img;
static lv_image_dsc_t s_hour_img;

static void fill_capsule(uint8_t *dst, int w, int h)
{
    float cx = (w - 1) * 0.5f;
    float rad = w * 0.5f;
    float y0 = rad - 0.5f;
    float y1 = (h - 1) - (rad - 0.5f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float py = (float)y;
            float cy = py < y0 ? y0 : (py > y1 ? y1 : py);
            float dx = (float)x - cx;
            float dy = py - cy;
            float d = sqrtf(dx * dx + dy * dy);
            int a = (int)((rad - d) * 255.0f + 80.0f);
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            dst[y * w + x] = (uint8_t)a;
        }
    }
}

static void init_tick_image(lv_image_dsc_t *img, const uint8_t *mask, int w, int h)
{
    memset(img, 0, sizeof(*img));
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_A8;
    img->header.w = w;
    img->header.h = h;
    img->header.stride = w;
    img->data_size = (uint32_t)(w * h);
    img->data = mask;
}

static void ensure_tick_masks(void)
{
    static bool ready;
    if (ready) return;
    fill_capsule(s_min_mask, TICK_MIN_W, TICK_MIN_H);
    fill_capsule(s_hour_mask, TICK_HOUR_W, TICK_HOUR_H);
    init_tick_image(&s_min_img, s_min_mask, TICK_MIN_W, TICK_MIN_H);
    init_tick_image(&s_hour_img, s_hour_mask, TICK_HOUR_W, TICK_HOUR_H);
    ready = true;
}

/* Same placement lv_obj_align(CENTER) used: outer tips meet at TICK_OUTER. */
static void tick_box(lv_obj_t *panel, int minute, lv_area_t *area, int *w, int *h)
{
    *h = (minute % 5) == 0 ? TICK_HOUR_H : TICK_MIN_H;
    *w = (minute % 5) == 0 ? TICK_HOUR_W : TICK_MIN_W;
    int ox, oy;
    polar(minute, TICK_OUTER - (*h / 2), &ox, &oy);
    lv_area_t panel_area;
    lv_obj_get_coords(panel, &panel_area);
    int pw = lv_area_get_width(&panel_area);
    int ph = lv_area_get_height(&panel_area);
    area->x1 = panel_area.x1 + (pw - *w) / 2 + ox;
    area->y1 = panel_area.y1 + (ph - *h) / 2 + oy;
    area->x2 = area->x1 + *w - 1;
    area->y2 = area->y1 + *h - 1;
}

static void tick_bounds(const lv_area_t *area, lv_area_t *bounds)
{
    int cx = (area->x1 + area->x2) / 2;
    int cy = (area->y1 + area->y2) / 2;
    int reach = lv_area_get_height(area);
    if (reach < lv_area_get_width(area)) reach = lv_area_get_width(area);
    reach = reach / 2 + 3;
    bounds->x1 = cx - reach;
    bounds->y1 = cy - reach;
    bounds->x2 = cx + reach;
    bounds->y2 = cy + reach;
}

static void invalidate_tick(screen_clock_t *s, int minute)
{
    if (!s->screen || minute < 0 || minute >= 60) return;
    if (lv_obj_get_width(s->screen) <= 0) return;
    int w, h;
    lv_area_t box, bounds;
    tick_box(s->screen, minute, &box, &w, &h);
    tick_bounds(&box, &bounds);
    lv_obj_invalidate_area(s->screen, &bounds);
}

static void invalidate_ring(screen_clock_t *s)
{
    if (!s->screen || lv_obj_get_width(s->screen) <= 0) return;
    lv_area_t panel_area;
    lv_obj_get_coords(s->screen, &panel_area);
    int cx = (panel_area.x1 + panel_area.x2) / 2;
    int cy = (panel_area.y1 + panel_area.y2) / 2;
    lv_area_t ring = {
        cx - (TICK_OUTER + 4), cy - (TICK_OUTER + 4),
        cx + (TICK_OUTER + 4), cy + (TICK_OUTER + 4)
    };
    lv_obj_invalidate_area(s->screen, &ring);
}

/* Endpoint and second share HI. The arc behind the endpoint is SPAN. */
static uint32_t tick_color(const screen_clock_t *s, int mark)
{
    if (s->last_second == mark) return HI_COLOR;
    if (s->last_minute >= 0 && mark == s->last_minute) return HI_COLOR;
    if (s->last_minute >= 0 && mark < s->last_minute) return SPAN_COLOR;
    return DIM_COLOR;
}

static void draw_mark(lv_layer_t *layer, const lv_image_dsc_t *img, const lv_area_t *box,
                      int w, int h, int minute, uint32_t color)
{
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = img;
    dsc.rotation = minute * 60;
    dsc.pivot.x = w / 2;
    dsc.pivot.y = h / 2;
    dsc.recolor = lv_color_hex(color);
    dsc.recolor_opa = LV_OPA_COVER;
    dsc.antialias = 1;
    lv_draw_image(layer, &dsc, box);
}

static void draw_ticks(lv_event_t *e)
{
    screen_clock_t *s = lv_event_get_user_data(e);
    lv_obj_t *panel = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!s || !panel || !layer) return;
    ensure_tick_masks();

    for (int minute = 0; minute < 60; minute++) {
        int w, h;
        lv_area_t box;
        tick_box(panel, minute, &box, &w, &h);
        draw_mark(layer, (minute % 5) == 0 ? &s_hour_img : &s_min_img, &box, w, h, minute,
                  tick_color(s, minute));
    }
}

/* One horizontal run of the icon, in pixels from the screen center.
 * Masks are the official face scaled onto the 466 circle. */
static lv_obj_t *icon_run(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, 1);
    lv_obj_align(o, LV_ALIGN_CENTER, x + w / 2, y);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(DIM_COLOR), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void add_runs(lv_obj_t *parent, lv_obj_t **dst, uint8_t *n, uint8_t cap,
                     const int8_t *runs, int count)
{
    for (int i = 0; i < count && *n < cap; i++) {
        const int8_t *r = runs + i * 3;
        dst[*n] = icon_run(parent, r[0], r[1], r[2]);
        (*n)++;
    }
}

/* Crescent, 21x21, opening to the left. Top-left of the mask is (-9, -83). */
static const int8_t MOON_RUNS[] = {
    3, -83, 3,
    4, -82, 3,
    4, -81, 4,
    5, -80, 4,
    5, -79, 5,
    5, -78, 6,
    5, -77, 6,
    5, -76, 6,
    5, -75, 7,
    5, -74, 7,
    5, -73, 7,
    4, -72, 8,
    3, -71, 9,
    3, -70, 8,
    2, -69, 9,
    -1, -68, 11,
    -9, -67, 19,
    -8, -66, 17,
    -7, -65, 15,
    -6, -64, 13,
    -4, -63, 9,
};

/* Disc plus eight rays, 26x25. Top-left of the mask is (-12, 62). */
static const int8_t SUN_RUNS[] = {
    -1, 62, 4,
    -1, 63, 4,
    -1, 64, 4,
    -9, 65, 3,  -1, 65, 4,  8, 65, 3,
    -9, 66, 4,  -1, 66, 4,  7, 66, 4,
    -8, 67, 4,  6, 67, 4,
    -7, 68, 3,  6, 68, 3,
    -2, 69, 6,
    -3, 70, 8,
    -4, 71, 10,
    -5, 72, 12,
    -12, 73, 5,  -5, 73, 12,  9, 73, 5,
    -12, 74, 5,  -5, 74, 12,  9, 74, 5,
    -12, 75, 5,  -5, 75, 12,  9, 75, 5,
    -5, 76, 12,
    -4, 77, 10,
    -3, 78, 8,
    -2, 79, 6,
    -7, 80, 3,  6, 80, 3,
    -8, 81, 4,  6, 81, 4,
    -9, 82, 4,  -1, 82, 4,  7, 82, 4,
    -9, 83, 3,  -1, 83, 4,  8, 83, 3,
    -1, 84, 4,
    -1, 85, 4,
    -1, 86, 4,
};

static void draw_moon(screen_clock_t *s)
{
    s->moon_n = 0;
    add_runs(s->screen, s->moon_part, &s->moon_n, 24,
             MOON_RUNS, (int)(sizeof(MOON_RUNS) / 3));
}

static void draw_sun(screen_clock_t *s)
{
    s->sun_n = 0;
    add_runs(s->screen, s->sun_part, &s->sun_n, 48,
             SUN_RUNS, (int)(sizeof(SUN_RUNS) / 3));
}

static void paint_sky(screen_clock_t *s, int day)
{
    uint32_t sun = (day == 1) ? SUN_COLOR : DIM_COLOR;
    uint32_t moon = (day == 0) ? MOON_COLOR : DIM_COLOR;
    for (int i = 0; i < s->moon_n; i++) {
        lv_obj_set_style_bg_color(s->moon_part[i], lv_color_hex(moon), 0);
    }
    for (int i = 0; i < s->sun_n; i++) {
        lv_obj_set_style_bg_color(s->sun_part[i], lv_color_hex(sun), 0);
    }
}

static void paint_minute_span(screen_clock_t *s, int minute)
{
    s->last_minute = (int16_t)minute;
    s->last_blink = -1;
    invalidate_ring(s);
}

/* Only the second mark changes. The minute color underneath is left as drawn. */
static void show_second(screen_clock_t *s, int second)
{
    int prev = s->last_second;
    if (prev == second) return;
    s->last_second = (int16_t)second;
    if (prev >= 0 && prev != second) invalidate_tick(s, prev);
    invalidate_tick(s, second);
}

screen_clock_t *screen_clock_create(lv_obj_t *parent)
{
    screen_clock_t *s = malloc(sizeof(screen_clock_t));
    if (!s || !parent) {
        free(s);
        return NULL;
    }
    s->last_sec = SCREEN_VALUE_SENTINEL;
    s->last_hour = -1;
    s->last_minute = -1;
    s->last_second = -1;
    s->last_day = -1;
    s->last_blink = -1;

    s->screen = screen_panel_create(parent);
    ensure_tick_masks();
    lv_obj_add_event_cb(s->screen, draw_ticks, LV_EVENT_DRAW_MAIN_END, s);

    static const char *nums[] = {
        "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"
    };
    for (int i = 0; i < 12; i++) {
        int ox, oy;
        polar(i * 5, NUM_R, &ox, &oy);
        lv_obj_t *label = lv_label_create(s->screen);
        lv_label_set_text(label, nums[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_34, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(DIM_COLOR), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, ox, oy);
        s->hour_label[i] = label;
    }

    draw_moon(s);
    draw_sun(s);

    lv_obj_t *brand = lv_label_create(s->screen);
    lv_label_set_text(brand, "THE CLOCK");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(LIT_COLOR), 0);
    lv_obj_set_style_text_letter_space(brand, 1, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -2);

    screen_pass_swipe(s->screen);
    return s;
}

void screen_clock_update(screen_clock_t *s, const pc_stats_t *stats)
{
    if (!s || !stats) return;

    if (stats->clock_sec < 0) {
        /* Sentinel and the explicit "no clock" value are both negative.
         * Repainting every mark here used to dirty the whole dial on boot. */
        if (s->last_sec < 0) return;
        if (s->last_hour >= 0 && s->last_hour < 12) {
            lv_obj_set_style_text_color(s->hour_label[s->last_hour], lv_color_hex(DIM_COLOR), 0);
        }
        paint_minute_span(s, -1);
        paint_sky(s, -1);
        s->last_sec = -1;
        s->last_hour = -1;
        s->last_minute = -1;
        s->last_second = -1;
        s->last_day = -1;
        s->last_blink = -1;
        return;
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int64_t total_ms = (int64_t)stats->clock_sec * 1000 + (int64_t)(now - stats->clock_sync_ms);
    if (total_ms >= 86400000) total_ms %= 86400000;
    if (total_ms < 0) total_ms = 0;
    int32_t sec = (int32_t)(total_ms / 1000);

    int hour24 = sec / 3600;
    int hour = hour24 % 12;
    int minute = (sec / 60) % 60;
    int second = sec % 60;
    int now_min = sec / 60;
    int rise = stats->sun_rise_min;
    int set = stats->sun_set_min;
    int day;
    if (rise >= 0 && set > rise && set <= 24 * 60) {
        day = (now_min >= rise && now_min < set) ? 1 : 0;
    } else {
        day = (hour24 >= 6 && hour24 < 18) ? 1 : 0;
    }
    if (sec != s->last_sec) {
        if (hour != s->last_hour) {
            if (s->last_hour >= 0 && s->last_hour < 12) {
                lv_obj_set_style_text_color(s->hour_label[s->last_hour], lv_color_hex(DIM_COLOR), 0);
            }
            lv_obj_set_style_text_color(s->hour_label[hour], lv_color_hex(HI_COLOR), 0);
            s->last_hour = (int16_t)hour;
        }
        if (minute != s->last_minute) {
            paint_minute_span(s, minute);
        }
        if (day != s->last_day) {
            paint_sky(s, day);
            s->last_day = (int8_t)day;
        }
        s->last_sec = sec;
    }
    show_second(s, second);
}
