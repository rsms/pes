#include "pes.h"
#include "pes.c"

f32   radius = 17;
Color color = { 255, 80, 0, 255 };
Vec2  pos = { 100, 100 };
Vec2  vel = { 1, 2 };
Vec2  squash = { 1, 1 };
Vec2  squash_vel = { 0, 0 };

u32     fb_w = 400; // px
u32     fb_h = 300; // px
Texture fb_tex;
Color   fb_pixels[640 * 480 * 4];

void draw_ellipse(Vec2 origin, Vec2 radius, Color color) {
    u32 x0 = (u32)clamp((i32)(origin.x - radius.x - 1), 0, (i32)fb_w - 1);
    u32 y0 = (u32)clamp((i32)(origin.y - radius.y - 1), 0, (i32)fb_h - 1);
    u32 x1 = (u32)clamp((i32)(origin.x + radius.x + 1), 0, (i32)fb_w - 1);
    u32 y1 = (u32)clamp((i32)(origin.y + radius.y + 1), 0, (i32)fb_h - 1);
    f32 inv_rx2 = 1.0f / (radius.x * radius.x);
    f32 inv_ry2 = 1.0f / (radius.y * radius.y);
    for (u32 y = y0; y <= y1; y++) {
        for (u32 x = x0; x <= x1; x++) {
            f32 dx = (f32)x + 0.5f - origin.x;
            f32 dy = (f32)y + 0.5f - origin.y;
            if (dx * dx * inv_rx2 + dy * dy * inv_ry2 <= 1.0f)
                fb_pixels[y * fb_w + x] = color;
        }
    }
}

void ball_update(f32 dt) {
    pos = vec2_add(pos, vec2_scale(vel, 90.0f * dt));

    bool hit_x = false, hit_y = false;

    if (pos.x - radius <= 0.0f) {
        vel.x = -vel.x;
        pos.x = radius;
        hit_x = true;
    } else if (pos.x + radius >= (f32)fb_w) {
        vel.x = -vel.x;
        pos.x = (f32)fb_w - radius;
        hit_x = true;
    }

    if (pos.y - radius <= 0.0f) {
        vel.y = -vel.y;
        pos.y = radius;
        hit_y = true;
    } else if (pos.y + radius >= (f32)fb_h) {
        vel.y = -vel.y;
        pos.y = (f32)fb_h - radius;
        hit_y = true;
    }

    if (hit_x)
        squash = vec2(0.90f, 1.11f);
    if (hit_y)
        squash = vec2(1.11f, 0.90f);
    if (hit_x || hit_y)
        squash_vel = vec2(0, 0);

    const f32 spring_strength = 100;
    const f32 spring_damping = 9;

    f32 m = 1.0f / (1.0f + spring_damping * dt);
    squash_vel.x = (squash_vel.x + (1.0f - squash.x) * spring_strength * dt) * m;
    squash_vel.y = (squash_vel.y + (1.0f - squash.y) * spring_strength * dt) * m;

    squash = vec2_add(squash, vec2_scale(squash_vel, dt));
}

void ball_render(Vec2 pos) {
    Vec2 r = { radius * squash.x, radius * squash.y };
    // keep squashed side touching the wall on the impact frame
    if (pos.x <= radius) {
        pos.x = r.x;
    } else if (pos.x >= (f32)fb_w - radius) {
        pos.x = (f32)fb_w - r.x;
    }
    if (pos.y <= radius) {
        pos.y = r.y;
    } else if (pos.y >= (f32)fb_h - radius) {
        pos.y = (f32)fb_h - r.y;
    }
    draw_ellipse(pos, r, color);
}

void main(void) {
    pes_init("Framebuffer", fb_w * 2, fb_h * 2, rgb(30, 30, 30));
    fb_tex = texture_create(fb_w, fb_h, Texture_STREAMING | Texture_NO_FILTER);
    f32 dt;
    while (pes_poll(&dt)) {
        ball_update(dt);
        Color* px = fb_pixels;
        for (u32 y = 0, w = fb_w, h = fb_h, d = 1; y < h; y++) {
            for (u32 x = 0; x < w; x++)
                *px++ = (d + x) % 2 == 0 ? rgb(240, 238, 220) : rgb(220, 218, 200);
            d++;
        }
        ball_render(pos);
        texture_write(fb_tex, 0, 0, fb_w, fb_h, fb_pixels);

        f32 w = fb_w * 2;
        f32 h = fb_h * 2;
        f32 x = (pes.screen.width - w) / 2.0f;
        f32 y = (pes.screen.height - h) / 2.0f;
        draw_texture(x, y, w, h, fb_tex);
    }
}

// void pes_init(void) {
//     pes.screen.clear_color = rgb(30, 30, 30);
//     pes.screen.width = fb_w * 2;
//     pes.screen.height = fb_h * 2;
//     fb_tex = texture_create(fb_w, fb_h, Texture_STREAMING | Texture_NO_FILTER);
// }

// void pes_update(u64 events, f32 dt) {
//     ball_update(dt);
//     Color* px = fb_pixels;
//     for (u32 y = 0, w = fb_w, h = fb_h, d = 1; y < h; y++) {
//         for (u32 x = 0; x < w; x++)
//             *px++ = (d + x) % 2 == 0 ? rgb(240, 238, 220) : rgb(220, 218, 200);
//         d++;
//     }
//     ball_render(pos);
//     texture_write(fb_tex, 0, 0, fb_w, fb_h, fb_pixels);
// }

// void pes_draw(void) {
//     f32 w = fb_w * 2;
//     f32 h = fb_h * 2;
//     f32 x = (pes.screen.width - w) / 2.0f;
//     f32 y = (pes.screen.height - h) / 2.0f;
//     draw_texture(x, y, w, h, fb_tex);
// }
