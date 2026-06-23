#include "pes.h"

_Static_assert(
    offsetof(Gamepad, have_button) % 4 == 0,
    "can load {have_button,have_axis,have_haptics} as u32");

_Static_assert(1u << GamepadHaptics_DEFAULT == PBSysHidHaptics_DEFAULT, "");
_Static_assert(1u << GamepadHaptics_ALL == PBSysHidHaptics_ALL, "");
_Static_assert(1u << GamepadHaptics_HANDLES == PBSysHidHaptics_HANDLES, "");
_Static_assert(1u << GamepadHaptics_LEFT_HANDLE == PBSysHidHaptics_LEFT_HANDLE, "");
_Static_assert(1u << GamepadHaptics_RIGHT_HANDLE == PBSysHidHaptics_RIGHT_HANDLE, "");
_Static_assert(1u << GamepadHaptics_TRIGGERS == PBSysHidHaptics_TRIGGERS, "");
_Static_assert(1u << GamepadHaptics_LEFT_TRIGGER == PBSysHidHaptics_LEFT_TRIGGER, "");
_Static_assert(1u << GamepadHaptics_RIGHT_TRIGGER == PBSysHidHaptics_RIGHT_TRIGGER, "");

#define assert_pes_initialized() \
    assertf(pes_internal.window.handle, "You forgot to call pes_init()")

