#include "pes.h"
#include "pes.c"

#ifdef __wasm_simd128__
    #include <wasm_simd128.h>
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
    #include <arm_neon.h>
#endif

// initial window size
#define WINDOW_W_DP 800.0f
#define WINDOW_H_DP 600.0f
#define FB_SCALE    1u

_Static_assert(FB_SCALE >= 1u, "FB_SCALE must be at least 1");

#define WORLD_X          128
#define WORLD_Y          128
#define WORLD_Z          32
#define WORLD_CELL_COUNT ((u32)WORLD_X * (u32)WORLD_Y * (u32)WORLD_Z)
#define VOXEL_COLOR_BITS 6u
#define VOXEL_COLOR_MASK ((1u << VOXEL_COLOR_BITS) - 1u)
#define WORLD_BYTE_COUNT ((WORLD_CELL_COUNT * VOXEL_COLOR_BITS + 7u) / 8u)

_Static_assert(WORLD_X <= 256, "FaceInst x requires WORLD_X <= 256");
_Static_assert(WORLD_Y <= 256, "FaceInst y requires WORLD_Y <= 256");
_Static_assert(WORLD_Z <= 256, "FaceInst z requires WORLD_Z <= 256");

#define TILE_W_DP  32.0f
#define TILE_H_DP  16.0f
#define BLOCK_H_DP 18.0f

#define MAX_BLOCKS        20000u
#define MAX_VISIBLE_FACES (MAX_BLOCKS * 3u)
#define SORT_KEY_MAX      ((WORLD_X + WORLD_Y + WORLD_Z + 4) * 4)
#define FACE_BITS         2u
#define FACE_MASK         ((1u << FACE_BITS) - 1u)
#define PALETTE_COUNT     64u
#define SHADOW_MAX_Z_DIST 12u
#define SHADOW_MAX_DARKEN 70u

_Static_assert(PALETTE_COUNT <= 1u << VOXEL_COLOR_BITS, "palette does not fit in packed world");
_Static_assert(SHADOW_MAX_Z_DIST >= 2u, "shadow range must reach the nearest visible blocker");
_Static_assert(SHADOW_MAX_DARKEN <= 100u, "shadow darken percent must be <= 100");

typedef struct {
    i32 x, y;
} P2;

typedef struct {
    u32 index;
    u8  x, y, z;
    u8  color;
} BlockRef;

typedef enum {
    FACE_LEFT,
    FACE_RIGHT,
    FACE_TOP,
} FaceKind;

_Static_assert(FACE_TOP <= FACE_MASK, "FaceInst color_face only has 2 face bits");

typedef struct {
    i16  off_x;
    i16  off_y;
    u16  w;
    u16  h;
    u32  alpha_cap;
    u32  span_cap;
    u8*  alpha;
    u8*  lines;
    u16* alpha_x0;
    u16* alpha_x1;
} FaceSprite;

typedef struct {
    i16 sx, sy;
    u8  x, y, z;
    u8  color_face;
    u32 sort_key;
} FaceInst;

static Texture fb_tex;
static Color*  fb_pixels;
static u32     fb_w;
static u32     fb_h;
static f32     fb_scale;

static u8    world[WORLD_BYTE_COUNT];
static Color palette[PALETTE_COUNT] = {
    { 220, 220, 220, 255 }, // light grey
    { 220, 80, 60, 255 },   // red
    { 80, 160, 250, 255 },  // blue
    { 250, 210, 80, 255 },  // yellow
    { 90, 200, 110, 255 },  // green
    { 200, 140, 240, 255 }, // purple
    { 120, 210, 210, 255 }, // cyan
    { 160, 140, 80, 255 },  // orange
};

static FaceSprite spr_top, spr_left, spr_right; // buffers

static BlockRef occupied_blocks[MAX_BLOCKS];
static u32      occupied_blocks_len;
static FaceInst faces[MAX_VISIBLE_FACES];
static FaceInst faces_sorted[MAX_VISIBLE_FACES];
static u32      faces_len = 0;
static u32      sort_counts[SORT_KEY_MAX];

