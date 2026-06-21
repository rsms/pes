#include "pes.h"
#include "pes.c"
/*
    +-----------------+
    |   OTHER GOAL    |
    |                 |
    |      OTHER      |
    |                 |
    |-------MID-------|
    |                 |
    |     PLAYER      |
    |                 |
    |   PLAYER GOAL   |
    +-----------------+
*/

static struct {
    struct {
        Vec2 pos;
        Vec2 vel;
        f32  radius;
    } puck;

    struct {
        Vec2 pos;
        Vec2 prev_pos;
        Vec2 vel;
        f32  radius;
        u32  score;
    } player[2];

    struct {
        Vec2 pos;           // screen space, already in dp
        f32  width, height; // unscaled, base units
        f32  scale;         // base units to screen space dp
    } scene;

    f32 goal_w;
    f32 goal_depth;
} g = { .scene.width = 192, .scene.height = 256 };

void scene_resize(void) {
    Vec2 screen_margin = { 32, 32 };
    f32  screen_aspect_ratio = math_round((pes.screen.width / pes.screen.height) * 10.0f);
    f32  scene_aspect_ratio = math_round((g.scene.width / g.scene.height) * 10.0f);
    if (screen_aspect_ratio == scene_aspect_ratio)
        screen_margin = vec2(0, 0);
    f32 screen_w = pes.screen.width - screen_margin.x * 2;
    f32 screen_h = pes.screen.height - screen_margin.y * 2;
    f32 screen_w_px = math_round(screen_w * pes.screen.scale);
    f32 screen_h_px = math_round(screen_h * pes.screen.scale);
    f32 scale_x = math_floor(screen_w_px / g.scene.width);
    f32 scale_y = math_floor(screen_h_px / g.scene.height);
    f32 scale_px = math_max(1.0f, math_min(scale_x, scale_y));
    f32 scale_dp = scale_px / pes.screen.scale;
    f32 scene_w_px = g.scene.width * scale_px;
    f32 scene_h_px = g.scene.height * scale_px;

    g.scene.scale = scale_dp;

    g.scene.pos.x = math_floor((screen_w_px - scene_w_px) * 0.5f) / pes.screen.scale;
    g.scene.pos.y = math_floor((screen_h_px - scene_h_px) * 0.5f) / pes.screen.scale;

    g.scene.pos.x += screen_margin.x;
    g.scene.pos.y += screen_margin.y;
}

void resize(void) {
    scene_resize();

    // g.table.w = g.scene.width - 32;
    // g.table.h = g.scene.height - 32;
}

void main(void) {
    pes_init("Air Hockey", 0, 0, rgb(40, 43, 48));
    f32 dt;
    while (pes_poll(&dt)) {
        if (pes.events & EV_RESIZE)
            resize();

        draw_rect(100, 100, 100, 100, rgb(255, 0, 0));
        Mat3x3 tr = draw_translate(200, 200);
        draw_rect(120, 120, 100, 100, rgb(0, 255, 0));
        // —— HERE ——
        //
        //  TODO: use pes.transform internally in draw_ functions
        //
        pes.transform = tr;
        draw_rect(130, 130, 100, 100, rgb(0, 255, 255));

        // // draw scene background
        // f32   sc = g.scene.scale, sx = g.scene.pos.x, sy = g.scene.pos.y;
        // Shape scene = draw_shape(sx, sy, g.scene.width * sc, g.scene.height * sc);
        // shape_fill(scene, rgb(180, 230, 200));
        // shape_corner_radius(scene, 4);

        // // draw table
        // Rect  r = { 16, 16, 160, 224 };
        // Shape table = draw_shape(sx + r.x * sc, sy + r.y * sc, r.width * sc, r.height * sc);
        // shape_fill(table, rgb(200, 200, 200));

        // // draw puck
        // Shape puck = draw_circle(
        //     vec2(sx + g.scene.width / 2 * sc, sy + g.scene.height / 2 * sc), 8.0f * sc);
        // shape_fill(puck, rgb(255, 255, 200));
    }
}
