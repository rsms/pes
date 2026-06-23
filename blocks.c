#include "pes.h"
#include "pes.c"

#ifdef __wasm_simd128__
    #include <wasm_simd128.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
    #include <arm_neon.h>
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// constants

#define WINDOW_W_DP 1088.0f
#define WINDOW_H_DP 800.0f
#define FB_SCALE    1u

#define WORLD_X          128
#define WORLD_Y          128
#define WORLD_Z          32
#define WORLD_CELL_COUNT ((u32)WORLD_X * (u32)WORLD_Y * (u32)WORLD_Z)
#define VOXEL_COLOR_BITS 6u
#define VOXEL_COLOR_MASK ((1u << VOXEL_COLOR_BITS) - 1u)
#define WORLD_BYTE_COUNT ((WORLD_CELL_COUNT * VOXEL_COLOR_BITS + 7u) / 8u)

#define TILE_W_DP  32.0f
#define TILE_H_DP  16.0f
#define BLOCK_H_DP 16.0f

#define MAX_BLOCKS        20000u
#define MAX_VISIBLE_FACES (MAX_BLOCKS * 3u)
#define SORT_KEY_MAX      ((WORLD_X + WORLD_Y + WORLD_Z + 4) * 4)
#define FACE_BITS         2u
#define FACE_MASK         ((1u << FACE_BITS) - 1u)
#define PALETTE_COUNT     64u
#define SHADOW_MAX_Z_DIST 12u
#define SHADOW_MAX_DARKEN 70u
#define MOUSE_PRIMARY     1u
#define MOUSE_SECONDARY   2u
#define IMPACT_DURATION   0.32f
#define IMPACT_PARTICLES  14u

////////////////////////////////////////////////////////////////////////////////////////////////////
// types

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

typedef enum {
    Mode_SELECT,
    Mode_ADD,
} InteractionMode;

typedef struct {
    bool     valid;
    FaceInst face;
} FaceHit;

typedef struct {
    bool     valid;
    i32      x, y, z;
    bool     contact_valid;
    FaceInst contact_face;
} CellTarget;

typedef struct {
    bool valid;
    i32  x, y, z;
} SelectedBlock;

typedef struct {
    f32 x, y;
    f32 vx, vy;
    u8  life;
} ImpactParticle;

typedef struct {
    bool           valid;
    i32            x, y, z;
    u8             color;
    f32            elapsed;
    ImpactParticle particle[IMPACT_PARTICLES];
} ImpactAnim;

////////////////////////////////////////////////////////////////////////////////////////////////////
// global state

static Texture fb_tex;
static Color*  fb_pixels;
static u32     fb_w;
static u32     fb_h;
static f32     fb_scale;

static u8    world[WORLD_BYTE_COUNT];
static Color palette[PALETTE_COUNT] = {
    { 220, 220, 220, 255 }, // white
    { 220, 80, 60, 255 },   // red
    { 80, 160, 250, 255 },  // blue
    { 250, 210, 80, 255 },  // yellow
    { 90, 200, 110, 255 },  // green
    { 200, 140, 240, 255 }, // purple
    { 120, 210, 210, 255 }, // cyan
    { 240, 120, 80, 255 },  // orange
    { 60, 60, 60, 255 },    // black
    { 245, 120, 180, 255 }, // pink
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

static InteractionMode interaction_mode = Mode_SELECT;
static FaceHit         hover_face;
static CellTarget      add_preview;
static SelectedBlock   selected_block;
static ImpactAnim      impact_anim;
static u8              selected_palette_slot = 1;
static Vec2            viewport_shake;

////////////////////////////////////////////////////////////////////////////////////////////////////

_Static_assert(FB_SCALE >= 1u, "FB_SCALE must be at least 1");

_Static_assert(WORLD_X <= 256, "FaceInst x requires WORLD_X <= 256");
_Static_assert(WORLD_Y <= 256, "FaceInst y requires WORLD_Y <= 256");
_Static_assert(WORLD_Z <= 256, "FaceInst z requires WORLD_Z <= 256");

_Static_assert(PALETTE_COUNT <= 1u << VOXEL_COLOR_BITS, "palette does not fit in packed world");
_Static_assert(SHADOW_MAX_Z_DIST >= 2u, "shadow range must reach the nearest visible blocker");
_Static_assert(SHADOW_MAX_DARKEN <= 100u, "shadow darken percent must be <= 100");

_Static_assert(sizeof(Color) == sizeof(u32), "clear_fb assumes 32-bit pixels");

_Static_assert(FACE_TOP <= FACE_MASK, "FaceInst color_face only has 2 face bits");

////////////////////////////////////////////////////////////////////////////////////////////////////

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
    i32 cx = (i32)fb_px_of_dp(camera.x + viewport_shake.x);
    i32 cy = (i32)fb_px_of_dp(camera.y + viewport_shake.y);
    return (P2){
        cx + (x - y) * (i32)(tile_w / 2),
        cy + (x + y) * (i32)(tile_h / 2) - z * (i32)block_h,
    };
}