#define pes_log_info(fmt, ...)  log("[pes] " fmt, ##__VA_ARGS__)
#define pes_log_error(fmt, ...) log("[pes %s] error: " fmt, __func__, ##__VA_ARGS__)
#define pes_log_debug(fmt, ...) \
    log("[pes debug]: " fmt " (%s %s:%d)", ##__VA_ARGS__, __func__, __FILE__, __LINE__)

#define DRAW_TRANSFORM_STACK_CAP 32u

#if __playbit__ > 0x000300
    #define PES_DRAW_USE_PB_TRANSFORM 1
#else
    #define PES_DRAW_USE_PB_TRANSFORM 0
#endif

#if !PES_DEBUG
    #undef PES_DEBUG
    #define PES_DEBUG 0
    #undef pes_log_debug
    #define pes_log_debug(fmt, ...) ((void)0)
#endif

#if PES_DISABLE_INFO_LOG
    #undef pes_log_info
    #define pes_log_info(fmt, ...) ((void)0)
#endif

typedef struct PesDrawing {
    Color     clear_color;
    u32       shapes_len;
    u32       instrs_len;
    u32       shape_run_len;
    Texture   texture; // current texture, from draw_set_texture
    Transform transform;
    Transform transform_stack[DRAW_TRANSFORM_STACK_CAP];
    u32       transform_stack_len;

    // starting at 'shapes', the remainder of this struct data is uninitialized
    PBSysWindowRendererShapeItem   shapes[512];
    PBSysWindowRendererInstruction instrs[512];
} PesDrawing;

static struct {
    bool                 screen_draw; // screen_draw() called since last pes_draw() call
    bool                 frame_ready; // true if we have seen a FRAME_SYNC event but not yet drawn
    bool                 draw;        // should call pes_draw (even if pes.draw==false)
    bool                 done_first_draw;
    bool                 app_active;
    bool                 fullscreen;
    PBWindow             window;
    PBTimer              update_timer;
    PBTime               update_time; // last time pes_update was called
    PesDrawing* nullable drawing;
    Events               pending_events;
    struct {
        Cursor cursor;
        bool   visible;
    } mouse;
} pes_internal = {
    .mouse.cursor = Cursor_DEFAULT,
    .mouse.visible = true,
};

struct PES pes = {
    .ent.id_pool.use_bm = pes.ent.id_pool.use_bm_storage,
    .ent.id_pool.use_bm_cap = sizeof(pes.ent.id_pool.use_bm_storage) * 8,
    .mouse.cursor = Cursor_DEFAULT,
    .mouse.visible = true,
};

UNUSED static const StrSlice kPesEmptyStrSlice = { .v = (const u8*)"" };

UNUSED static const u32 kPesEmptyStr_data[2] = { 0 };
UNUSED static const Str kPesEmptyStr = (char*)&kPesEmptyStr_data[1];

static u32 pes_Key_of_PBKeyboardKey(PBKeyboardKey key);

////////////////////////////////////////////////////////////////////////////////////////////////////
// math

f32 math_atan2_f32(f32 y, f32 x) {
    // Fast approximate atan2f. Assumes finite x/y.
    // Error is roughly below 0.01 rad for normal inputs.

    const f32 PI_4 = 0.78539816339744830962f;
    const f32 PI_3_4 = 2.35619449019234492885f;

    f32 ax = math_abs(x);
    f32 ay = math_abs(y);

    if (ax == 0.0f && ay == 0.0f)
        return 0.0f;

    // scale to avoid overflow in x + ay
    f32 s = ax > ay ? ax : ay;
    x /= s;
    ay /= s;

    f32 r, angle;

    if (x < 0.0f) {
        r = (x + ay) / (ay - x);
        angle = PI_3_4;
    } else {
        r = (x - ay) / (x + ay);
        angle = PI_4;
    }

    angle += (0.1963f * r * r - 0.9817f) * r;

    return y < 0.0f ? -angle : angle;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// arena

void* arena_alloc(usize nbyte) {
    nbyte = PBAlign2(nbyte, 8);
    usize cap = sizeof(pes.arena.bytes);
    usize avail = cap - pes.arena.used;
    if UNLIKELY (avail < nbyte)
        panic("out of memory");
    void* ptr = &pes.arena.bytes[pes.arena.used];
    pes.arena.used += nbyte;
    return ptr;
}

void* arena_allocz(usize nbyte) {
    void* ptr = arena_alloc(nbyte);
    memset(ptr, 0, nbyte);
    return ptr;
}

void arena_free(void* ptr, usize nbyte) {
    nbyte = PBAlign2(nbyte, 8);
    u8* ptr_end = (u8*)ptr + nbyte;
    assertf(
        (uintptr)ptr >= (uintptr)pes.arena.bytes
            && (uintptr)ptr_end <= (uintptr)&pes.arena.bytes[sizeof(pes.arena.bytes)],
        "%p not allocated from arena",
        ptr);
    if (ptr_end == &pes.arena.bytes[sizeof(pes.arena.used)])
        pes.arena.used -= nbyte;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// id_pool

bool id_pool_is_valid(const IdPool* pool, u32 id) {
    if (id == 0 || id > pool->max_id)
        return false;
    u32 chunk_idx = (id - 1) >> 6;     // (id-1)/64
    u32 bit_idx = (id - 1) & (64 - 1); // (id-1)%64
    return pool->use_bm[chunk_idx] & ((u64)1 << bit_idx);
}

u32 id_pool_alloc(IdPool* pool) {
    for (u32 chunk_idx = 0, nchunks = pool->use_bm_cap / 64; chunk_idx < nchunks; chunk_idx++) {
        u64 bm = pool->use_bm[chunk_idx];

        // find first free (0) bit in this chunk
        u32 bit_idx = __builtin_ffsll(~bm);

        if LIKELY (bit_idx) {
            // found a free block; mark it as used by setting bit (bit_idx-1) to 0
            u32 id = chunk_idx * 64 + bit_idx; // note: id is 1-based, not 0-based
            pool->use_bm[chunk_idx] = bm | ((u64)1 << (bit_idx - 1));
            if (id > pool->max_id)
                pool->max_id = id;
            return id;
        }
        // all blocks of this chunk are in use; try next chunk
    }
    return 0;
}

void id_pool_free(IdPool* pool, u32 id) {
    assertf(id_pool_is_valid(pool, id), "#%u not allocated", id);

    u32 chunk_idx = (id - 1) >> 6;     // (id-1)/64
    u32 bit_idx = (id - 1) & (64 - 1); // (id-1)%64

    pool->use_bm[chunk_idx] &= ~((u64)1 << bit_idx);

    if (id != pool->max_id)
        return;

    // find new max_id
    for (;;) {
        u64 chunk = pool->use_bm[chunk_idx];
        if (chunk == ~(u64)0) {
            // chunk is full and next chunk is empty
            pool->max_id = (chunk_idx + 1) << 6;
            break;
        } else if (chunk) {
            // chunk has at least one 0 bit (used entry).
            // Find the last used entry in this chunk.
            pool->max_id = chunk_idx * 64 + (64 - __builtin_clzll(chunk));
            break;
        } else {
            // chunk is completely free
            if (chunk_idx == 0) {
                pool->max_id = 0;
                break;
            }
            chunk_idx--;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// ent

Ent ent_create(const char* debug_name) {
    Ent ent = id_pool_alloc(&pes.ent.id_pool);

#if PB_DEBUG
    PBStr*   sp = array_assign_ent(ent, &pes.ent.debug_names);
    StrSlice s = StrSliceOf(debug_name);
    if (s.len == 0) {
        sp->len = 0;
    } else {
        PBStrAssign(sp, kMemGPA, StrSliceOf(debug_name));
    }
#endif

    return ent;
}

void ent_del(Ent ent) {
    id_pool_free(&pes.ent.id_pool, ent);
}

StrSlice ent_debug_name_slice(Ent ent) {
#if PB_DEBUG
    StrSlice s = StrSliceOf(pes.ent.debug_names.v[ent - 1]);
    if (s.len > 0)
        return s;
#endif
    return kPesEmptyStrSlice;
}

const char* ent_debug_name(Ent ent) {
    return (const char*)ent_debug_name_slice(ent).v;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// screen / window

static bool pes_window_read_info(void) {
    PBSysWindowInfo info;
    PanicOnErr(PBSysWindowInfoGet(pes_internal.window.handle, &info, sizeof(info)));

    if (pes.screen.scale == info.dpScale && pes.screen.width == info.contentWidth
        && pes.screen.height == info.contentHeight)
    {
        // no change
        return false;
    }

    pes.screen.scale = info.dpScale;
    pes.screen.width = info.contentWidth;
    pes.screen.height = info.contentHeight;

    pes_log_debug(
        "screen resized to %g x %g @ %.0f%%",
        pes.screen.width,
        pes.screen.height,
        pes.screen.scale * 100.0f);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// draw

u64 color_argb16(Color c) {
    // 0xAABBGGRR -> 0xAAAARRRRGGGGBBBB
    const u64 e = 0x0101;
    return (e * c.a << 48) | (e * c.r << 32) | (e * c.g << 16) | e * c.b;
}

Color hsv(f32 h, f32 s, f32 v) {
    s = clamp_0_1(s);
    v = clamp_0_1(v);

    // wrap hue to [0, 360)
    while (h < 0.0f)
        h += 360.0f;
    while (h >= 360.0f)
        h -= 360.0f;

    f32 hh = h / 60.0f;
    int i = (int)hh;     // 0..5
    f32 f = hh - (f32)i; // fractional part within sector

    f32 p = v * (1.0f - s);
    f32 q = v * (1.0f - s * f);
    f32 t = v * (1.0f - s * (1.0f - f));

    f32 r, g, b;

    // clang-format off
    switch (i) {
        default:
        case 0: r = v; g = t; b = p; break; // red -> yellow
        case 1: r = q; g = v; b = p; break; // yellow -> green
        case 2: r = p; g = v; b = t; break; // green -> cyan
        case 3: r = p; g = q; b = v; break; // cyan -> blue
        case 4: r = t; g = p; b = v; break; // blue -> magenta
        case 5: r = v; g = p; b = q; break; // magenta -> red
    }
    // clang-format on

    return (Color){
        .r = (u8)(clamp_0_1(r) * 255.0f + 0.5f),
        .g = (u8)(clamp_0_1(g) * 255.0f + 0.5f),
        .b = (u8)(clamp_0_1(b) * 255.0f + 0.5f),
        .a = 255,
    };
}

inline static f32 pes_hue_to_rgb(f32 p, f32 q, f32 t) {
    if (t < 0.0f)
        t += 1.0f;
    if (t > 1.0f)
        t -= 1.0f;

    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * ((2.0f / 3.0f) - t) * 6.0f;
    return p;
}

Color hsl(f32 h, f32 s, f32 l) {
    s = clamp_0_1(s);
    l = clamp_0_1(l);

    while (h < 0.0f)
        h += 360.0f;
    while (h >= 360.0f)
        h -= 360.0f;

    f32 r, g, b;

    if (s == 0.0f) {
        // achromatic gray
        r = g = b = l;
    } else {
        h /= 360.0f;

        f32 q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;

        f32 p = 2.0f * l - q;

        r = pes_hue_to_rgb(p, q, h + 1.0f / 3.0f);
        g = pes_hue_to_rgb(p, q, h);
        b = pes_hue_to_rgb(p, q, h - 1.0f / 3.0f);
    }

    return (Color){
        .r = (u8)(clamp_0_1(r) * 255.0f + 0.5f),
        .g = (u8)(clamp_0_1(g) * 255.0f + 0.5f),
        .b = (u8)(clamp_0_1(b) * 255.0f + 0.5f),
        .a = 255,
    };
}

static PBSysWindowRendererInstruction* pes_draw_add_instrs(u32 count) {
    assert(pes_internal.drawing->instrs_len + count <= countof(pes_internal.drawing->instrs));
    PBSysWindowRendererInstruction* instr =
        &pes_internal.drawing->instrs[pes_internal.drawing->instrs_len];
    pes_internal.drawing->instrs_len += count;
    return instr;
}

static void pes_draw_end_shape_run(void) {
    assert(pes_internal.drawing->shape_run_len > 0);
    PBSysWindowRendererInstruction* instr = pes_draw_add_instrs(1);
    *instr = (PBSysWindowRendererInstruction){
        .kind = PBSysWindowRendererInstructionKind_SHAPE,
        .content.runLength = pes_internal.drawing->shape_run_len,
    };
    pes_internal.drawing->shape_run_len = 0;
}

static Transform pes_draw_transform_px(void) {
    Transform transform = pes_internal.drawing->transform;
    transform.o[0] = px_of_dp(transform.o[0]);
    transform.o[1] = px_of_dp(transform.o[1]);
    return transform;
}

#if PB_DEBUG && !PES_DRAW_USE_PB_TRANSFORM

static bool pes_draw_transform_position_only(Transform transform) {
    return transform.x[0] == 1.0f && transform.x[1] == 0.0f && transform.y[0] == 0.0f
        && transform.y[1] == 1.0f;
}

static void pes_draw_assert_position_only(Transform transform) {
    assertf(
        pes_draw_transform_position_only(transform),
        "draw transforms beyond translation require Playbit > 0.3.0");
}

#else

    #define pes_draw_assert_position_only(transform) ((void)0)

#endif

void draw_push(void) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");
    if UNLIKELY (drawing->transform_stack_len == countof(drawing->transform_stack))
        panic("draw transform stack overflow");
    drawing->transform_stack[drawing->transform_stack_len++] = drawing->transform;
}

void draw_pop(void) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");
    if UNLIKELY (drawing->transform_stack_len == 0)
        panic("draw transform stack underflow");
    drawing->transform = drawing->transform_stack[--drawing->transform_stack_len];
}

Transform draw_get_transform(void) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");
    return drawing->transform;
}

Transform draw_transform(Transform next) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");
    pes_draw_assert_position_only(next);
    Transform prev = drawing->transform;
    drawing->transform = next;
    return prev;
}

void draw_translate(f32 x, f32 y) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");
    drawing->transform = transform_translate(drawing->transform, x, y);
}

void draw_scale(f32 x, f32 y) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");
    drawing->transform = transform_scale(drawing->transform, x, y);
    pes_draw_assert_position_only(drawing->transform);
}

void draw_rotate(f32 radians) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");
    drawing->transform = transform_rotate(drawing->transform, radians);
    pes_draw_assert_position_only(drawing->transform);
}

Shape draw_shape(f32 x, f32 y, f32 w, f32 h) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");

    x = px_of_dp(x);
    y = px_of_dp(y);
    w = px_of_dp(w);
    h = px_of_dp(h);

    if UNLIKELY (drawing->shapes_len == countof(drawing->shapes))
        panic("too many shapes (%u)", drawing->shapes_len);

    Transform transform = pes_draw_transform_px();
#if !PES_DRAW_USE_PB_TRANSFORM
    pes_draw_assert_position_only(transform);
    x += transform.o[0];
    y += transform.o[1];
    PBSysWindowRendererShapeItem* shape = &drawing->shapes[drawing->shapes_len++];
    *shape = (PBSysWindowRendererShapeItem){
        .bounds = { x, y, x + w, y + h },
        .opacity = 1.0,
        .uvMax = { 1, 1 },
    };
#else
    PBSysWindowRendererShapeItem* shape = &drawing->shapes[drawing->shapes_len++];
    *shape = (PBSysWindowRendererShapeItem){
        .bounds = { x, y, x + w, y + h },
        .opacity = 1.0,
        .uvMax = { 1, 1 },
        .transformX = { transform.x[0], transform.x[1] },
        .transformY = { transform.y[0], transform.y[1] },
        .transformO = { transform.o[0], transform.o[1] },
    };
#endif

    drawing->shape_run_len++;

    return shape;
}

