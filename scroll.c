#include "pes.h"
#include "pes.c"

typedef struct {
    Texture tex;
    u32     tex_line_width;
    u32     size;
    Color   line_color;
} Grid;

typedef struct {
    Vec2 origin;
    f32  zoom;
} Camera;

Grid   grid = { .size = 64 };
Camera camera = { .zoom = 1.0f };

static Vec2 camera_tr_point(Camera camera, Vec2 p) { // world coord -> camera coord
    return (Vec2){ camera.origin.x + p.x * camera.zoom, camera.origin.y + p.y * camera.zoom };
}

static Vec2 camera_tr_size(Camera camera, Vec2 size) { // world coord -> camera coord
    return (Vec2){ size.x * camera.zoom, size.y * camera.zoom };
}

static Rect camera_tr_rect(Camera camera, Rect r) { // world coord -> camera coord
    return (Rect){ camera_tr_point(camera, r.origin), camera_tr_size(camera, r.size) };
}

static void camera_update(Camera* camera) {
    if (pes.events & EV_SCROLL) {
        f32 px = 1.0f / pes.screen.scale;
        camera->origin.x += pes.mouse.scrolled.x * px;
        camera->origin.y -= pes.mouse.scrolled.y * px;
    }
    if (pes.events & EV_PINCH) {
        f32  old_zoom = camera->zoom;
        f32  new_zoom = clamp(camera->zoom + pes.mouse.pinch * old_zoom, 0.1f, 4.0f);
        f32  zoom_ratio = new_zoom / old_zoom;
        Vec2 p = pes.mouse.pinch_origin;
        camera->origin.x = p.x + (camera->origin.x - p.x) * zoom_ratio;
        camera->origin.y = p.y + (camera->origin.y - p.y) * zoom_ratio;
        camera->zoom = new_zoom;
    }
}

static f32 grid_ival_overlap(f32 a0, f32 a1, f32 b0, f32 b1) {
    return math_max(0.0f, math_min(a1, b1) - math_max(a0, b0));
}

inline static void grid_resize(Grid* grid, f32 tex_line_size, const u32 tex_size) {
    if (!grid->tex.handle)
        grid->tex = texture_create(tex_size, tex_size, Texture_WRAP_U | Texture_WRAP_V);

    Color* pixels = arena_alloc_array(Color, tex_size * tex_size);
    f32    half_line_size = tex_line_size * 0.5f;
    f32    x_coverage[tex_size];
    Color  line_color = grid->line_color;

    for (u32 x = 0; x < tex_size; x++) {
        f32 start = (f32)x;
        f32 end = start + 1.0f;
        f32 coverage = grid_ival_overlap(start, end, 0.0f, half_line_size);
        coverage += grid_ival_overlap(start, end, (f32)tex_size - half_line_size, (f32)tex_size);
        x_coverage[x] = clamp_0_1(coverage);
    }

    for (u32 y = 0; y < tex_size; y++) {
        f32 start = (f32)y;
        f32 end = start + 1.0f;
        f32 coverage = grid_ival_overlap(start, end, 0.0f, half_line_size);
        coverage += grid_ival_overlap(start, end, (f32)tex_size - half_line_size, (f32)tex_size);
        f32 y_coverage = clamp_0_1(coverage);

        for (u32 x = 0; x < tex_size; x++) {
            f32 alpha = 1.0f - (1.0f - x_coverage[x]) * (1.0f - y_coverage);
            if (alpha > 0.0f)
                pixels[y * tex_size + x] = color_with_a(line_color, (u8)math_round(90.0f * alpha));
        }
    }

    texture_write(grid->tex, 0, 0, tex_size, tex_size, pixels);
}

void grid_draw(Grid* grid, Camera camera) {
    const u32 tex_size = 256;
    f32       cell_size = (f32)grid->size * camera.zoom;
    f32       line_size = math_max(1.0f, math_round(pes.screen.scale)) / pes.screen.scale;
    f32       tex_line_size = clamp(line_size * (f32)tex_size / cell_size, 1.0f, (f32)tex_size);
    u32       tex_line_width = (u32)math_round(tex_line_size * 256.0f);

    if UNLIKELY (!grid->tex.handle || tex_line_width != grid->tex_line_width) {
        grid->tex_line_width = tex_line_width;
        grid_resize(grid, tex_line_size, tex_size);
    }

    f32 u = -camera.origin.x / cell_size;
    f32 v = -camera.origin.y / cell_size;

    Shape sh = draw_texture(0, 0, pes.screen.width, pes.screen.height, grid->tex);
    sh->uvMin.x = u;
    sh->uvMin.y = v;
    sh->uvMax.x = u + pes.screen.width / cell_size;
    sh->uvMax.y = v + pes.screen.height / cell_size;
}

void main(void) {
    pes_init("Scroll", 800, 600, rgb(200, 200, 200));
    f32 dt;
    while (pes_poll(&dt)) {
        camera_update(&camera);
        grid_draw(&grid, camera);

        Shape sh;
        draw_rect(camera_tr_rect(camera, rect(256, 256, 64, 64)), rgb(255, 200, 30));
        draw_rect(camera_tr_rect(camera, rect(320, 320, 64, 64)), rgb(225, 90, 60));
        sh = draw_rect(camera_tr_rect(camera, rect(544, 288, 128, 64)), rgb(100, 90, 240));
        shape_corner_radius(sh, 9999);
        sh->opacity = 0.5;
    }
}