static Color shade_face(Color c, FaceKind face) {
    f32 m = face == FACE_TOP ? 1.10f : face == FACE_LEFT ? 0.85f : 0.70f;
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
    voxel_set(2, 21, 1, 8);
    voxel_set(2, 22, 1, 9);

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

static bool face_hit_test(const FaceInst* f, i32 px, i32 py) {
    FaceKind          face = face_inst_face(f);
    const FaceSprite* s = sprite_for_face(face);
    i32               x = px - (f->sx + s->off_x);
    i32               y = py - (f->sy + s->off_y);

    if ((u32)x >= s->w || (u32)y >= s->h)
        return false;
    return s->alpha[(u32)y * s->w + (u32)x] != 0;
}

static FaceHit pick_visible_face(void) {
    FaceHit hit = { 0 };
    if (fb_w == 0 || fb_h == 0)
        return hit;

    i32 px = (i32)math_round(pes.mouse.origin.x * fb_scale);
    i32 py = (i32)math_round(pes.mouse.origin.y * fb_scale);

    for (u32 i = faces_len; i > 0; i--) {
        FaceInst* f = &faces_sorted[i - 1u];
        if (!face_hit_test(f, px, py))
            continue;

        hit.valid = true;
        hit.face = *f;
        return hit;
    }

    return hit;
}

static CellTarget add_target_from_face(const FaceInst* f) {
    CellTarget target = {
        .valid = true,
        .x = f->x,
        .y = f->y,
        .z = f->z,
    };

    switch (face_inst_face(f)) {
        case FACE_LEFT:  target.y++; break;
        case FACE_RIGHT: target.x++; break;
        case FACE_TOP:   target.z++; break;
    }

    target.valid = voxel_in_bounds(target.x, target.y, target.z)
                && voxel_read_at(voxel_index(target.x, target.y, target.z)) == 0;
    target.contact_valid = target.valid;
    target.contact_face = *f;
    return target;
}

static CellTarget add_target_from_mouse_column(void) {
    CellTarget target = { 0 };
    if (tile_w == 0 || tile_h == 0)
        return target;

    f32 px = pes.mouse.origin.x * fb_scale;
    f32 py = pes.mouse.origin.y * fb_scale;
    f32 cx = fb_px_of_dp(camera.x + viewport_shake.x);
    f32 cy = fb_px_of_dp(camera.y + viewport_shake.y);
    f32 a = (px - cx) / ((f32)tile_w * 0.5f);
    f32 b = (py - cy) / ((f32)tile_h * 0.5f);
    i32 x = (i32)math_floor((a + b) * 0.5f);
    i32 y = (i32)math_floor((b - a) * 0.5f);

    if ((u32)x >= WORLD_X || (u32)y >= WORLD_Y)
        return target;

    target.x = x;
    target.y = y;
    target.z = 0;
    target.valid = true;

    for (i32 z = (i32)WORLD_Z - 1; z >= 0; z--) {
        if (voxel_read_at(voxel_index(x, y, z)) == 0)
            continue;

        target.z = z + 1;
        target.valid = target.z < (i32)WORLD_Z;
        return target;
    }

    return target;
}

static CellTarget add_target_from_hover(void) {
    if (hover_face.valid)
        return add_target_from_face(&hover_face.face);
    return add_target_from_mouse_column();
}

static Color alpha_over(Color dst, Color src) {
    u32 a = src.a;
    u32 inv = 255u - a;
    return rgba(
        (u8)(((u32)src.r * a + (u32)dst.r * inv) / 255u),
        (u8)(((u32)src.g * a + (u32)dst.g * inv) / 255u),
        (u8)(((u32)src.b * a + (u32)dst.b * inv) / 255u),
        255);
}

static Color color_alpha(Color c, u8 alpha) {
    c.a = alpha;
    return c;
}

static void blend_pixel(i32 x, i32 y, Color color) {
    if ((u32)x >= fb_w || (u32)y >= fb_h)
        return;
    Color* p = fb_pixels + (u32)y * fb_w + (u32)x;
    *p = alpha_over(*p, color);
}

static void blend_stamp(i32 x, i32 y, i32 thickness, Color color) {
    i32 r = thickness / 2;
    for (i32 yy = 0; yy < thickness; yy++) {
        for (i32 xx = 0; xx < thickness; xx++)
            blend_pixel(x + xx - r, y + yy - r, color);
    }
}

static void draw_fb_line(P2 a, P2 b, i32 thickness, Color color) {
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
        blend_stamp(x0, y0, thickness, color);
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

static void blit_face_blend(const FaceSprite* s, i32 sx, i32 sy, Color color) {
    i32 dst_x0 = sx + s->off_x;
    i32 dst_y0 = sy + s->off_y;
    i32 y0 = 0;
    i32 y1 = (i32)s->h;

    if (dst_y0 < 0)
        y0 = -dst_y0;
    if (dst_y0 + y1 > (i32)fb_h)
        y1 = (i32)fb_h - dst_y0;

    for (i32 y = y0; y < y1; y++) {
        i32 dy = dst_y0 + y;
        i32 x0 = dst_x0 + (i32)s->alpha_x0[y];
        i32 x1 = dst_x0 + (i32)s->alpha_x1[y];

        if (x0 < 0)
            x0 = 0;
        if (x1 > (i32)fb_w)
            x1 = (i32)fb_w;

        for (i32 x = x0; x < x1; x++)
            blend_pixel(x, dy, color);
    }
}

static void draw_block_overlay(i32 x, i32 y, i32 z, u8 color, u8 alpha) {
    P2 p = iso_project(x, y, z);

    Color left = color_alpha(shade_face(palette[color - 1u], FACE_LEFT), alpha);
    Color right = color_alpha(shade_face(palette[color - 1u], FACE_RIGHT), alpha);
    Color top = color_alpha(shade_face(palette[color - 1u], FACE_TOP), alpha);

    blit_face_blend(&spr_left, p.x, p.y, left);
    blit_face_blend(&spr_right, p.x, p.y, right);
    blit_face_blend(&spr_top, p.x, p.y, top);
}

static void draw_preview_shadow(const CellTarget* target, u8 alpha) {
    if (target->contact_valid) {
        FaceKind          face = face_inst_face(&target->contact_face);
        const FaceSprite* s = sprite_for_face(face);
        blit_face_blend(s, target->contact_face.sx, target->contact_face.sy, rgba(0, 0, 0, alpha));
        return;
    }

    if (target->z <= 0)
        return;

    i32 support_z = target->z - 1;
    if (!voxel_in_bounds(target->x, target->y, support_z)
        || voxel_read_at(voxel_index(target->x, target->y, support_z)) == 0)
    {
        return;
    }

    P2 p = iso_project(target->x, target->y, support_z);
    blit_face_blend(&spr_top, p.x, p.y, rgba(0, 0, 0, alpha));
}

static void draw_face_outline_edges(FaceKind face, i32 sx, i32 sy, i32 thickness, Color color) {
    P2 top = { sx, sy };
    P2 left = { sx - (i32)tile_w / 2, sy + (i32)tile_h / 2 };
    P2 right = { sx + (i32)tile_w / 2, sy + (i32)tile_h / 2 };
    P2 left_bottom = { sx - (i32)tile_w / 2, sy + (i32)block_h };
    P2 right_bottom = { sx + (i32)tile_w / 2, sy + (i32)tile_h / 2 + (i32)block_h };
    P2 bottom = { sx, sy + (i32)tile_h + (i32)block_h };

    switch (face) {
        case FACE_TOP:
            draw_fb_line(top, left, thickness, color);
            draw_fb_line(top, right, thickness, color);
            break;
        case FACE_LEFT:
            draw_fb_line(left, left_bottom, thickness, color);
            draw_fb_line(left_bottom, bottom, thickness, color);
            break;
        case FACE_RIGHT:
            draw_fb_line(right, right_bottom, thickness, color);
            draw_fb_line(bottom, right_bottom, thickness, color);
            break;
    }
}

static void draw_block_outline(i32 x, i32 y, i32 z, bool only_visible_faces) {
    i32   thickness = (i32)math_max(1, (i32)fb_px_of_dp(2.0f));
    Color color = rgba(255, 255, 255, 230);

    if (!only_visible_faces) {
        P2 p = iso_project(x, y, z);
        draw_face_outline_edges(FACE_TOP, p.x, p.y, thickness, color);
        draw_face_outline_edges(FACE_LEFT, p.x, p.y, thickness, color);
        draw_face_outline_edges(FACE_RIGHT, p.x, p.y, thickness, color);
        return;
    }

    for (u32 i = 0; i < faces_len; i++) {
        FaceInst* f = &faces_sorted[i];
        if (f->x != x || f->y != y || f->z != z)
            continue;
        draw_face_outline_edges(face_inst_face(f), f->sx, f->sy, thickness, color);
    }
}

static f32 impact_unit(u32 seed) {
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return (f32)(seed & 0xffffu) / 65535.0f;
}

static void start_impact(i32 x, i32 y, i32 z, u8 color) {
    impact_anim = (ImpactAnim){
        .valid = true,
        .x = x,
        .y = y,
        .z = z,
        .color = color,
        .elapsed = 0.0f,
    };

    P2  base = iso_project(x, y, z > 0 ? z - 1 : z);
    u32 seed = (u32)(x * 73856093) ^ (u32)(y * 19349663) ^ (u32)(z * 83492791);

    for (u32 i = 0; i < IMPACT_PARTICLES; i++) {
        f32 r0 = impact_unit(seed + i * 7u);
        f32 r1 = impact_unit(seed + i * 11u + 3u);
        f32 r2 = impact_unit(seed + i * 13u + 5u);
        f32 side = r0 < 0.5f ? -1.0f : 1.0f;
        f32 speed = fb_px_of_dp(30.0f + r1 * 45.0f);

        impact_anim.particle[i] = (ImpactParticle){
            .x = (f32)base.x + (r1 - 0.5f) * (f32)tile_w * 0.75f,
            .y = (f32)base.y + (f32)tile_h * 0.55f + (r2 - 0.5f) * (f32)tile_h * 0.55f,
            .vx = side * speed * (0.45f + r2 * 0.55f),
            .vy = -speed * (0.25f + r0 * 0.35f),
            .life = (u8)(210u + (u32)(r2 * 45.0f)),
        };
    }
}

static bool update_impact(f32 dt) {
    if (!impact_anim.valid)
        return false;

    impact_anim.elapsed += dt;
    if (impact_anim.elapsed >= IMPACT_DURATION) {
        impact_anim = (ImpactAnim){ 0 };
        return true;
    }

    return true;
}

static Vec2 impact_shake(void) {
    if (!impact_anim.valid)
        return vec2(0.0f, 0.0f);

    f32 t = clamp_0_1(impact_anim.elapsed / IMPACT_DURATION);
    f32 amp = (1.0f - t) * 0.8f;
    return vec2(math_sin(t * PI * 10.0f) * amp, math_sin(t * PI * 14.0f) * amp * 0.45f);
}

static void draw_impact_particles(void) {
    if (!impact_anim.valid)
        return;

    f32 t = clamp_0_1(impact_anim.elapsed / IMPACT_DURATION);
    f32 sx = fb_px_of_dp(viewport_shake.x);
    f32 sy = fb_px_of_dp(viewport_shake.y);
    for (u32 i = 0; i < IMPACT_PARTICLES; i++) {
        ImpactParticle* p = &impact_anim.particle[i];
        f32             px = p->x + sx + p->vx * impact_anim.elapsed;
        f32             py = p->y + sy + p->vy * impact_anim.elapsed
                           + fb_px_of_dp(95.0f) * impact_anim.elapsed * impact_anim.elapsed;
        f32             prev_px = px - p->vx * 0.035f;
        f32             prev_py = py - p->vy * 0.035f;
        u8              alpha = (u8)((f32)p->life * (1.0f - t));
        i32             size = i < 5 ? 5 : 3;
        Color           color = i < 5 ? rgba(255, 245, 170, alpha) : rgba(190, 160, 115, alpha);

        draw_fb_line(
            (P2){ (i32)math_round(prev_px), (i32)math_round(prev_py) },
            (P2){ (i32)math_round(px), (i32)math_round(py) },
            size,
            color);
    }
}

static void render_impact_overlay(void) {
    if (!impact_anim.valid)
        return;

    f32 t = clamp_0_1(impact_anim.elapsed / IMPACT_DURATION);
    u8  alpha = (u8)(150.0f * (1.0f - t));

    if (interaction_mode != Mode_ADD)
        draw_block_outline(impact_anim.x, impact_anim.y, impact_anim.z, false);
    draw_block_overlay(impact_anim.x, impact_anim.y, impact_anim.z, impact_anim.color, alpha);
    draw_impact_particles();
}

static void render_interaction_overlay(void) {
    if (interaction_mode != Mode_ADD && selected_block.valid)
        draw_block_outline(selected_block.x, selected_block.y, selected_block.z, true);

    if (interaction_mode == Mode_ADD && add_preview.valid) {
        draw_preview_shadow(&add_preview, 75);
        draw_block_overlay(
            add_preview.x, add_preview.y, add_preview.z, selected_palette_slot + 1u, 105);
    }

    render_impact_overlay();
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
    viewport_shake = impact_shake();

    clear_fb(rgb(200, 200, 200));
    build_face_list();
    sort_faces_counting();
    render_faces();
    render_interaction_overlay();

    texture_write(fb_tex, 0, 0, fb_w, fb_h, fb_pixels);

    f32 w = pes.screen.width;
    f32 h = pes.screen.height;
    f32 x = (pes.screen.width - w) * 0.5f;
    f32 y = (pes.screen.height - h) * 0.5f;
    draw_texture(x, y, w, h, fb_tex);

    viewport_shake = vec2(0.0f, 0.0f);
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

static void refresh_hover(void) {
    hover_face = (FaceHit){ 0 };
    add_preview = (CellTarget){ 0 };

    if (fb_w == 0 || fb_h == 0)
        return;

    build_face_list();
    sort_faces_counting();

    hover_face = pick_visible_face();
    if (interaction_mode == Mode_ADD)
        add_preview = add_target_from_hover();
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

static bool update_palette_selection(void) {
    bool changed = false;

    for (u8 i = 0; i < 10; i++) {
        if (!key_pressed((KeyboardKey)(Key_0 + i)))
            continue;

        selected_palette_slot = i;
        changed = true;
    }

    return changed;
}

static bool handle_mouse_interaction(void) {
    bool changed = false;

    if (pes.mouse.pressed & MOUSE_SECONDARY) {
        interaction_mode = interaction_mode == Mode_SELECT ? Mode_ADD : Mode_SELECT;
        pes_log_debug("blocks mode -> %s", interaction_mode == Mode_SELECT ? "select" : "add");
        changed = true;
    }

    refresh_hover();

    if (pes.mouse.pressed) {
        pes_log_debug(
            "blocks mouse pressed=%04x held=%04x mode=%s hover=%u add_preview=%u",
            (u32)pes.mouse.pressed,
            (u32)pes.mouse.held,
            interaction_mode == Mode_SELECT ? "select" : "add",
            (u32)hover_face.valid,
            (u32)add_preview.valid);
    }

    if ((pes.mouse.pressed & MOUSE_PRIMARY) && !key_held(Key_Space)) {
        if (interaction_mode == Mode_SELECT) {
            selected_block.valid = hover_face.valid;
            if (hover_face.valid) {
                selected_block.x = hover_face.face.x;
                selected_block.y = hover_face.face.y;
                selected_block.z = hover_face.face.z;
                pes_log_debug(
                    "blocks select (%d,%d,%d)",
                    selected_block.x,
                    selected_block.y,
                    selected_block.z);
            }
            changed = true;
        } else if (add_preview.valid) {
            u8 color = selected_palette_slot + 1u;
            voxel_set(add_preview.x, add_preview.y, add_preview.z, color);
            selected_block = (SelectedBlock){
                .valid = true,
                .x = add_preview.x,
                .y = add_preview.y,
                .z = add_preview.z,
            };
            start_impact(add_preview.x, add_preview.y, add_preview.z, color);
            refresh_hover();
            pes_log_debug(
                "blocks add (%d,%d,%d) color=%u",
                selected_block.x,
                selected_block.y,
                selected_block.z,
                (u32)color);
            changed = true;
        }
    }

    return changed;
}

static bool update(f32 dt) {
    bool changed = update_camera(dt);

    changed = update_impact(dt) || changed;
    changed = update_palette_selection() || changed;
    changed = handle_mouse_interaction() || changed;

    if (key_held(Key_Space)) {
        pes.mouse.cursor = Cursor_OPEN_HAND;
    } else if (interaction_mode == Mode_ADD) {
        pes.mouse.cursor = add_preview.valid ? Cursor_CROSSHAIR : Cursor_NOT_ALLOWED;
    } else {
        pes.mouse.cursor = Cursor_DEFAULT;
    }

    if (pes.events & EV_MOUSE)
        changed = true;

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