static Vec2 camera = { WINDOW_W_DP * 0.5f, -80.0f };
static u32  tile_w = 0;
static u32  tile_h = 0;
static u32  block_h = 0;
static bool gridlines_enabled = true;

_Static_assert(sizeof(Color) == sizeof(u32), "clear_fb assumes 32-bit pixels");

inline static u32 color_bits(Color c) {
    return (u32)c.r | ((u32)c.g << 8u) | ((u32)c.b << 16u) | ((u32)c.a << 24u);
}

inline static void fill_color_span(Color* p, u32 n, Color c) {
#ifdef __wasm_simd128__
    v128_t fill = wasm_u32x4_splat(color_bits(c));

    for (u32 chunks = n / 32u; chunks; chunks--) {
        wasm_v128_store(p + 0, fill);
        wasm_v128_store(p + 4, fill);
        wasm_v128_store(p + 8, fill);
        wasm_v128_store(p + 12, fill);
        wasm_v128_store(p + 16, fill);
        wasm_v128_store(p + 20, fill);
        wasm_v128_store(p + 24, fill);
        wasm_v128_store(p + 28, fill);
        p += 32;
    }

    n &= 31u;
#elif defined(__aarch64__) && defined(__ARM_NEON)
    uint32x4_t fill = vdupq_n_u32(color_bits(c));

    for (u32 chunks = n / 32u; chunks; chunks--) {
        vst1q_u32((u32*)(void*)(p + 0), fill);
        vst1q_u32((u32*)(void*)(p + 4), fill);
        vst1q_u32((u32*)(void*)(p + 8), fill);
        vst1q_u32((u32*)(void*)(p + 12), fill);
        vst1q_u32((u32*)(void*)(p + 16), fill);
        vst1q_u32((u32*)(void*)(p + 20), fill);
        vst1q_u32((u32*)(void*)(p + 24), fill);
        vst1q_u32((u32*)(void*)(p + 28), fill);
        p += 32;
    }

    n &= 31u;
#endif

    for (u32 i = 0; i < n; i++)
        p[i] = c;
}

static f32 fb_px_of_dp(f32 dp) {
    return math_round(dp * fb_scale);
}

static bool voxel_in_bounds(i32 x, i32 y, i32 z) {
    return (u32)x < WORLD_X && (u32)y < WORLD_Y && (u32)z < WORLD_Z;
}

static u32 voxel_index(i32 x, i32 y, i32 z) {
    return ((u32)z * WORLD_Y + (u32)y) * WORLD_X + (u32)x;
}

static u8 voxel_read_at(u32 index) {
    u32 bit_index = index * VOXEL_COLOR_BITS;
    u32 byte_index = bit_index >> 3;
    u32 shift = bit_index & 7u;
    u32 value = (u32)world[byte_index] >> shift;

    if (shift > 8u - VOXEL_COLOR_BITS)
        value |= (u32)world[byte_index + 1u] << (8u - shift);

    return (u8)(value & VOXEL_COLOR_MASK);
}

static void voxel_write_at(u32 index, u8 color) {
    u32 bit_index = index * VOXEL_COLOR_BITS;
    u32 byte_index = bit_index >> 3;
    u32 shift = bit_index & 7u;
    u32 mask = VOXEL_COLOR_MASK << shift;
    u32 value = (u32)world[byte_index];

    if (shift > 8u - VOXEL_COLOR_BITS)
        value |= (u32)world[byte_index + 1u] << 8u;

    value = (value & ~mask) | ((u32)color << shift);
    world[byte_index] = (u8)value;

    if (shift > 8u - VOXEL_COLOR_BITS)
        world[byte_index + 1u] = (u8)(value >> 8u);
}

static u32 occupied_block_find(u32 index) {
    for (u32 i = 0; i < occupied_blocks_len; i++) {
        if (occupied_blocks[i].index == index)
            return i;
    }
    return (u32)-1;
}

