/**
 * Weather page between brightness and CPU.
 * Round 466 layout: city on the title line, icon above center,
 * temperature in the wide middle, condition and humidity above the page dots.
 */

#include "screens_lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

enum {
    ICON_NONE = 0,
    ICON_SUN,
    ICON_PARTLY,
    ICON_CLOUD,
    ICON_RAIN,
    ICON_SNOW,
    ICON_FOG,
    ICON_STORM,
};

static const char *weather_cond(int code)
{
    if (code < 0) return "N/A";
    if (code == 0) return "Sunny";
    if (code == 1) return "Mostly Sunny";
    if (code == 2) return "Partly Cloudy";
    if (code == 3) return "Overcast";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code == 85 || code == 86) return "Snow";
    if (code >= 95 && code <= 99) return "Thunder";
    return "Weather";
}

static int icon_kind(int code)
{
    if (code < 0) return ICON_NONE;
    if (code <= 1) return ICON_SUN;
    if (code == 2) return ICON_PARTLY;
    if (code == 3) return ICON_CLOUD;
    if (code == 45 || code == 48) return ICON_FOG;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return ICON_SNOW;
    if (code >= 95) return ICON_STORM;
    if (code >= 51) return ICON_RAIN;
    return ICON_CLOUD;
}

static lv_obj_t *blob(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    return o;
}

static void draw_sun(lv_obj_t *parent, int cx, int cy, int disc)
{
    const uint32_t color = 0xFFD54F;
    blob(parent, cx - disc / 2, cy - disc / 2, disc, disc, color);
    int ray_l = disc / 2;
    if (ray_l < 8) ray_l = 8;
    if (ray_l > 16) ray_l = 16;
    int ray_w = disc > 30 ? 6 : 4;
    int gap = disc / 2 + 5;
    blob(parent, cx - ray_w / 2, cy - gap - ray_l, ray_w, ray_l, color);
    blob(parent, cx - ray_w / 2, cy + gap, ray_w, ray_l, color);
    blob(parent, cx - gap - ray_l, cy - ray_w / 2, ray_l, ray_w, color);
    blob(parent, cx + gap, cy - ray_w / 2, ray_l, ray_w, color);
    int d = disc > 30 ? 6 : 5;
    int o = (gap * 7) / 10;
    blob(parent, cx + o, cy - o - d, d, d, color);
    blob(parent, cx - o - d, cy - o - d, d, d, color);
    blob(parent, cx + o, cy + o, d, d, color);
    blob(parent, cx - o - d, cy + o, d, d, color);
}

static void draw_cloud(lv_obj_t *parent, int ox, int oy)
{
    const uint32_t color = 0xECEFF1;
    blob(parent, ox + 4, oy + 20, 34, 34, color);
    blob(parent, ox + 24, oy + 6, 42, 42, color);
    blob(parent, ox + 54, oy + 20, 32, 32, color);
    blob(parent, ox + 10, oy + 36, 68, 22, color);
}

static void weather_icon_set(lv_obj_t *icon, int code)
{
    if (!icon) return;
    lv_obj_clean(icon);
    int kind = icon_kind(code);
    if (kind == ICON_NONE) return;

    if (kind == ICON_SUN) {
        draw_sun(icon, 56, 56, 40);
        return;
    }
    if (kind == ICON_PARTLY) {
        draw_sun(icon, 74, 30, 22);
        draw_cloud(icon, 2, 46);
        return;
    }
    if (kind == ICON_FOG) {
        blob(icon, 16, 30, 80, 10, 0xB0BEC5);
        blob(icon, 26, 52, 60, 10, 0x90A4AE);
        blob(icon, 16, 74, 80, 10, 0xB0BEC5);
        return;
    }

    draw_cloud(icon, 12, 4);
    if (kind == ICON_RAIN || kind == ICON_STORM) {
        blob(icon, 26, 74, 6, 16, 0x4FC3F7);
        blob(icon, 50, 80, 6, 18, 0x4FC3F7);
        blob(icon, 74, 74, 6, 16, 0x4FC3F7);
    }
    if (kind == ICON_SNOW) {
        blob(icon, 28, 76, 8, 8, 0xFFFFFF);
        blob(icon, 50, 88, 8, 8, 0xFFFFFF);
        blob(icon, 72, 76, 8, 8, 0xFFFFFF);
    }
    if (kind == ICON_STORM) {
        blob(icon, 48, 70, 16, 10, 0xFFD54F);
        blob(icon, 40, 80, 16, 10, 0xFFD54F);
        blob(icon, 50, 90, 10, 10, 0xFFD54F);
    }
}

