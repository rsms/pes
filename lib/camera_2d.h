/*
Simple 2D camera

Interactions:

    mouse scroll                 -> pan
    mouse scroll + press ALT key -> zoom
    touch pinch                  -> zoom
    press SHIFT + 1..8 key       -> set zoom level

Example:

    void main(void) {
        pes_init("Camera", 0, 0, rgb(200, 200, 200));
        static Camera camera = CAMERA_INIT;
        for (f32 dt; pes_poll(&dt);) {
            camera_update(&camera, 0.005f, 1.0f, 1.0f);
            Rect rect_in_world_space = rect(100, 100, 50, 50);
            Rect rect_in_screen_space = camera_tr_rect(camera, rect_in_world_space);
            draw_rect(rect_in_screen_space, rgb(255, 200, 30));
        }
    }

*/
#include "../pes.h"
PB_API_BEGIN

typedef struct {
    Vec2 origin;
    f32  zoom; // 1.0 = 100%
    f32  zoom_min, zoom_max; // limit zoom value to range [zoom_min-zoom_max]
} Camera;

#define CAMERA_INIT ((Camera){ .zoom = 1.0f, .zoom_min = 0.1f, .zoom_max = 8.0f })

static Vec2 camera_tr_point(Camera camera, Vec2 p) { // world coord -> camera coord
    return (Vec2){ camera.origin.x + p.x * camera.zoom, camera.origin.y + p.y * camera.zoom };
}

static Vec2 camera_tr_size(Camera camera, Vec2 size) { // world coord -> camera coord
    return (Vec2){ size.x * camera.zoom, size.y * camera.zoom };
}

static Rect camera_tr_rect(Camera camera, Rect r) { // world coord -> camera coord
    return (Rect){ camera_tr_point(camera, r.origin), camera_tr_size(camera, r.size) };
}

static void camera_set_zoom(Camera* camera, f32 new_zoom, Vec2 origin) {
    new_zoom = clamp(new_zoom, camera->zoom_min, camera->zoom_max);
    f32 zoom_ratio = new_zoom / camera->zoom;
    camera->origin.x = origin.x + (camera->origin.x - origin.x) * zoom_ratio;
    camera->origin.y = origin.y + (camera->origin.y - origin.y) * zoom_ratio;
    camera->zoom = new_zoom;
}

static void camera_adjust_zoom(Camera* camera, f32 zoom_delta, Vec2 origin) {
    f32 new_zoom = camera->zoom + zoom_delta * camera->zoom;
    camera_set_zoom(camera, new_zoom, origin);
}

static void camera_update(Camera* camera, f32 scroll_zoom_exp, f32 pinch_exp, f32 pan_exp) {
    // ALT+scroll -> zoom
    // scroll     -> pan
    if (pes.events & EV_SCROLL) {
        if (key_alt_held()) {
            camera_adjust_zoom(camera, pes.mouse.scrolled.y * scroll_zoom_exp, pes.mouse.origin);
        } else {
            camera->origin.x += pes.mouse.scrolled.x * pan_exp;
            camera->origin.y -= pes.mouse.scrolled.y * pan_exp;
        }
    }

    // touch pinch -> zoom (only available in Playbit >=0.3.1)
    if ((pes.events & EV_PINCH) && __playbit__ >= 0x000301)
        camera_adjust_zoom(camera, pes.mouse.pinch * pinch_exp, pes.mouse.pinch_origin);

    // SHIFT+1..8 -> set zoom level
    int digit = key_digit_held();
    if (key_shift_held() && digit > 0 && digit < 9)
        camera_set_zoom(camera, (f32)digit, pes.mouse.origin);
}

PB_API_END