static void voxel_clear(i32 x, i32 y, i32 z) {
    if (!voxel_in_bounds(x, y, z))
        return;

    u32 index = voxel_index(x, y, z);
    if (!voxel_read_at(index))
        return;

    u32 i = occupied_block_find(index);
    assertf(i != (u32)-1, "occupied block list missing voxel %u", index);

    voxel_write_at(index, 0);
    occupied_blocks[i] = occupied_blocks[--occupied_blocks_len];
}

static void voxel_set(i32 x, i32 y, i32 z, u8 color) {
    assertf(
        color < PALETTE_COUNT,
        "voxel color %u exceeds %u-color FaceInst palette",
        color,
        PALETTE_COUNT);
    if (color == 0) {
        voxel_clear(x, y, z);
        return;
    }
    if (!voxel_in_bounds(x, y, z))
        return;

    u32 index = voxel_index(x, y, z);
    if (voxel_read_at(index)) {
        u32 i = occupied_block_find(index);
        assertf(i != (u32)-1, "occupied block list missing voxel %u", index);
        voxel_write_at(index, color);
        occupied_blocks[i].color = color;
        return;
    }

    assertf(occupied_blocks_len < MAX_BLOCKS, "occupied block list exceeds MAX_BLOCKS");
    voxel_write_at(index, color);
    occupied_blocks[occupied_blocks_len++] = (BlockRef){
        .index = index,
        .x = (u8)x,
        .y = (u8)y,
        .z = (u8)z,
        .color = color,
    };
}

static P2 iso_project(i32 x, i32 y, i32 z) {
    i32 cx = (i32)fb_px_of_dp(camera.x);
    i32 cy = (i32)fb_px_of_dp(camera.y);
    return (P2){
        cx + (x - y) * (i32)(tile_w / 2),
        cy + (x + y) * (i32)(tile_h / 2) - z * (i32)block_h,
    };
}

static Color shade_face(Color c, FaceKind face) {
    f32 m = face == FACE_TOP ? 1.10f : face == FACE_RIGHT ? 0.85f : 0.70f;
    return rgba(
        (u8)clamp((i32)((f32)c.r * m), 0, 255),
        (u8)clamp((i32)((f32)c.g * m), 0, 255),
        (u8)clamp((i32)((f32)c.b * m), 0, 255),
        c.a);
}

static Color darken_color(Color c, u32 percent) {
    u32 keep = 100u - percent;
    return rgba(
        (u8)((u32)c.r * keep / 100u),
        (u8)((u32)c.g * keep / 100u),
        (u8)((u32)c.b * keep / 100u),
        c.a);
}

static u32 top_shadow_darken(u8 x, u8 y, u8 z) {
    u32 range = SHADOW_MAX_Z_DIST - 2u;
    u32 index = voxel_index(x, y, z);
    u32 z_stride = WORLD_X * WORLD_Y;

    for (u32 dz = 2u; dz <= SHADOW_MAX_Z_DIST && (u32)z + dz < WORLD_Z; dz++) {
        if (voxel_read_at(index + dz * z_stride) == 0)
            continue;

        if (range == 0)
            return SHADOW_MAX_DARKEN;
        return SHADOW_MAX_DARKEN * (SHADOW_MAX_Z_DIST - dz) / range;
    }

    return 0;
}

static Color gridline_color(Color c) {
    return rgba(
        (u8)((u32)c.r * 45u / 100u), (u8)((u32)c.g * 45u / 100u), (u8)((u32)c.b * 45u / 100u), c.a);
}