Shape draw_shape_uv(f32 x, f32 y, f32 w, f32 h, Edges uv) {
    PBSysWindowRendererShapeItem* shape = draw_shape(x, y, w, h);
    shape->uvMin.x = uv.left;
    shape->uvMin.y = uv.top;
    shape->uvMax.x = uv.right;
    shape->uvMax.y = uv.bottom;
    // need fill to display since textures are blended with the fill color
    shape->fillColor = ~0ull;
    return shape;
}

Shape draw_circle(Vec2 center_pos, f32 radius) {
    f32   cx = center_pos.x - radius;
    f32   cy = center_pos.y - radius;
    Shape shape = draw_shape(cx, cy, radius * 2.0f, radius * 2.0f);
    return shape_corner_radius(shape, radius);
}

Texture draw_set_texture(Texture tex) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing, "called outside of pes_draw()");

    Texture previous_tex = drawing->texture;
    drawing->texture = tex;

    if (tex.handle != previous_tex.handle) {
        if (drawing->shape_run_len > 0)
            pes_draw_end_shape_run();

        PBSysWindowRendererInstruction* instr = pes_draw_add_instrs(1);
        *instr = (PBSysWindowRendererInstruction){
            .kind = PBSysWindowRendererInstructionKind_TEXTURE_SET,
            .setTexture.texture = tex.handle,
        };
    }

    return previous_tex;
}