screen_weather_t *screen_weather_create(lv_obj_t *parent)
{
    screen_weather_t *s = malloc(sizeof(screen_weather_t));
    if (!s || !parent) {
        free(s);
        return NULL;
    }

    s->last_temp = SCREEN_VALUE_SENTINEL;
    s->last_code = SCREEN_VALUE_SENTINEL;
    s->last_hum = SCREEN_VALUE_SENTINEL;
    s->last_city[0] = '\0';

    s->screen = screen_panel_create(parent);

    /* 466 circle. Stay inside the wide middle: the bezel clips the caps,
     * the status dot sits near y=-156, and the page dots sit near y=+180. */
    s->icon = lv_obj_create(s->screen);
    lv_obj_set_size(s->icon, 112, 112);
    lv_obj_align(s->icon, LV_ALIGN_CENTER, 0, -36);
    lv_obj_set_style_bg_opa(s->icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->icon, 0, 0);
    lv_obj_set_style_pad_all(s->icon, 0, 0);
    lv_obj_remove_flag(s->icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s->icon, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);

    s->label_city = lv_label_create(s->screen);
    lv_obj_set_width(s->label_city, 220);
    lv_obj_align(s->label_city, LV_ALIGN_CENTER, 0, -128);
    lv_label_set_long_mode(s->label_city, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s->label_city, "Weather");
    lv_obj_set_style_text_align(s->label_city, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s->label_city, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s->label_city, lv_color_hex(0x4FC3F7), 0);

    s->label_temp = lv_label_create(s->screen);
    lv_obj_align(s->label_temp, LV_ALIGN_CENTER, 0, 58);
    lv_label_set_text(s->label_temp, "N/A");
    lv_obj_set_style_text_font(s->label_temp, &lv_font_montserrat_42, 0);
    lv_obj_set_style_text_color(s->label_temp, lv_color_white(), 0);

    s->label_cond = lv_label_create(s->screen);
    lv_obj_set_width(s->label_cond, 230);
    lv_obj_align(s->label_cond, LV_ALIGN_CENTER, 0, 108);
    lv_label_set_long_mode(s->label_cond, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s->label_cond, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s->label_cond, "");
    lv_obj_set_style_text_font(s->label_cond, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s->label_cond, lv_color_white(), 0);

    s->label_hum = lv_label_create(s->screen);
    lv_obj_align(s->label_hum, LV_ALIGN_CENTER, 0, 140);
    lv_label_set_text(s->label_hum, "");
    lv_obj_set_style_text_font(s->label_hum, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s->label_hum, lv_color_hex(0x90CAF9), 0);

    screen_pass_swipe(s->screen);
    return s;
}

void screen_weather_update(screen_weather_t *s, const pc_stats_t *stats)
{
    if (!s || !stats) return;
    if (stats->weather_temp == s->last_temp && stats->weather_code == s->last_code &&
        stats->weather_hum == s->last_hum && strcmp(stats->weather_city, s->last_city) == 0) {
        return;
    }
    bool code_changed = stats->weather_code != s->last_code;
    s->last_temp = stats->weather_temp;
    s->last_code = stats->weather_code;
    s->last_hum = stats->weather_hum;
    snprintf(s->last_city, sizeof(s->last_city), "%s", stats->weather_city);

    lv_label_set_text(s->label_city, stats->weather_city[0] ? stats->weather_city : "Weather");

    if (stats->weather_temp < -80 || stats->weather_temp > 60) {
        lv_label_set_text(s->label_temp, "N/A");
        lv_obj_set_style_text_color(s->label_temp, lv_color_hex(0xFF4444), 0);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d°C", stats->weather_temp);
        lv_label_set_text(s->label_temp, buf);
        lv_color_t color = lv_color_white();
        if (stats->weather_temp >= 35) color = lv_color_hex(0xFF4444);
        else if (stats->weather_temp >= 28) color = lv_color_hex(0xFFB74D);
        else if (stats->weather_temp < 0) color = lv_color_hex(0x81D4FA);
        lv_obj_set_style_text_color(s->label_temp, color, 0);
    }

    if (stats->weather_hum < 0 || stats->weather_hum > 100) {
        lv_label_set_text(s->label_hum, "");
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "RH %d%%", stats->weather_hum);
        lv_label_set_text(s->label_hum, buf);
    }
    lv_label_set_text(s->label_cond, weather_cond(stats->weather_code));
    if (code_changed) weather_icon_set(s->icon, stats->weather_code);
}