static i32 edge(P2 a, P2 b, i32 x, i32 y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

static bool point_in_quad(P2* p, i32 x, i32 y) {
    bool have_neg = false;
    bool have_pos = false;

    for (u32 i = 0; i < 4; i++) {
        i32 e = edge(p[i], p[(i + 1) & 3u], x, y);
        have_neg = have_neg || e < 0;
        have_pos = have_pos || e > 0;
    }

    return !(have_neg && have_pos);
}

static void set_mask_px(u8* mask, u32 w, u32 h, i32 x, i32 y) {
    if ((u32)x < w && (u32)y < h)
        mask[(u32)y * w + (u32)x] = 255;
}

static void draw_mask_line(u8* mask, u32 w, u32 h, P2 a, P2 b) {
    i32 x0 = a.x;
    i32 y0 = a.y;
    i32 x1 = b.x;
    i32 y1 = b.y;
    i32 dx = math_abs(x1 - x0);
    i32 sx = x0 < x1 ? 1 : -1;
    i32 dy = -math_abs(y1 - y0);
    i32 sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;

    for (;;) {
        set_mask_px(mask, w, h, x0, y0);
        if (x0 == x1 && y0 == y1)
            break;

        i32 e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_mask_stair_line(u8* mask, u32 w, u32 h, P2 a, P2 b) {
    i32 x0 = a.x;
    i32 y0 = a.y;
    i32 dx = b.x - a.x;
    i32 dy = b.y - a.y;
    i32 sx = dx < 0 ? -1 : 1;
    i32 sy = dy < 0 ? -1 : 1;
    dx = math_abs(dx);
    dy = math_abs(dy);

    if (dy == 0) {
        draw_mask_line(mask, w, h, a, b);
        return;
    }

    for (i32 step = 0; step < dy; step++) {
        i32 y = y0 + sy * step;
        i32 x_a = x0 + sx * ((step * dx) / dy);
        i32 x_b = x0 + sx * (((step + 1) * dx) / dy);

        if (sx > 0) {
            for (i32 x = x_a; x < x_b; x++)
                set_mask_px(mask, w, h, x, y);
        } else {
            for (i32 x = x_a; x > x_b; x--)
                set_mask_px(mask, w, h, x, y);
        }
    }

    set_mask_px(mask, w, h, b.x, b.y);
}

static void fill_mask_quad(FaceSprite* s, P2 a, P2 b, P2 c, P2 d) {
    P2 p[4] = { a, b, c, d };
    for (i32 y = 0, w = (i32)s->w, h = (i32)s->h; y < h; y++) {
        for (i32 x = 0; x < w; x++)
            s->alpha[y * w + x] = 255 * (u8)point_in_quad(p, x, y);
    }

    memset(s->lines, 0, (usize)s->w * s->h);
    draw_mask_line(s->lines, s->w, s->h, a, b);
    draw_mask_line(s->lines, s->w, s->h, b, c);
    draw_mask_line(s->lines, s->w, s->h, c, d);
    draw_mask_line(s->lines, s->w, s->h, d, a);

    for (u32 y = 0; y < s->h; y++) {
        u32       x0 = 0;
        u32       x1 = s->w;
        const u8* alpha = s->alpha + y * s->w;

        while (x0 < s->w && !alpha[x0])
            x0++;
        while (x1 > x0 && !alpha[x1 - 1u])
            x1--;

        s->alpha_x0[y] = (u16)x0;
        s->alpha_x1[y] = (u16)x1;
    }
}

static void fill_mask_left_quad(FaceSprite* s, P2 a, P2 b, P2 c, P2 d) {
    fill_mask_quad(s, a, b, c, d);
    memset(s->lines, 0, (usize)s->w * s->h);
    draw_mask_stair_line(s->lines, s->w, s->h, a, b);
    draw_mask_line(s->lines, s->w, s->h, b, c);
    draw_mask_line(s->lines, s->w, s->h, c, d);
    draw_mask_line(s->lines, s->w, s->h, d, a);
}

static void fill_mask_right_quad(FaceSprite* s, P2 a, P2 b, P2 c, P2 d) {
    fill_mask_quad(s, a, b, c, d);
    memset(s->lines, 0, (usize)s->w * s->h);
    draw_mask_stair_line(s->lines, s->w, s->h, a, b);
    draw_mask_line(s->lines, s->w, s->h, b, c);
    draw_mask_line(s->lines, s->w, s->h, c, d);
    draw_mask_line(s->lines, s->w, s->h, d, a);
}

static void fill_mask_top_quad(FaceSprite* s, P2 a, P2 b, P2 c, P2 d) {
    fill_mask_quad(s, a, b, c, d);
    memset(s->lines, 0, (usize)s->w * s->h);
    draw_mask_line(s->lines, s->w, s->h, a, b);
    draw_mask_line(s->lines, s->w, s->h, b, c);
    draw_mask_stair_line(s->lines, s->w, s->h, c, d);
    draw_mask_line(s->lines, s->w, s->h, d, a);
}

static void resize_face_sprite(FaceSprite* s, i32 off_x, i32 off_y, u32 w, u32 h) {
    u32 alpha_cap = w * h;
    if (s->alpha_cap < alpha_cap) {
        s->alpha = PBMemRealloc(kPBMemGPA, s->alpha, alpha_cap);
        assertf(s->alpha, "cannot allocate face sprite alpha (%u B)", alpha_cap);
        s->lines = PBMemRealloc(kPBMemGPA, s->lines, alpha_cap);
        assertf(s->lines, "cannot allocate face sprite lines (%u B)", alpha_cap);
        s->alpha_cap = alpha_cap;
    }
    if (s->span_cap < h) {
        s->alpha_x0 = PBMemRealloc(kPBMemGPA, s->alpha_x0, h * sizeof(s->alpha_x0[0]));
        assertf(s->alpha_x0, "cannot allocate face sprite alpha spans (%u rows)", h);
        s->alpha_x1 = PBMemRealloc(kPBMemGPA, s->alpha_x1, h * sizeof(s->alpha_x1[0]));
        assertf(s->alpha_x1, "cannot allocate face sprite alpha spans (%u rows)", h);
        s->span_cap = h;
    }
    s->off_x = (i16)off_x;
    s->off_y = (i16)off_y;
    s->w = (u16)w;
    s->h = (u16)h;
}

static void init_face_sprites(void) {
    tile_w = (u32)fb_px_of_dp(TILE_W_DP * (f32)FB_SCALE);
    tile_h = (u32)fb_px_of_dp(TILE_H_DP * (f32)FB_SCALE);
    block_h = (u32)fb_px_of_dp(BLOCK_H_DP * (f32)FB_SCALE);

    assertf(tile_w >= 2 && tile_h >= 2 && block_h >= 1, "invalid block pixel size");

    resize_face_sprite(&spr_top, -(i32)tile_w / 2, 0, tile_w + 1, tile_h + 1);
    resize_face_sprite(
        &spr_left, -(i32)tile_w / 2, (i32)tile_h / 2, tile_w / 2 + 1, block_h + tile_h / 2 + 1);
    resize_face_sprite(&spr_right, 0, (i32)tile_h / 2, tile_w / 2 + 1, block_h + tile_h / 2 + 1);

    fill_mask_top_quad(
        &spr_top,
        (P2){ (i32)tile_w / 2, 0 },
        (P2){ (i32)tile_w, (i32)tile_h / 2 },
        (P2){ (i32)tile_w / 2, (i32)tile_h },
        (P2){ 0, (i32)tile_h / 2 });

    fill_mask_left_quad(
        &spr_left,
        (P2){ 0, 0 },
        (P2){ (i32)tile_w / 2, (i32)tile_h / 2 },
        (P2){ (i32)tile_w / 2, (i32)(tile_h / 2 + block_h) },
        (P2){ 0, (i32)block_h });

    fill_mask_right_quad(
        &spr_right,
        (P2){ (i32)tile_w / 2, 0 },
        (P2){ 0, (i32)tile_h / 2 },
        (P2){ 0, (i32)(tile_h / 2 + block_h) },
        (P2){ (i32)tile_w / 2, (i32)block_h });
}

static void init_test_world(void) {
    voxel_set(0, 20, 1, 1);
    voxel_set(0, 21, 1, 2);
    voxel_set(0, 22, 1, 3);
    voxel_set(1, 20, 1, 4);
    voxel_set(1, 21, 1, 5);
    voxel_set(1, 22, 1, 6);
    voxel_set(2, 20, 1, 7);
    voxel_set(2, 21, 1, 1);
    voxel_set(2, 22, 1, 2);

    for (i32 y = 16; y < 48; y++) {
        for (i32 x = 16; x < 48; x++)
            voxel_set(x, y, 0, 1);
    }

    for (i32 y = 18; y < 46; y++) {
        for (i32 x = 18; x < 46; x++) {
            if (((x * 17 + y * 31) & 15) < 3)
                voxel_set(x, y, 1, 6);
        }
    }

    for (i32 z = 1; z < 10; z++) {
        voxel_set(24, 24, z, 2);
        voxel_set(25, 24, z, 2);
        voxel_set(24, 25, z, 2);
        voxel_set(25, 25, z, 2);
    }

    for (i32 x = 20; x < 45; x++) {
        for (i32 z = 1; z < 6; z++)
            voxel_set(x, 34, z, 3);
    }
    for (i32 x = 29; x <= 35; x++) {
        for (i32 z = 1; z <= 3; z++)
            voxel_clear(x, 34, z);
    }

    for (i32 x = 28; x < 45; x++) {
        voxel_set(x, 26, 7, 4);
        voxel_set(x, 27, 7, 4);
    }

    for (i32 z = 1; z < 7; z++) {
        voxel_set(28, 26, z, 5);
        voxel_set(28, 27, z, 5);
        voxel_set(44, 26, z, 5);
        voxel_set(44, 27, z, 5);
    }

    // staircase, for testing shadows
    for (i32 z = 1; z < 18; z++) {
        i32 step = (z - 1) & 7;
        i32 x = 0, y = 0;
        switch (step) {
            case 0: x = 36, y = 18; break;
            case 1: x = 37, y = 18; break;
            case 2: x = 38, y = 18; break;
            case 3: x = 38, y = 19; break;
            case 4: x = 38, y = 20; break;
            case 5: x = 37, y = 20; break;
            case 6: x = 36, y = 20; break;
            case 7: x = 36, y = 19; break;
        }
        voxel_set(x, y, z, (u8)(2 + (z % 5)));
    }

    for (i32 y = 70; y < 114; y++) {
        for (i32 x = 68; x < 126; x++) {
            i32 h = 1 + ((x * 13 + y * 7) % 5);
            voxel_set(x, y, 0, 7);
            voxel_set(x, y, 1, 7);
            if (h >= 3)
                voxel_set(x, y, 2, (u8)(2 + (x + y) % 5));
        }
    }
}

static u32 face_sort_key(i32 x, i32 y, i32 z, FaceKind face) {
    u32 face_order = face == FACE_LEFT ? 0u : face == FACE_RIGHT ? 1u : 2u;
    return ((u32)(x + y + z) << 2) | face_order;
}

static u8 pack_color_face(u8 color, FaceKind face) {
    assertf(color != 0, "face color 0 represents empty voxel");
    assertf(
        color < PALETTE_COUNT,
        "face color %u exceeds %u-color FaceInst palette",
        color,
        PALETTE_COUNT);
    assertf((u32)face <= FACE_MASK, "face %u exceeds %u-bit FaceInst field", (u32)face, FACE_BITS);
    return (u8)((color << FACE_BITS) | (u8)face);
}

static u8 face_inst_color(const FaceInst* f) {
    return (f->color_face >> FACE_BITS) - 1;
}

static FaceKind face_inst_face(const FaceInst* f) {
    return (FaceKind)(f->color_face & FACE_MASK);
}

static void push_face(i32 x, i32 y, i32 z, FaceKind face, u8 color) {
    if (faces_len >= MAX_VISIBLE_FACES)
        return;

    P2 p = iso_project(x, y, z);
    faces[faces_len++] = (FaceInst){
        .sx = (i16)p.x,
        .sy = (i16)p.y,
        .x = (u8)x,
        .y = (u8)y,
        .z = (u8)z,
        .color_face = pack_color_face(color, face),
        .sort_key = face_sort_key(x, y, z, face),
    };
}

static void emit_visible_faces_for_block(const BlockRef* b) {
    i32 x = b->x;
    i32 y = b->y;
    i32 z = b->z;

    if ((u32)b->z + 1u >= WORLD_Z || voxel_read_at(b->index + WORLD_X * WORLD_Y) == 0)
        push_face(x, y, z, FACE_TOP, b->color);

    if ((u32)b->y + 1u >= WORLD_Y || voxel_read_at(b->index + WORLD_X) == 0)
        push_face(x, y, z, FACE_LEFT, b->color);

    if ((u32)b->x + 1u >= WORLD_X || voxel_read_at(b->index + 1u) == 0)
        push_face(x, y, z, FACE_RIGHT, b->color);
}

static void build_face_list(void) {
    faces_len = 0;

    for (u32 i = 0; i < occupied_blocks_len; i++)
        emit_visible_faces_for_block(&occupied_blocks[i]);
}

static void sort_faces_counting(void) {
    for (u32 i = 0; i < SORT_KEY_MAX; i++)
        sort_counts[i] = 0;

    for (u32 i = 0; i < faces_len; i++)
        sort_counts[faces[i].sort_key]++;

    u32 total = 0;
    for (u32 i = 0; i < SORT_KEY_MAX; i++) {
        u32 count = sort_counts[i];
        sort_counts[i] = total;
        total += count;
    }

    for (u32 i = 0; i < faces_len; i++) {
        u32 dst = sort_counts[faces[i].sort_key]++;
        faces_sorted[dst] = faces[i];
    }
}

static void clear_fb(Color c) {
    fill_color_span(fb_pixels, fb_w * fb_h, c);
}

static bool face_sprite_offscreen(const FaceSprite* s, i32 sx, i32 sy) {
    i32 x0 = sx + s->off_x;
    i32 y0 = sy + s->off_y;
    i32 x1 = x0 + s->w;
    i32 y1 = y0 + s->h;

    return x1 < 0 || y1 < 0 || x0 >= (i32)fb_w || y0 >= (i32)fb_h;
}

static void blit_face_lines(const FaceSprite* s, i32 sx, i32 sy, Color color) {
    i32 dst_x0 = sx + s->off_x;
    i32 dst_y0 = sy + s->off_y;

    for (u32 y = 0; y < s->h; y++) {
        i32 dy = dst_y0 + (i32)y;
        if ((u32)dy >= fb_h)
            continue;

        const u8* src = s->lines + y * s->w;
        Color*    dst = fb_pixels + dy * fb_w;

        for (u32 x = 0; x < s->w; x++) {
            if (!src[x])
                continue;

            i32 dx = dst_x0 + (i32)x;
            if ((u32)dx >= fb_w)
                continue;

            dst[dx] = color;
        }
    }
}

inline static void blit_face(const FaceSprite* s, i32 sx, i32 sy, Color color) {
    i32 dst_x0 = sx + s->off_x;
    i32 dst_y0 = sy + s->off_y;
    i32 y0 = 0;
    i32 y1 = (i32)s->h;

    if (dst_y0 < 0)
        y0 = -dst_y0;
    if (dst_y0 + y1 > (i32)fb_h)
        y1 = (i32)fb_h - dst_y0;

    for (i32 y = y0; y < y1; y++) {
        i32 dy = dst_y0 + (i32)y;
        i32 x0 = dst_x0 + (i32)s->alpha_x0[y];
        i32 x1 = dst_x0 + (i32)s->alpha_x1[y];

        if (x0 < 0)
            x0 = 0;
        if (x1 > (i32)fb_w)
            x1 = (i32)fb_w;
        if (x0 >= x1)
            continue;

        fill_color_span(fb_pixels + dy * fb_w + x0, (u32)(x1 - x0), color);
    }
}

inline static const FaceSprite* sprite_for_face(FaceKind face) {
    switch (face) {
        case FACE_LEFT:  return &spr_left;
        case FACE_RIGHT: return &spr_right;
        case FACE_TOP:   return &spr_top;
    }
    return &spr_top;
}

static void render_faces(void) {
    for (u32 i = 0; i < faces_len; i++) {
        FaceInst*         f = &faces_sorted[i];
        FaceKind          face = face_inst_face(f);
        const FaceSprite* s = sprite_for_face(face);

        if (face_sprite_offscreen(s, f->sx, f->sy))
            continue;

        Color face_color = shade_face(palette[face_inst_color(f)], face);
        if (face == FACE_TOP)
            face_color = darken_color(face_color, top_shadow_darken(f->x, f->y, f->z));

        blit_face(s, f->sx, f->sy, face_color);

        if (gridlines_enabled)
            blit_face_lines(s, f->sx, f->sy, gridline_color(face_color));
    }
}

static void render(void) {
    clear_fb(rgb(200, 200, 200));
    build_face_list();
    sort_faces_counting();
    render_faces();

    texture_write(fb_tex, 0, 0, fb_w, fb_h, fb_pixels);

    f32 w = pes.screen.width;
    f32 h = pes.screen.height;
    f32 x = (pes.screen.width - w) * 0.5f;
    f32 y = (pes.screen.height - h) * 0.5f;
    draw_texture(x, y, w, h, fb_tex);
}

static void resize_framebuffer(void) {
    f32 next_scale = pes.screen.scale / (f32)FB_SCALE;
    u32 next_w = (u32)math_max(1, (i32)math_round(pes.screen.width * next_scale));
    u32 next_h = (u32)math_max(1, (i32)math_round(pes.screen.height * next_scale));
    assertf(next_w > 0 && next_h > 0, "invalid framebuffer size %u x %u", next_w, next_h);

    if (fb_w != next_w || fb_h != next_h) {
        if (fb_tex.handle)
            texture_close(fb_tex);

        usize pixel_size = (usize)next_w * (usize)next_h * sizeof(Color);
        fb_pixels = PBMemRealloc(kPBMemGPA, fb_pixels, pixel_size);
        assertf(fb_pixels, "cannot allocate framebuffer (%u x %u)", next_w, next_h);

        fb_tex = texture_create(next_w, next_h, Texture_STREAMING | Texture_NO_FILTER);
        fb_w = next_w;
        fb_h = next_h;
    }

    if (fb_scale != next_scale) {
        fb_scale = next_scale;
        init_face_sprites();
    }
}

static bool update_camera(f32 dt) {
    f32 speed = 150.0f * dt;

    bool changed = false;

    if (key_held(Key_Left)) {
        camera.x += speed;
        changed = true;
    }
    if (key_held(Key_Right)) {
        camera.x -= speed;
        changed = true;
    }
    if (key_held(Key_Up)) {
        camera.y += speed;
        changed = true;
    }
    if (key_held(Key_Down)) {
        camera.y -= speed;
        changed = true;
    }
    if (key_held(Key_Space) && !vec2_is_zero(pes.mouse.moved)) {
        camera.x += pes.mouse.moved.x;
        camera.y += pes.mouse.moved.y;
        changed = true;
    }

    return changed;
}

static bool update(f32 dt) {
    bool changed = update_camera(dt);

    pes.mouse.cursor = key_held(Key_Space) ? Cursor_OPEN_HAND : Cursor_DEFAULT;

    if (key_pressed(Key_G)) {
        gridlines_enabled = !gridlines_enabled;
        changed = true;
    }

    return changed;
}

void main(void) {
    pes_init("Blocks", 0, 0, rgb(30, 30, 30));
    window_resize(WINDOW_W_DP, WINDOW_H_DP);

    init_test_world();

    f32 dt;
    while (pes_poll(&dt)) {
        bool changed = update(dt);
        if (changed || (pes.events & EV_RESIZE)) {
            if (pes.events & EV_RESIZE)
                resize_framebuffer();
            render();
        }
    }
}