Shape draw_texture(f32 x, f32 y, f32 w, f32 h, Texture tex) {
    Texture previous_tex = draw_set_texture(tex);
    Shape   shape = draw_shape(x, y, w, h);
    shape->fillColor = ~0ull;
    shape->uvMax.x = 1;
    shape->uvMax.y = 1;
    draw_set_texture(previous_tex);
    return shape;
}

static void pes_draw_begin(void) {
    assertf(pes_internal.drawing == NULL, "pes_draw_begin() called twice");
    pes_internal.drawing = arena_alloc(sizeof(PesDrawing));
    memset(pes_internal.drawing, 0, offsetof(PesDrawing, shapes));
    pes_internal.drawing->transform = transform_identity();
}

static void pes_draw_end(void) {
    PesDrawing* drawing = pes_internal.drawing;
    assertf(drawing != NULL, "pes_draw_end() called without pes_draw_begin()");

    if (drawing->shapes_len > 0 || !pes_internal.done_first_draw) {
        if (drawing->shape_run_len > 0)
            pes_draw_end_shape_run();
        // pes_log_debug(
        //     "render submit shapes=%u instrs=%u", drawing->shapes_len, drawing->instrs_len);
        PBSysWindowRendererPackage package = {
            .backgroundColor = color_argb16(pes.screen.clear_color),
            .shapes = drawing->shapes,
            .shapesLen = drawing->shapes_len,
            .shapeSize = sizeof(*package.shapes),
            .instructions = drawing->instrs,
            .instructionsLen = drawing->instrs_len,
            .instructionSize = sizeof(*package.instructions),
            .textSize = sizeof(*package.texts),
            .clipSize = sizeof(*package.clips),
        };
        PanicOnErr(
            PBSysWindowRendererPackageWrite(pes_internal.window.handle, &package, sizeof(package)));
    }

    pes_internal.drawing = NULL;
    pes_internal.done_first_draw = true;
}

Shape shape_fill(Shape shape, Color color) {
    shape->fillColor = color_argb16(color);
    return shape;
}

Shape shape_stroke(Shape shape, Color color, f32 thickness) {
    shape->strokeColor = color_argb16(color);
    shape->strokeWidth = px_of_dp(thickness);
    // 1=stroke_inside, 2=stroke_outside, 4=clip
    shape->flags &= ~(1 | 2); // clear any existing alignment flags so we get "center"
    return shape;
}

Shape shape_stroke_inner(Shape shape, Color color, f32 thickness) {
    shape_stroke(shape, color, thickness);
    shape->flags |= 1;
    return shape;
}

Shape shape_stroke_outer(Shape shape, Color color, f32 thickness) {
    shape_stroke(shape, color, thickness);
    shape->flags |= 2;
    return shape;
}

Shape shape_corner_radius(Shape shape, f32 radius) {
    radius = px_of_dp(radius);
    shape->cornerRadius.x = radius;
    shape->cornerRadius.y = radius;
    shape->cornerRadius.z = radius;
    shape->cornerRadius.w = radius;
    return shape;
}

Shape shape_corner_radius4(Shape shape, f32 tl, f32 tr, f32 br, f32 bl) {
    shape->cornerRadius.x = px_of_dp(tl);
    shape->cornerRadius.y = px_of_dp(tr);
    shape->cornerRadius.z = px_of_dp(br);
    shape->cornerRadius.w = px_of_dp(bl);
    return shape;
}

