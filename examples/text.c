#include "../pes.h"
#include "../pes.c"

inline static f32 remap(f32 x, f32 min, f32 max) {
    return min + (x + 1.0f) * 0.5f * (max - min);
}

f32 tsin(i64 offs_ms, f32 rate, f32 min, f32 max) {
    i64 ms = (i64)(pes.time / MILLISECOND) + offs_ms;
    return remap(math_sin((f32)ms / rate), min, max);
}

f32 tcos(i64 offs_ms, f32 rate, f32 min, f32 max) {
    i64 ms = (i64)(pes.time / MILLISECOND) + offs_ms;
    return remap(math_cos((f32)ms / rate), min, max);
}

void main(void) {
    pes_init("Text", 0, 0, rgb(40, 43, 48));
    for (f32 dt; pes_poll(&dt);) {
        // unlimited (single-line composition)
        draw_text(rect(32, 32, 0, 0), FONT_UI_M, rgb(90, 250, 250), "Single line");

        // limited by width (but not by height)
        draw_text(rect(120, 32, 60, 0), FONT_UI_M, rgb(90, 150, 250), "This text will wrap");

        // limited by width and height
        draw_text(rect(200, 32, 60, 60), FONT_UI_M, rgb(200, 150, 250), "This text will wrap");

        // animated example:

        Rect r = { { 64, 96 }, { 0, 0 } };
        Font font = { .size = 24, .weight = 400 };
        r.origin.x = tsin(0, 1000.0f, 32, 200);
        r.origin.y += text_size(draw_text(r, font, rgb(255, 90, 90), "Hello world!")).y;

        font.weight = tsin(400, 400.0f, 200, 600);
        font.size = tcos(0, 4000.0f, 18, 64);
        r.origin.x = tsin(200, 1000.0f, 32, 200);
        r.origin.y += text_size(draw_text(r, font, rgb(255, 130, 80), "¡Hola Mundo!")).y;

        font.size = tcos(0, 1000.0f, 24, 128);
        font.weight = 600;
        r.origin.x = tsin(400, 1000.0f, 32, 200);
        r.origin.y += text_size(draw_text(r, font, rgb(255, 190, 60), "Hej världen!")).y;

        font.weight = 400;
        font.size = tcos(300, 4000.0f, 18, 64);
        r.origin.x = tsin(600, 1000.0f, 32, 200);
        r.origin.y += text_size(draw_text(r, font, rgb(50, 150, 250), "Γειά σου Κόσμε!")).y;

        font.weight = 400;
        font.size = tcos(600, 5000.0f, 8, 140);
        r.origin.x = tsin(800, 1000.0f, 32, 200);
        r.origin.y += text_size(draw_text(r, font, rgb(90, 220, 120), "Привіт, світе!")).y;

        font.weight = 500;
        font.size = tcos(10, 2000.0f, 8, 42);
        r.origin.x = tsin(1000, 1000.0f, 32, 200);
        r.origin.y += text_size(draw_text(r, font, rgb(220, 225, 230), "こんにちは世界！")).y;

        // draw frame counter aligned to bottom right corner of the window
        static u64 frame = 0;
        font = (Font){ .family = FontFamily_MONOSPACE, .size = 42, .weight = 300 };
        f32   max_w = pes.screen.width - 64.0f;
        Color color = rgb(255, 160, 240);
        Text  text = draw_textf(rect(0, 0, max_w, 0), font, color, "Frame %4llu", ++frame);
        Vec2  size = text_size(text);
        Vec2  origin = { pes.screen.width - size.x - 32.0f, pes.screen.height - size.y - 32.0f };
        text_set_origin(text, origin);
    }
}
