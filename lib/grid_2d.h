/*
2D planar grid

Example:

    void main(void) {
        pes_init("Grid", 0, 0, rgb(200, 200, 200));
        static Grid grid = { .size = 32, .line_alpha = 90 };
        Vec2 origin = { 0, 0 };
        f32 zoom = 1.0f;
        for (f32 dt; pes_poll(&dt);) {
            grid_draw(&grid, origin, zoom);
        }
    }

Example using camera_2d.h:

    void main(void) {
        pes_init("Grid", 0, 0, rgb(200, 200, 200));
        static Grid   grid = { .size = 32, .line_alpha = 90 };
        static Camera camera = CAMERA_INIT;
        for (f32 dt; pes_poll(&dt);) {
            grid_draw(&grid, camera.origin, camera.zoom);
        }
    }

*/
#include "../pes.h"
PB_API_BEGIN

typedef struct {
    Texture tex;
    f32     tex_line_size;
    u32     tex_size;
    u32     size;
    Color   line_color;
    u8      line_alpha;
} Grid;

#define GRID_INIT ((Grid){ .size = 32, .line_alpha = 90 })

static f32 grid_ival_overlap(f32 a0, f32 a1, f32 b0, f32 b1) {
    return math_max(0.0f, math_min(a1, b1) - math_max(a0, b0));
}

inline static void grid_draw_resize(Grid* grid, f32 zoom, u32 tex_size) {
    if (grid->tex_size != tex_size) {
        grid->tex_size = tex_size;
        texture_close(grid->tex);
        grid->tex = texture_create(tex_size, tex_size, Texture_WRAP_U | Texture_WRAP_V);
    }

    Color* pixels = arena_alloc_array(Color, tex_size * tex_size);
    f32    half_line_size = grid->tex_line_size * 0.5f;
    f32    x_coverage[tex_size];
    Color  line_color = grid->line_color;
    f32    line_alpha = (f32)math_round((f32)grid->line_alpha * clamp(zoom, 0.0f, 1.0f));

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
            if (alpha > 0.0f) {
                pixels[y * tex_size + x] = color_with_a(
                    line_color, (u8)math_round(line_alpha * alpha));
            }
        }
    }

    texture_write(grid->tex, 0, 0, tex_size, tex_size, pixels);
}

static void grid_draw(Grid* grid, Vec2 origin, f32 zoom) {
    assertf(grid->size >= 1.0f, "grid.size must be at least 1");

    u32 tex_size = PBPow2Ceil((u32)math_ceil((f32)grid->size * pes.screen.scale * zoom));
    tex_size = clamp(tex_size, 64.0f, 1024.0f);

    f32 cell_size = (f32)grid->size * zoom;
    f32 line_size = math_max(1.0f, math_round(pes.screen.scale)) / pes.screen.scale;
    f32 tex_line_size = clamp(line_size * (f32)tex_size / cell_size, 1.0f, (f32)tex_size);

    if UNLIKELY (tex_line_size != grid->tex_line_size || grid->tex_size != tex_size) {
        grid->tex_line_size = tex_line_size;
        grid_draw_resize(grid, zoom, tex_size);
    }

    f32 u = -origin.x / cell_size;
    f32 v = -origin.y / cell_size;

    Shape sh = draw_texture(0, 0, pes.screen.width, pes.screen.height, grid->tex);
    sh->uvMin.x = u;
    sh->uvMin.y = v;
    sh->uvMax.x = u + pes.screen.width / cell_size;
    sh->uvMax.y = v + pes.screen.height / cell_size;
}

PB_API_END