Shape shape_set_uv(Shape shape, Edges uv) {
    shape->uvMin.x = uv.left;
    shape->uvMin.y = uv.top;
    shape->uvMax.x = uv.right;
    shape->uvMax.y = uv.bottom;
    return shape;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// texture

Texture texture_load(const char* resource_name) {
    assert_pes_initialized();

    PBSysTextureFormat            format = PBSysTextureFormat_RGBA8;
    PBSysWindowCreateTextureFlags flags = PBSysWindowCreateTextureFlag_NEAREST;
    PBAppResource                 res = PBAppResourceWithName(PBStrSliceOf(resource_name));

    PBSysHandle handle = PBSysWindowCreateTextureFromData(
        pes_internal.window.handle, format, res.data.v, res.data.len, flags);

    if UNLIKELY (handle <= 0) {
        pes_log_error(
            "failed to load \"%.*s\" (%zu B): %s",
            PBStr_FMT_ARG(res.name),
            res.data.len,
            PBSysErrStr(handle));
    }

    return (PBTexture){ .handle = handle };
}

Texture texture_create(u32 width, u32 height, TextureFlags flags) {
    assert_pes_initialized();
    PBSysTextureFormat format = PBSysTextureFormat_RGBA8;
    i32 handle = PBSysWindowCreateTexture(pes_internal.window.handle, format, width, height, flags);
    if UNLIKELY (handle <= 0)
        pes_log_error("(%u, %u): %s", width, height, PBSysErrStr(handle));
    return (Texture){ handle };
}

void texture_close(Texture tex) {
    PBTextureDestroy(&tex);
}

void texture_write(Texture tex, u32 x_px, u32 y_px, u32 w_px, u32 h_px, const Color* pixels) {
    usize pixels_size = (usize)w_px * (usize)h_px * 4ul;
    PanicOnErr(PBSysTextureWrite(tex.handle, x_px, y_px, pixels, pixels_size, w_px, h_px));
}

Edges texture_uv_of_rect(f32 tex_width, f32 tex_height, Rect rect) {
    f32 top = rect.y / tex_height;
    f32 left = rect.x / tex_width;
    return (Edges){
        .top = top,
        .right = left + (rect.height / tex_width),
        .bottom = top + (rect.width / tex_height),
        .left = left,
    };
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// gamepad

static u32 pes_gamepad_index_of_id(u32 deviceId) {
    for (u32 idx = 0; idx < GAMEPAD_COUNT; idx++) {
        if (pes.gamepad[idx].deviceId == deviceId)
            return idx;
    }
    return GAMEPAD_COUNT;
}

static u32 pes_gamepad_alloc(u32 deviceId) {
    for (u32 idx = 0; idx < GAMEPAD_COUNT; idx++) {
        if (!gamepad_available(idx)) {
            // unused
            return idx;
        }
    }
    return GAMEPAD_COUNT;
}

Str gamepad_name(u32 player) {
    assert(player < GAMEPAD_COUNT);
    i32 nbyte = PBHidDeviceName(pes.gamepad[player].deviceId, NULL, 0);
    if (nbyte < 0)
        return kPesEmptyStr;
    StrHeader* str = arena_alloc(4 + nbyte + 1);
    i32 n = Min(nbyte, PBHidDeviceName(pes.gamepad[player].deviceId, (u8*)str->chars, nbyte));
    if UNLIKELY (n <= 0) {
        arena_free(str, 4 + nbyte + 1);
        return kPesEmptyStr;
    }
    str->len = n;
    str->chars[n] = 0;
    return str->chars;
}

static void pes_gamepad_set_player_index(u32 idx, u32 do_assign) {
    assert(idx < 4); // PBHidDeviceSetPlayerIndex only supports 4 players
    u32      playerIndex = (idx + 1) * do_assign; // 0=unset, 1..4 = player 1..4
    PBSysErr err = PBHidDeviceSetPlayerIndex(pes.gamepad[idx].deviceId, playerIndex);
    if UNLIKELY (err && err != PBSysErr_NOT_SUPPORTED)
        pes_log_error("%s", PBSysErrStr(err));
}

static void pes_gamepad_disable(u32 idx) {
    pes_log_info("disable gamepad #%u", idx);

    if (idx != GAMEPAD_COUNT && gamepad_available(idx)) {
        memset(&pes.gamepad[idx], 0, sizeof(pes.gamepad[idx]));
        if (idx < 4)
            pes_gamepad_set_player_index(idx, 0);
    }
}

static void pes_gamepad_enable(u32 idx) {
    PBSysHidDeviceInfo device_info;
    PBSysErr           err = PBHidDeviceInfoGet(pes.gamepad[idx].deviceId, &device_info);
    if (err)
        return pes_gamepad_disable(idx);

    // ok since
    // - enum GamepadButton == enum PBSysGamepadButton
    // - enum GamepadAxis == enum PBSysGamepadAxis
    pes.gamepad[idx].have_button = (u16)device_info.gamepadButtonMask;
    pes.gamepad[idx].have_axis = (u8)device_info.gamepadAxisMask;

    // ok since PBSysHidHapticsLocality mask values match effective GamepadHaptics bit positions
    if (device_info.flags & PBSysHidDevice_HAS_HAPTICS)
        pes.gamepad[idx].have_haptics = (u8)device_info.gamepadAxisMask;

    pes_log_info(
        "enable gamepad #%u \"%s\" device=%u have_{button,axis,haptics}={0b%013b,0b%08b,0b%08b}",
        idx,
        gamepad_name(idx),
        pes.gamepad[idx].deviceId,
        pes.gamepad[idx].have_button,
        pes.gamepad[idx].have_axis,
        pes.gamepad[idx].have_haptics);

    // Check if {have_button,have_axis,have_haptics} are all zero.
    // We use {have_button,have_axis,have_haptics} to determine if a gamepad is available.
    if UNLIKELY (!gamepad_available(idx)) {
        // ignore gamepad with no abilities
        pes_gamepad_disable(idx);
        return;
    }

    if ((device_info.flags & PBSysHidDevice_HAS_PLAYER_INDEX) && idx < 4) {
        pes_log_info("assign player #%u to gamepad #%u", idx + 1, idx);
        pes_gamepad_set_player_index(idx, 1);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// pointer

static void pes_cursor_update(void) {
    bool was_visible = pes_internal.mouse.visible;

    pes_internal.mouse.cursor = pes.mouse.cursor;
    pes_internal.mouse.visible = pes.mouse.visible;

    if (!pes.mouse.visible && was_visible == pes.mouse.visible) {
        // cursor style changed, but it's still hidden
        return;
    }

    PBSysCursorStyle      style = pes.mouse.cursor;
    PBSysHandle           image = 0;
    PBSysCursorStyleFlags flags = pes.mouse.visible ? 0 : PBSysCursorStyleFlag_HIDDEN;
    PanicOnErr(PBSysWindowSetCursor(PBWindowMain().handle, style, image, flags));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// event

UNUSED static void pes_log_debug_event(const PBEvent* event) {
    char buf[512];
    PBEventStr(event, buf, sizeof(buf));
    pes_log_debug("%s", buf);
}

UNUSED static void pes_log_debug_events(PBEvent* events, i32 n) {
    pes_log_debug("—— %d events:", n);
    for (PBEvent* ev = events; n--; ev = PBEventNext(ev))
        pes_log_debug_event(ev);
    pes_log_debug("——");
}

#if !PES_DEBUG
    #define pes_log_debug_event(...)  ((void)0)
    #define pes_log_debug_events(...) ((void)0)
#endif

static void pes_on_keyboard_event(PBKeyboardEvent* ev) {
    if (ev->inputEvent.clientId != 0 || ev->inputEvent.deviceId != 0)
        return;

    u32 key_bit = pes_Key_of_PBKeyboardKey(ev->keyCode);
    if UNLIKELY (key_bit == U32_MAX)
        return;

    bool pressed = ev->inputEvent.event.type == PBEventType_KEY_DOWN;
    bit_toggle(pes.keyboard.held, key_bit, pressed);
    bit_toggle(pes.keyboard.pressed, key_bit, pressed);

    pes.keyboard.held_count =
        __builtin_popcountll(pes.keyboard.held[0]) + __builtin_popcountll(pes.keyboard.held[1])
        + __builtin_popcountll(pes.keyboard.held[2]);
    pes.keyboard.pressed_count =
        __builtin_popcountll(pes.keyboard.pressed[0])
        + __builtin_popcountll(pes.keyboard.pressed[1])
        + __builtin_popcountll(pes.keyboard.pressed[2]);

    pes.events |= EV_INPUT | EV_KEYBOARD;

    // pes_log_debug(
    //     "keyboard:\n"
    //     "  .held =    %064llb\n"
    //     "             %064llb\n"
    //     "             %064llb\n"
    //     "  .pressed = %064llb\n"
    //     "             %064llb\n"
    //     "             %064llb\n",
    //     pes.keyboard.held[0],
    //     pes.keyboard.held[1],
    //     pes.keyboard.held[2],
    //     pes.keyboard.pressed[0],
    //     pes.keyboard.pressed[1],
    //     pes.keyboard.pressed[2]);
}

static void pes_on_window_signal_event(PBSignalEvent* ev) {
    pes_internal.app_active = ev->signals & PBSysWindowSignal_KEY;
    pes_internal.fullscreen = ev->signals & PBSysWindowSignal_FULLSCREEN;

    if (ev->signals & PBSysWindowSignal_RESIZE) {
        if (pes_window_read_info()) {
            // size and/or scale actually changed
            pes.events |= EV_RESIZE;
        }
    }

    if (ev->signals & PBSysWindowSignal_FRAME_SYNC)
        pes_internal.frame_ready = true;
}

static void pes_on_signal_event(PBSignalEvent* ev) {
    if (ev->handle == pes_internal.window.handle)
        return pes_on_window_signal_event(ev);
}

static void pes_on_gamepad_event(PBGamepadEvent* ev) {
    pes_log_debug_event((PBEvent*)ev);

    u32 deviceId = ev->inputEvent.deviceId;
    u32 idx = pes_gamepad_index_of_id(deviceId);

    if (ev->inputEvent.event.type == PBEventType_GAMEPAD_DISCONNECTED) {
        if (gamepad_available(idx))
            pes_gamepad_disable(idx);
        return;
    }

    if UNLIKELY (idx == GAMEPAD_COUNT) {
        if ((idx = pes_gamepad_alloc(deviceId)) == GAMEPAD_COUNT)
            return;
        pes.gamepad[idx].deviceId = deviceId;
        pes_gamepad_enable(idx);
    }

    if (ev->inputEvent.event.type == PBEventType_GAMEPAD_BUTTON_DOWN
        || ev->inputEvent.event.type == PBEventType_GAMEPAD_BUTTON_UP)
    {
        bool pressed = ev->inputEvent.event.type == PBEventType_GAMEPAD_BUTTON_DOWN;

        _Static_assert(offsetof(Gamepad, held) % 4 == 0, "can load as aligned u32");
        u32 prev_button_state = *(u32*)&pes.gamepad[idx].held;

        bit_toggle(&pes.gamepad[idx].held, ev->control, pressed);
        bit_toggle(&pes.gamepad[idx].pressed, ev->control, pressed);

        // pes_log_debug("pes.gamepad[%u].held:    %016b", idx, pes.gamepad[idx].held);
        // pes_log_debug("pes.gamepad[%u].pressed: %016b", idx, pes.gamepad[idx].pressed);

        u32 next_button_state = *(u32*)&pes.gamepad[idx].held;
        if (prev_button_state != next_button_state)
            pes.events |= EV_INPUT | EV_GAMEPAD;

    } else if (ev->inputEvent.event.type == PBEventType_GAMEPAD_AXIS_MOVE) {
        assertf(ev->control < countof(pes.gamepad[idx].axis_value), "%u", ev->control);
        pes.gamepad[idx].axis_value[ev->control] = ev->value;

        // pes_log_debug("pes.gamepad[%u].axis_value[%s] = %g",
        //     idx,
        //     ev->control == GamepadAxis_LEFT_X         ? "LEFT_X"
        //     : ev->control == GamepadAxis_LEFT_Y       ? "LEFT_Y"
        //     : ev->control == GamepadAxis_RIGHT_X      ? "RIGHT_X"
        //     : ev->control == GamepadAxis_RIGHT_Y      ? "RIGHT_Y"
        //     : ev->control == GamepadAxis_DPAD_X       ? "DPAD_X"
        //     : ev->control == GamepadAxis_DPAD_Y       ? "DPAD_Y"
        //     : ev->control == GamepadAxis_LEFT_TRIGGER ? "LEFT_TRIGGER"
        //     : ev->control == GamepadAxis_RIGHT_TRIGGER
        //         ? "RIGHT_TRIGGER"
        //         : "?",
        //     ev->value);
    }
}

static void pes_on_pointer_event(PBPointerEvent* ev) {
    if (ev->inputEvent.event.type == PBEventType_POINTER_DOWN
        || ev->inputEvent.event.type == PBEventType_POINTER_UP)
    {
        pes_log_debug(
            "pointer %s kind=%u flags=%04x buttons=%04x button=%u pos=(%.1f,%.1f)",
            ev->inputEvent.event.type == PBEventType_POINTER_DOWN ? "down" : "up",
            (u32)ev->kind,
            (u32)ev->flags,
            (u32)ev->buttons,
            (u32)ev->button,
            ev->x,
            ev->y);
    }

    if (!(ev->flags & PBPointerFlag_PRIMARY))
        return;
    if (ev->kind == PBPointerKind_MOUSE || ev->kind == PBPointerKind_TRACKPAD) {
        pes.mouse.origin.x = ev->x;
        pes.mouse.origin.y = ev->y;
        pes.mouse.moved.x += ev->dx;
        pes.mouse.moved.y += ev->dy;
        pes.mouse.held = ev->buttons;
        if (ev->inputEvent.event.type == PBEventType_POINTER_DOWN) {
            pes.mouse.pressed |= ev->buttons;
            pes_log_debug(
                "mouse pressed=%04x held=%04x from buttons=%04x button=%u",
                (u32)pes.mouse.pressed,
                (u32)pes.mouse.held,
                (u32)ev->buttons,
                (u32)ev->button);
        }
        pes.events |= EV_INPUT | EV_POINTER | EV_MOUSE;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void pes_init(const char* title, f32 w, f32 h, Color bg) {
    pes_internal.window = PBWindowMain();
    PBWindowSetTitle(pes_internal.window, PBStrSliceOfCStr(title));
    PBWindowSetStyle(
        pes_internal.window,
        PBSysWindowStyle_TITLEBAR | PBSysWindowStyle_TRANSPARENT_TITLEBAR
            | PBSysWindowStyle_CLOSABLE | PBSysWindowStyle_RESIZABLE | PBSysWindowStyle_MINIMIZABLE
            | PBSysWindowStyle_FULLSIZE_CONTENT);

    if (w > 0.0f && h > 0.0f)
        PBWindowSetContentSizeCentered(pes_internal.window, w, h);

    PBObserve(pes_internal.window.handle, PBSysSignal_SYSTEM_ALL);
    PanicOnErr(PBSysWindowFrameSyncEnable(pes_internal.window.handle, 1));
    pes_window_read_info();
    pes.screen.clear_color = bg;
    pes_internal.pending_events = EV_RESIZE;
}

void window_resize(f32 w, f32 h) {
    assert_pes_initialized();
    PanicOnErr(PBWindowSetContentSizeCentered(pes_internal.window, w, h));
    if (pes_window_read_info())
        pes_internal.pending_events |= EV_RESIZE;
}

bool pes_poll(f32* delta_time_out) {
    assert_pes_initialized();

    if (pes_internal.drawing)
        pes_draw_end();

    // reset
    pes.events = pes_internal.pending_events;
    pes_internal.pending_events = 0;
    pes.arena.used = 0;
    pes.app_active = pes_internal.app_active;
    pes.screen.fullscreen = pes_internal.fullscreen;
    pes.keyboard.pressed_count = 0;
    memset(pes.keyboard.pressed, 0, sizeof(pes.keyboard.pressed));
    pes.mouse.moved.x = 0.0f;
    pes.mouse.moved.y = 0.0f;
    pes.mouse.pressed = 0;
    for (u32 i = 0; i < GAMEPAD_COUNT; i++)
        pes.gamepad[i].pressed = 0;
    pes_internal.frame_ready = false;

    // check for changes
    if UNLIKELY (
        pes.mouse.cursor != pes_internal.mouse.cursor
        || pes.mouse.visible != pes_internal.mouse.visible)
    {
        pes_cursor_update();
    }

    // process events until FRAME_SYNC
    while (!pes_internal.frame_ready) {
        PBEvent* events = (PBEvent*)arena_alloc(PBSysEventSize_MAX);
        i32      events_count = PanicOnErr(PBEventPoll(events, PBSysEventSize_MAX, U32_MAX));
        if UNLIKELY (events_count <= 0)
            return false; // thread is exiting
        for (PBEvent* event = events; events_count--; event = PBEventNext(event)) {
            // log_event(event);
            switch (event->type) {
                case PBEventType_KEY_DOWN:
                case PBEventType_KEY_UP:   pes_on_keyboard_event((PBKeyboardEvent*)event); break;
                case PBEventType_SIGNAL:   pes_on_signal_event((PBSignalEvent*)event); break;
                case PBEventType_TIMER:    break;

                case PBEventType_GAMEPAD_CONNECTED:
                case PBEventType_GAMEPAD_DISCONNECTED:
                case PBEventType_GAMEPAD_BUTTON_DOWN:
                case PBEventType_GAMEPAD_BUTTON_UP:
                case PBEventType_GAMEPAD_AXIS_MOVE:
                    pes_on_gamepad_event((PBGamepadEvent*)event);
                    break;

                case PBEventType_POINTER_ENTER:
                case PBEventType_POINTER_LEAVE:
                case PBEventType_POINTER_DOWN:
                case PBEventType_POINTER_UP:
                case PBEventType_POINTER_MOVE:
                case PBEventType_POINTER_CANCEL:
                    pes_on_pointer_event((PBPointerEvent*)event);
                    break;

                case PBEventType_SCROLL:
                case PBEventType_GESTURE_PAN:
                case PBEventType_GESTURE_PINCH:
                case PBEventType_GESTURE_ROTATE:
                case PBEventType_FILE_PANEL_CLOSED:
                case PBEventType_INVALID:           break;
            }
        }
    }

    // check for state changes
    pes.events = flag_toggle(pes.events, EV_ACTIVE, pes.app_active != pes_internal.app_active);
    pes.app_active = pes_internal.app_active;

    pes.events = flag_toggle(
        pes.events, EV_FULLSCREEN, pes.screen.fullscreen != pes_internal.fullscreen);
    pes.screen.fullscreen = pes_internal.fullscreen;

    // allocate space for drawing in arena
    pes_draw_begin();

    // update time
    pes.time = PBTimeNow();
    *delta_time_out = (f32)(pes.time - pes_internal.update_time) * 1e-9f;
    pes_internal.update_time = pes.time;

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// keyboard key code translation

static u32 pes_Key_of_PBKeyboardKey(PBKeyboardKey key) {
    switch (key) {
        case PBKeyboardKey_Space:          return Key_Space;
        case PBKeyboardKey_Quote:          return Key_Quote;
        case PBKeyboardKey_Comma:          return Key_Comma;
        case PBKeyboardKey_Minus:          return Key_Minus;
        case PBKeyboardKey_Period:         return Key_Period;
        case PBKeyboardKey_Slash:          return Key_Slash;
        case PBKeyboardKey_0:              return Key_0;
        case PBKeyboardKey_1:              return Key_1;
        case PBKeyboardKey_2:              return Key_2;
        case PBKeyboardKey_3:              return Key_3;
        case PBKeyboardKey_4:              return Key_4;
        case PBKeyboardKey_5:              return Key_5;
        case PBKeyboardKey_6:              return Key_6;
        case PBKeyboardKey_7:              return Key_7;
        case PBKeyboardKey_8:              return Key_8;
        case PBKeyboardKey_9:              return Key_9;
        case PBKeyboardKey_Semicolon:      return Key_Semicolon;
        case PBKeyboardKey_Equal:          return Key_Equal;
        case PBKeyboardKey_A:              return Key_A;
        case PBKeyboardKey_B:              return Key_B;
        case PBKeyboardKey_C:              return Key_C;
        case PBKeyboardKey_D:              return Key_D;
        case PBKeyboardKey_E:              return Key_E;
        case PBKeyboardKey_F:              return Key_F;
        case PBKeyboardKey_G:              return Key_G;
        case PBKeyboardKey_H:              return Key_H;
        case PBKeyboardKey_I:              return Key_I;
        case PBKeyboardKey_J:              return Key_J;
        case PBKeyboardKey_K:              return Key_K;
        case PBKeyboardKey_L:              return Key_L;
        case PBKeyboardKey_M:              return Key_M;
        case PBKeyboardKey_N:              return Key_N;
        case PBKeyboardKey_O:              return Key_O;
        case PBKeyboardKey_P:              return Key_P;
        case PBKeyboardKey_Q:              return Key_Q;
        case PBKeyboardKey_R:              return Key_R;
        case PBKeyboardKey_S:              return Key_S;
        case PBKeyboardKey_T:              return Key_T;
        case PBKeyboardKey_U:              return Key_U;
        case PBKeyboardKey_V:              return Key_V;
        case PBKeyboardKey_W:              return Key_W;
        case PBKeyboardKey_X:              return Key_X;
        case PBKeyboardKey_Y:              return Key_Y;
        case PBKeyboardKey_Z:              return Key_Z;
        case PBKeyboardKey_LeftBracket:    return Key_LeftBracket;
        case PBKeyboardKey_Backslash:      return Key_Backslash;
        case PBKeyboardKey_RightBracket:   return Key_RightBracket;
        case PBKeyboardKey_Grave:          return Key_Grave;
        case PBKeyboardKey_Escape:         return Key_Escape;
        case PBKeyboardKey_Enter:          return Key_Enter;
        case PBKeyboardKey_Tab:            return Key_Tab;
        case PBKeyboardKey_Backspace:      return Key_Backspace;
        case PBKeyboardKey_Insert:         return Key_Insert;
        case PBKeyboardKey_Delete:         return Key_Delete;
        case PBKeyboardKey_Left:           return Key_Left;
        case PBKeyboardKey_Right:          return Key_Right;
        case PBKeyboardKey_Down:           return Key_Down;
        case PBKeyboardKey_Up:             return Key_Up;
        case PBKeyboardKey_PageUp:         return Key_PageUp;
        case PBKeyboardKey_PageDown:       return Key_PageDown;
        case PBKeyboardKey_Home:           return Key_Home;
        case PBKeyboardKey_End:            return Key_End;
        case PBKeyboardKey_CapsLock:       return Key_CapsLock;
        case PBKeyboardKey_LeftShift:      return Key_LeftShift;
        case PBKeyboardKey_LeftCtrl:       return Key_LeftCtrl;
        case PBKeyboardKey_LeftAlt:        return Key_LeftAlt;
        case PBKeyboardKey_LeftSuper:      return Key_LeftSuper;
        case PBKeyboardKey_MediaNext:      return Key_MediaNext;
        case PBKeyboardKey_MediaPrev:      return Key_MediaPrev;
        case PBKeyboardKey_MediaPlay:      return Key_MediaPlay;
        case PBKeyboardKey_MediaStop:      return Key_MediaStop;
        case PBKeyboardKey_World1:         return Key_World1;
        case PBKeyboardKey_World2:         return Key_World2;
        case PBKeyboardKey_ScrollLock:     return Key_ScrollLock;
        case PBKeyboardKey_NumLock:        return Key_NumLock;
        case PBKeyboardKey_PrintScreen:    return Key_PrintScreen;
        case PBKeyboardKey_Pause:          return Key_Pause;
        case PBKeyboardKey_F1:             return Key_F1;
        case PBKeyboardKey_F2:             return Key_F2;
        case PBKeyboardKey_F3:             return Key_F3;
        case PBKeyboardKey_F4:             return Key_F4;
        case PBKeyboardKey_F5:             return Key_F5;
        case PBKeyboardKey_F6:             return Key_F6;
        case PBKeyboardKey_F7:             return Key_F7;
        case PBKeyboardKey_F8:             return Key_F8;
        case PBKeyboardKey_F9:             return Key_F9;
        case PBKeyboardKey_F10:            return Key_F10;
        case PBKeyboardKey_F11:            return Key_F11;
        case PBKeyboardKey_F12:            return Key_F12;
        case PBKeyboardKey_F13:            return Key_F13;
        case PBKeyboardKey_F14:            return Key_F14;
        case PBKeyboardKey_F15:            return Key_F15;
        case PBKeyboardKey_F16:            return Key_F16;
        case PBKeyboardKey_F17:            return Key_F17;
        case PBKeyboardKey_F18:            return Key_F18;
        case PBKeyboardKey_F19:            return Key_F19;
        case PBKeyboardKey_F20:            return Key_F20;
        case PBKeyboardKey_F21:            return Key_F21;
        case PBKeyboardKey_F22:            return Key_F22;
        case PBKeyboardKey_F23:            return Key_F23;
        case PBKeyboardKey_F24:            return Key_F24;
        case PBKeyboardKey_Numpad0:        return Key_Numpad0;
        case PBKeyboardKey_Numpad1:        return Key_Numpad1;
        case PBKeyboardKey_Numpad2:        return Key_Numpad2;
        case PBKeyboardKey_Numpad3:        return Key_Numpad3;
        case PBKeyboardKey_Numpad4:        return Key_Numpad4;
        case PBKeyboardKey_Numpad5:        return Key_Numpad5;
        case PBKeyboardKey_Numpad6:        return Key_Numpad6;
        case PBKeyboardKey_Numpad7:        return Key_Numpad7;
        case PBKeyboardKey_Numpad8:        return Key_Numpad8;
        case PBKeyboardKey_Numpad9:        return Key_Numpad9;
        case PBKeyboardKey_NumpadDot:      return Key_NumpadDot;
        case PBKeyboardKey_NumpadDivide:   return Key_NumpadDivide;
        case PBKeyboardKey_NumpadMultiply: return Key_NumpadMultiply;
        case PBKeyboardKey_NumpadSubtract: return Key_NumpadSubtract;
        case PBKeyboardKey_NumpadAdd:      return Key_NumpadAdd;
        case PBKeyboardKey_NumpadEnter:    return Key_NumpadEnter;
        case PBKeyboardKey_NumpadEquals:   return Key_NumpadEquals;
        case PBKeyboardKey_NumpadClear:    return Key_NumpadClear;
        case PBKeyboardKey_RightShift:     return Key_RightShift;
        case PBKeyboardKey_RightCtrl:      return Key_RightCtrl;
        case PBKeyboardKey_RightAlt:       return Key_RightAlt;
        case PBKeyboardKey_RightSuper:     return Key_RightSuper;
        case PBKeyboardKey_Menu:           return Key_Menu;
        case PBKeyboardKey_VolumeUp:       return Key_VolumeUp;
        case PBKeyboardKey_VolumeDown:     return Key_VolumeDown;
        case PBKeyboardKey_Mute:           return Key_Mute;
        case PBKeyboardKey_None:
        case PBKeyboardKey_COUNT:          break;
    }
    return U32_MAX;
}
