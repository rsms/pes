#pragma once
#define Str
#include <playbit/playbit.h>
#undef Str

#if PB_DEBUG
    #define WHEN_DEBUG(stmt) stmt
#else
    #define WHEN_DEBUG(stmt)
#endif

typedef u32 Events;
#define EV_INPUT      (1llu << 0)
#define EV_KEYBOARD   (1llu << 1) // EV_INPUT will also be set
#define EV_POINTER    (1llu << 2) // EV_INPUT will also be set
#define EV_MOUSE      (1llu << 3) // EV_POINTER and EV_INPUT will also be set
#define EV_GAMEPAD    (1llu << 4) // EV_INPUT will also be set
#define EV_ACTIVE     (1llu << 5) // sys.active changed
#define EV_RESIZE     (1llu << 6) // size and/or scale of screen changed
#define EV_FULLSCREEN (1llu << 7) // sys.fullscreen changed

#define PI          3.14159265358979323846f
#define HOUR        3600000000000ull
#define MINUTE      60000000000ull
#define SECOND      1000000000ull
#define MILLISECOND 1000000ull
#define MICROSECOND 1000ull
#define NANOSECOND  1ull
#define WHITE       ((Color){ 255, 255, 255, 255 })
#define BLACK       ((Color){ 255, 0, 0, 0 })

#define GAMEPAD_COUNT 4u

typedef PBSysWindowRendererShapeItem* Shape;
typedef PBTexture                     Texture;
typedef u32                           Ent;

#define Array(ELEM_TYPE) PBArrayType(ELEM_TYPE)

typedef PBSysWindowCreateTextureFlags TextureFlags;
enum {
    Texture_STREAMING = PBSysWindowCreateTextureFlag_STREAMING, // optimize for frequent changes
    Texture_NO_FILTER = PBSysWindowCreateTextureFlag_NEAREST,   // disable resampling filter
};

typedef struct {
    f32 x, y;
} __attribute__((aligned(8))) Vec2;

typedef struct {
    f32 x, y, z;
} __attribute__((aligned(8))) Vec3;

typedef struct {
    f32 x, y, z, w;
} __attribute__((aligned(8))) Vec4;

typedef struct {
    f32 top, right, bottom, left;
} Edges;

typedef struct {
    f32 x, y, width, height;
} Rect;

typedef struct {
    u8 r, g, b, a;
} __attribute__((aligned(4))) Color;

typedef __attribute__((__ext_vector_type__(2))) f32 SimdF32x2; // f32[2]
typedef struct {
    SimdF32x2 x;
    SimdF32x2 y;
    SimdF32x2 o;
} Transform;

typedef char* Str; // AStrHeader at ptr-4

typedef enum KeyboardKey    KeyboardKey;
typedef enum GamepadButton  GamepadButton;
typedef enum GamepadAxis    GamepadAxis;
typedef enum GamepadHaptics GamepadHaptics;
typedef enum GamepadInput   GamepadInput;

typedef struct {
    u64* use_bm;            // bitmap of allocated ids
    u64  use_bm_storage[1]; // inline storage for first 64 IDs
    u32  use_bm_cap;        // multiple of 64
    u32  max_id;
} IdPool;

typedef struct {
    u32 deviceId;      // device identifier, stable across reconnects
    u16 held;          // buttons currently pressed (GamepadButton)
    u16 pressed;       // buttons pressed at least once since last pes_event call (GamepadButton)
    u16 have_button;   // GamepadButton bit is set if button is available
    u8  have_axis;     // GamepadAxis bit is set if axis is available
    u8  have_haptics;  // GamepadHaptics bit is set if haptics is available at locality bit
    f32 axis_value[8]; // current value (GamepadAxis)
} Gamepad;

struct PES {
    PBTime time;       // current time (monotonic clock, nanoseconds)
    Events events;     // what changed since last pes_poll call (bits of EV_)
    bool   app_active; // true when app has user focus, is in foreground (EV_ACTIVE)

    struct {
        u32 used;
        u32 _unused;
        union {
            u8  bytes[16 * 1024 * 1024];
            u64 _align;
        };
    } arena;

    struct {
        IdPool id_pool;
        WHEN_DEBUG(Array(PBStr) debug_names);
    } ent;

    struct {
        Color clear_color;
        f32   scale;      // 1 dp = scale px
        f32   width;      // in dp
        f32   height;     // in dp
        bool  fullscreen; // window is presented full screen (EV_FULLSCREEN)
    } screen;

    struct {
        u8  held_count;    // number of keys held
        u8  pressed_count; // number of keys pressed
        u64 held[3];       // currently depressed keys i.e. level triggered (KeyboardKey)
        u64 pressed[3];    // pressed since last update i.e. edge triggered (KeyboardKey)
    } keyboard;

    struct {
        u32 deviceId;      // device identifier, stable across reconnects
        u16 held;          // buttons currently pressed (GamepadButton)
        u16 pressed;       // buttons pressed at least once since last update (GamepadButton)
        u16 have_button;   // GamepadButton bit is set if button is available
        u8  have_axis;     // GamepadAxis bit is set if axis is available
        u8  have_haptics;  // GamepadHaptics bit is set if haptics is available at locality bit
        f32 axis_value[8]; // current value (GamepadAxis)
    } gamepad[GAMEPAD_COUNT];

    struct {
        Vec2 origin;
        Vec2 moved;   // delta moved since last update
        u16  held;    // currently depressed buttons (bit 1 = primary, bit 2 = secondary)
        u16  pressed; // pressed since last update
    } mouse;
};
extern struct PES pes; // system state

// In your main() function, call pes_init() before calling any other pes functions.
// Then call pes_poll() in a loop until it returns false.
// pes_poll() returns at vsync (when a frame has just been presented on the display, regardless
// of if you drew anything or not. Note that if you don't draw anything, PES will not re-render
// anything, so energy usage is minimal if you don't draw.
void pes_init(const char* title, f32 w, f32 h, Color bg);
bool pes_poll(f32* delta_time_out);

// void pes_update(u64 events, f32 dt); // called at least 30 times per second; dt in seconds
// void pes_draw(void);                 // called when it's time to draw the screen

static bool key_held(KeyboardKey);
static bool key_pressed(KeyboardKey);

static bool gamepad_available(u32 player);
Str         gamepad_name(u32 player);
static bool gamepad_button(u32 player, GamepadButton);         // true if currently held
static bool gamepad_button_pressed(u32 player, GamepadButton); // pressed since last update
static bool gamepad_button_available(u32 player, GamepadButton);
static Vec2 gamepad_left_stick(u32 player);
static Vec2 gamepad_right_stick(u32 player);
static Vec2 gamepad_dpad_stick(u32 player);
static f32  gamepad_left_trigger(u32 player);
static f32  gamepad_right_trigger(u32 player);
static f32  gamepad_axis_value(u32 player, GamepadAxis axis);
static bool gamepad_axis_available(u32 player, GamepadAxis);
static bool gamepad_haptics_available(u32 player, GamepadHaptics);

Shape        draw_shape(f32 x, f32 y, f32 w, f32 h);
Shape        draw_shape_uv(f32 x, f32 y, f32 w, f32 h, Edges uv);
Texture      draw_set_texture(Texture tex); // NoTexture to clear, returns prev texture
Shape        draw_texture(f32 x, f32 y, f32 w, f32 h, Texture tex); // with uv {0,0,1,1}
Shape        draw_circle(Vec2 center_pos, f32 radius);
static Shape draw_rect(f32 x, f32 y, f32 w, f32 h, Color fill_color);
void         draw_push(void);
void         draw_pop(void);
Transform    draw_get_transform(void);
Transform    draw_transform(Transform next); // replace transform, returns previous value
void         draw_translate(f32 x, f32 y);
void         draw_scale(f32 x, f32 y);
void         draw_rotate(f32 radians);

Shape shape_fill(Shape shape, Color color);
Shape shape_stroke(Shape shape, Color color, f32 thickness);
Shape shape_stroke_inner(Shape shape, Color color, f32 thickness);
Shape shape_stroke_outer(Shape shape, Color color, f32 thickness);
Shape shape_set_uv(Shape shape, Edges uv);
Shape shape_corner_radius(Shape shape, f32 radius);
Shape shape_corner_radius4(Shape shape, f32 tl, f32 tr, f32 br, f32 bl);

#define NoTexture ((Texture){ 0 })
Texture texture_load(const char* resource_name);
Texture texture_create(u32 width, u32 height, TextureFlags flags); // 8-bit RGBA (0xAABBGGRR in LE)
void    texture_write(Texture tex, u32 x_px, u32 y_px, u32 w_px, u32 h_px, const Color* pixels);
Edges   texture_uv_of_rect(f32 tex_width, f32 tex_height, Rect rect);

static Vec2      vec2(f32 x, f32 y);
static Vec3      vec3(f32 x, f32 y, f32 z);
static Vec4      vec4(f32 x, f32 y, f32 z, f32 w);
static Transform transform_identity(void);
static Transform transform_translate(Transform transform, f32 x, f32 y);
static Transform transform_scale(Transform transform, f32 x, f32 y);
static Transform transform_rotate(Transform transform, f32 radians);
static bool      vec2_is_zero(Vec2 v);
static Vec2      vec2_scale(Vec2 v, f32 exponent);
static Vec2      vec2_add(Vec2 a, Vec2 b);

static Edges edges_flip_x(Edges e);
static Edges edges_flip_y(Edges e);

void* arena_alloc(usize nbyte);
void* arena_allocz(usize nbyte);
void  arena_free(void* ptr, usize nbyte);
#define arena_alloc_obj(T)          ((T*)arena_allocz(sizeof(T)))
#define arena_alloc_array(T, count) ((T*)arena_allocz(sizeof(T) * (count)))

#define math_min(x, y) PBMin((x), (y)) // returns the smaller value of x and y
#define math_max(x, y) PBMax((x), (y)) // returns the larger value of x and y
#define math_abs(x)    PBAbs(x)        // x as positive number
#define math_round(x)  PBRound(x)      // nearest integral
#define math_floor(x)  PBFloor(x)      // rounds toward negative infinity
#define math_ceil(x)   PBCeil(x)       // rounds toward positive infinity
#define math_sin(x)    PBSin(x)        // sine approximation
#define math_cos(x)    PBCos(x)        // cosine approximation
#define math_sqrt(x)   PBSqrt(x)       // square root
#define math_sign(x)   PBSign(x)       // 1 or -1, indicating the sign of x
#define math_tan(x) _Generic((y), f32: math_atan2_f32)((y), (x)) // tangent of a number in radians
#define math_atan2(x) \
    _Generic((y),     \
        f32: math_atan2_f32)((y), (x)) // angle in the plane (in radians) between the positive
                                       // x-axis and the ray from (0, 0) to the point (x, y),
static f32 math_random(void);          // [0.0-1.0)
static f32 math_random1(void);         // [0.0-1.0] (1.0 inclusive)
static f32 math_random_range(f32 min, f32 max); // [min-max]
static u64 math_random_uint(void);              // [0-U64_MAX]
static i64 math_random_int(void);               // [I64_MIN-I64_MAX]
static f32 snap(f32 v, f32 step);               // nearest multiple
static f32 snap_floor(f32 v, f32 step);         // previous/lower multiple
static f32 snap_ceil(f32 v, f32 step);          // next/higher multiple
static f32 clamp_0_1(f32 v);                    // clamp to [0.0..1.0]
#define clamp(v, min, max) Max(Min((v), (max)), (min))

static Color rgb(u8 r, u8 g, u8 b);
static Color rgba(u8 r, u8 g, u8 b, u8 a);
static Color grey(u8 rgb);              // == rgb(rgb, rgb, rgb);
Color        hsv(f32 h, f32 s, f32 v);  // 0-360, 0-1, 0-1
Color        hsl(f32 h, f32 s, f32 l);  // 0-360, 0-1, 0-1
u64          color_argb16(Color rgba8); // 0xAABBGGRR -> 0xAAAARRRRGGGGBBBB

static f32 px_of_dp(f32 dp_value);
static f32 dp_of_px(f32 px_value);

Ent         ent_create(const char* debug_name);
void        ent_del(Ent ent);
StrSlice    ent_debug_name_slice(Ent ent);
const char* ent_debug_name(Ent ent);
Ent         ent_assert_valid(Ent ent); // passthrough in release builds
// T* array_assign_ent(Ent ent, Array(T*) array)
#define array_assign_ent(Ent_ent, Array_array) \
    array_assign(ent_assert_valid(Ent_ent) - 1u, (Array_array))

u32  id_pool_alloc(IdPool* pool); // 0 if full
void id_pool_free(IdPool* pool, u32 id);
bool id_pool_is_valid(const IdPool* pool, u32 id);

static bool bit_get(const void* bits, usize bit);
static void bit_set(void* bits, usize bit);
static void bit_clear(void* bits, usize bit);
static void bit_toggle(void* bits, usize bit, bool on);
static u64  flag_toggle(u64 flags, u64 flag, bool on);

// bool array_resize(Array(T)* array, usize min_cap)
// T* array_assign(usize index, Array(T*) array)
#define array_resize(array, min_cap) PBArrayResize((array), kMemGPA, (min_cap), PBMem_ZERO)

#define assert  PBDebugExpect
#define assertf PBDebugExpectf

////////////////////////////////////////////////////////////////////////////////////////////////////
// impl

typedef struct {
    u32  len;
    char chars[];
} StrHeader;

#define array_assign(usize_index, Array_array)                                   \
    ({                                                                           \
        usize                       PB_TMPID(index) = (usize_index);             \
        __typeof__(*(Array_array))* PB_TMPID(array) = (Array_array);             \
        if UNLIKELY (PB_TMPID(array)->cap <= PB_TMPID(index)) {                  \
            UNUSED bool ok = array_resize(PB_TMPID(array), PB_TMPID(index) + 1); \
            assertf(ok, "cannot allocate memory (" #Array_array ")");            \
        }                                                                        \
        &PB_TMPID(array)->v[PB_TMPID(index)];                                    \
    })

#if PB_DEBUG
    #define ent_assert_valid(Ent_ent) _ent_assert_valid((Ent_ent), __func__, __FILE__, __LINE__)
inline static Ent _ent_assert_valid(Ent ent, const char* func, const char* file, int line) {
    if UNLIKELY (!id_pool_is_valid(&pes.ent.id_pool, ent))
        _PBPanic(__FILE__, __LINE__, __FUNCTION__, "invalid ent#%u", ent);
    return ent;
}
#else
    #define ent_assert_valid(Ent_ent) (Ent_ent)
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// enums

enum GamepadButton {
    GamepadButton_A = PBSysGamepadButton_A,
    GamepadButton_B = PBSysGamepadButton_B,
    GamepadButton_X = PBSysGamepadButton_X,
    GamepadButton_Y = PBSysGamepadButton_Y,
    GamepadButton_LEFT_SHOULDER = PBSysGamepadButton_LEFT_SHOULDER,
    GamepadButton_RIGHT_SHOULDER = PBSysGamepadButton_RIGHT_SHOULDER,
    GamepadButton_LEFT_TRIGGER = PBSysGamepadButton_LEFT_TRIGGER,
    GamepadButton_RIGHT_TRIGGER = PBSysGamepadButton_RIGHT_TRIGGER,
    GamepadButton_LEFT_THUMBSTICK = PBSysGamepadButton_LEFT_THUMBSTICK,
    GamepadButton_RIGHT_THUMBSTICK = PBSysGamepadButton_RIGHT_THUMBSTICK,
    GamepadButton_MENU = PBSysGamepadButton_MENU,
    GamepadButton_OPTIONS = PBSysGamepadButton_OPTIONS,
    GamepadButton_HOME = PBSysGamepadButton_HOME,
};

enum GamepadAxis {
    GamepadAxis_LEFT_X = PBSysGamepadAxis_LEFT_X,
    GamepadAxis_LEFT_Y = PBSysGamepadAxis_LEFT_Y,
    GamepadAxis_RIGHT_X = PBSysGamepadAxis_RIGHT_X,
    GamepadAxis_RIGHT_Y = PBSysGamepadAxis_RIGHT_Y,
    GamepadAxis_DPAD_X = PBSysGamepadAxis_DPAD_X,
    GamepadAxis_DPAD_Y = PBSysGamepadAxis_DPAD_Y,
    GamepadAxis_LEFT_TRIGGER = PBSysGamepadAxis_LEFT_TRIGGER,
    GamepadAxis_RIGHT_TRIGGER = PBSysGamepadAxis_RIGHT_TRIGGER,
};

enum GamepadHaptics {
    GamepadHaptics_DEFAULT,       // PBSysHidHaptics_DEFAULT       1u << 0
    GamepadHaptics_ALL,           // PBSysHidHaptics_ALL           1u << 1
    GamepadHaptics_HANDLES,       // PBSysHidHaptics_HANDLES       1u << 2
    GamepadHaptics_LEFT_HANDLE,   // PBSysHidHaptics_LEFT_HANDLE   1u << 3
    GamepadHaptics_RIGHT_HANDLE,  // PBSysHidHaptics_RIGHT_HANDLE  1u << 4
    GamepadHaptics_TRIGGERS,      // PBSysHidHaptics_TRIGGERS      1u << 5
    GamepadHaptics_LEFT_TRIGGER,  // PBSysHidHaptics_LEFT_TRIGGER  1u << 6
    GamepadHaptics_RIGHT_TRIGGER, // PBSysHidHaptics_RIGHT_TRIGGER 1u << 7
};

enum GamepadInput {
    GamepadInput_LEFT,
    GamepadInput_RIGHT,
    GamepadInput_DPAD,
    GamepadInput_LEFT_TRIGGER,
    GamepadInput_RIGHT_TRIGGER,
};

enum KeyboardKey {
    // start of pressed[0] bits

    Key_0,
    Key_1,
    Key_2,
    Key_3,
    Key_4,
    Key_5,
    Key_6,
    Key_7,
    Key_8,
    Key_9,
    Key_A,
    Key_B,
    Key_C,
    Key_D,
    Key_E,
    Key_F,
    Key_G,
    Key_H,
    Key_I,
    Key_J,
    Key_K,
    Key_L,
    Key_M,
    Key_N,
    Key_O,
    Key_P,
    Key_Q,
    Key_R,
    Key_S,
    Key_T,
    Key_U,
    Key_V,
    Key_W,
    Key_X,
    Key_Y,
    Key_Z,
    Key_Space,
    Key_Left,
    Key_Right,
    Key_Down,
    Key_Up,
    Key_Escape,    // ESC
    Key_Enter,     // RETURN SYMBOL
    Key_Tab,       // TAB
    Key_Backspace, // ERASE TO THE LEFT
    Key_Insert,    // INSERT
    Key_Delete,    // ERASE TO THE RIGHT
    Key_Quote,
    Key_Comma,
    Key_Minus,
    Key_Period,
    Key_Slash,
    Key_Semicolon,
    Key_Equal,
    Key_LeftBracket,
    Key_Backslash,
    Key_RightBracket,
    Key_Grave,
    Key_PageUp,
    Key_PageDown,
    Key_Home,
    Key_End,

    // start of pressed[1] bits

    Key_Numpad0 = 64,
    Key_Numpad1,
    Key_Numpad2,
    Key_Numpad3,
    Key_Numpad4,
    Key_Numpad5,
    Key_Numpad6,
    Key_Numpad7,
    Key_Numpad8,
    Key_Numpad9,
    Key_NumpadDot,
    Key_NumpadDivide,
    Key_NumpadMultiply,
    Key_NumpadSubtract,
    Key_NumpadAdd,
    Key_NumpadEnter,
    Key_NumpadEquals,
    Key_NumpadClear,

    Key_CapsLock,
    Key_LeftShift,
    Key_LeftCtrl,
    Key_LeftAlt,
    Key_LeftSuper,

    Key_RightShift,
    Key_RightCtrl,
    Key_RightAlt,
    Key_RightSuper,

    Key_ScrollLock,
    Key_NumLock,
    Key_PrintScreen,
    Key_Pause,

    Key_MediaNext,
    Key_MediaPrev,
    Key_MediaPlay,
    Key_MediaStop,
    Key_VolumeUp,
    Key_VolumeDown,
    Key_Mute,

    Key_World1,
    Key_World2,

    Key_Menu,

    // start of pressed[2] bits

    Key_F1 = 128,
    Key_F2,
    Key_F3,
    Key_F4,
    Key_F5,
    Key_F6,
    Key_F7,
    Key_F8,
    Key_F9,
    Key_F10,
    Key_F11,
    Key_F12,
    Key_F13,
    Key_F14,
    Key_F15,
    Key_F16,
    Key_F17,
    Key_F18,
    Key_F19,
    Key_F20,
    Key_F21,
    Key_F22,
    Key_F23,
    Key_F24,
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// implementation

inline static bool bit_get(const void* bits, usize bit) {
    return !!(((u8*)bits)[bit / 8] & ((u8)1 << (bit % 8)));
}

inline static void bit_set(void* bits, usize bit) {
    ((u8*)bits)[bit / 8] |= (u8)1 << (bit % 8);
}

inline static void bit_clear(void* bits, usize bit) {
    ((u8*)bits)[bit / 8] &= ~((u8)1 << (bit % 8));
}

inline static void bit_toggle(void* bits, usize bit, bool on) {
    u8* p = (u8*)bits + (bit >> 3);
    *p ^= ((u8)(0u - (u8)on) ^ *p) & (u8)(1u << (bit & 7));
}

inline static u64 flag_toggle(u64 flags, u64 flag, bool on) {
    return flags ^ ((0llu - (u64)on ^ flags) & flag);
}

inline static bool key_held(enum KeyboardKey key) {
    return bit_get(pes.keyboard.held, key);
}

inline static bool key_pressed(enum KeyboardKey key) {
    return bit_get(pes.keyboard.pressed, key);
}

inline static bool gamepad_available(u32 player) {
    assert(player < GAMEPAD_COUNT);
    // true if any bit is set in have_button or have_axis or have_haptics
    return *(u32*)&pes.gamepad[player].have_button;
}

inline static bool gamepad_button(u32 player, GamepadButton button) {
    assert(player < GAMEPAD_COUNT);
    return pes.gamepad[player].held & ((u16)1 << button);
}

inline static bool gamepad_button_pressed(u32 player, GamepadButton button) {
    assert(player < GAMEPAD_COUNT);
    return pes.gamepad[player].pressed & ((u16)1 << button);
}

inline static f32 gamepad_axis_value(u32 player, GamepadAxis axis) {
    assert(player < GAMEPAD_COUNT);
    assert(axis < countof(pes.gamepad[player].axis_value));
    return pes.gamepad[player].axis_value[axis];
}

inline static f32 gamepad_left_trigger(u32 player) {
    assert(player < GAMEPAD_COUNT);
    return pes.gamepad[player].axis_value[GamepadAxis_LEFT_TRIGGER];
}

inline static f32 gamepad_right_trigger(u32 player) {
    assert(player < GAMEPAD_COUNT);
    return pes.gamepad[player].axis_value[GamepadAxis_RIGHT_TRIGGER];
}

inline static Vec2 gamepad_left_stick(u32 player) {
    assert(player < GAMEPAD_COUNT);
    return (Vec2){ pes.gamepad[player].axis_value[GamepadAxis_LEFT_X],
                   pes.gamepad[player].axis_value[GamepadAxis_LEFT_Y] };
}

inline static Vec2 gamepad_right_stick(u32 player) {
    assert(player < GAMEPAD_COUNT);
    return (Vec2){ pes.gamepad[player].axis_value[GamepadAxis_RIGHT_X],
                   pes.gamepad[player].axis_value[GamepadAxis_RIGHT_Y] };
}

inline static Vec2 gamepad_dpad_stick(u32 player) {
    assert(player < GAMEPAD_COUNT);
    return (Vec2){ pes.gamepad[player].axis_value[GamepadAxis_DPAD_X],
                   pes.gamepad[player].axis_value[GamepadAxis_DPAD_Y] };
}

inline static bool gamepad_button_available(u32 player, GamepadButton button) {
    assert(player < GAMEPAD_COUNT);
    return pes.gamepad[player].have_button & ((u16)1 << (u16)button);
}

inline static bool gamepad_axis_available(u32 player, GamepadAxis axis) {
    assert(player < GAMEPAD_COUNT);
    return pes.gamepad[player].have_axis & ((u8)1 << (u8)axis);
}

inline static bool gamepad_haptics_available(u32 player, GamepadHaptics haptics) {
    assert(player < GAMEPAD_COUNT);
    return pes.gamepad[player].have_haptics & ((u8)1 << (u8)haptics);
}

////

inline static Vec2 vec2(f32 x, f32 y) {
    return (Vec2){ (x), (y) };
}
inline static Vec3 vec3(f32 x, f32 y, f32 z) {
    return (Vec3){ (x), (y), (z) };
}
inline static Vec4 vec4(f32 x, f32 y, f32 z, f32 w) {
    return (Vec4){ (x), (y), (z), (w) };
}

inline static bool vec2_is_zero(Vec2 v) {
    return *(u64*)&v == 0ull;
    // return v.x == 0.0f && v.y == 0.0f;
}

inline static Vec2 vec2_scale(Vec2 v, f32 s) {
    return (Vec2){ v.x * s, v.y * s };
}

inline static Vec2 vec2_add(Vec2 a, Vec2 b) {
    return (Vec2){ a.x + b.x, a.y + b.y };
}

////

inline static Edges edges_flip_x(Edges e) {
    return (Edges){ e.top, e.left, e.bottom, e.right };
}

inline static Edges edges_flip_y(Edges e) {
    return (Edges){ e.bottom, e.right, e.top, e.left };
}

////

f32 math_atan2_f32(f32 y, f32 x); // fast approximate; assumes finite x/y; error <0.01 rad

inline static f32 math_random(void) {
    // 24 random bits mapped to [0, 1)
    return (f32)(PBRand() >> 40) * 0x1.0p-24f;
}

inline static f32 math_random1(void) {
    // 24 random bits, mapped to [0, 1]
    return (f32)(PBRand() >> 40) / 16777215.0f;
}

inline static f32 math_random_range(f32 min, f32 max) {
    u32 x = (u32)(PBRand() >> 40);
    if (x == 0)
        return min;
    if (x == 0x00ffffffu)
        return max;
    return min + ((f32)x / 16777215.0f) * (max - min);
}

// inline static f32 math_random_range(f32 min, f32 max) {
//     // exclusive; [min-max)
//     return min + math_random() * (max - min);
// }

inline static i64 math_random_int(void) {
    return (i64)PBRand();
}

inline static u64 math_random_uint(void) {
    return PBRand();
}

inline static f32 snap(f32 v, f32 step) {
    return math_round(v / step) * step;
}

inline static f32 snap_floor(f32 v, f32 step) {
    return math_floor(v / step) * step;
}

inline static f32 snap_ceil(f32 v, f32 step) {
    return math_ceil(v / step) * step;
}

inline static f32 clamp_0_1(f32 x) {
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

////

inline static Color rgb(u8 r, u8 g, u8 b) {
    return (Color){ r, g, b, 255 };
}

inline static Color rgba(u8 r, u8 g, u8 b, u8 a) {
    return (Color){ r, g, b, a };
}

inline static Color grey(u8 v) {
    return (Color){ v, v, v, 255 };
}

inline static f32 px_of_dp(f32 dp_value) {
    return math_round(dp_value * pes.screen.scale);
}

inline static f32 dp_of_px(f32 px_value) {
    return px_value / pes.screen.scale;
}

////

inline static Shape draw_rect(f32 x, f32 y, f32 w, f32 h, Color fill_color) {
    return shape_fill(draw_shape(x, y, w, h), fill_color);
}

#define SIMD_CFUNC                                                                             \
    __attribute__((__always_inline__)) __attribute__((__const__)) __attribute__((__nodebug__)) \
    __attribute__((__overloadable__))

inline static SIMD_CFUNC SimdF32x2 simd_muladd(SimdF32x2 x, SimdF32x2 y, SimdF32x2 z) {
#pragma STDC FP_CONTRACT ON
    return x * y + z;
}

inline static Transform transform_identity(void) {
    return (Transform){
        .x = { 1.0f, 0.0f },
        .y = { 0.0f, 1.0f },
        .o = { 0.0f, 0.0f },
    };
}

inline static Transform transform_translate(Transform transform, f32 x, f32 y) {
    transform.o = simd_muladd(transform.x, (SimdF32x2){ x, x }, transform.o);
    transform.o = simd_muladd(transform.y, (SimdF32x2){ y, y }, transform.o);
    return transform;
}

inline static Transform transform_scale(Transform transform, f32 x, f32 y) {
    transform.x *= x;
    transform.y *= y;
    return transform;
}

inline static Transform transform_rotate(Transform transform, f32 radians) {
    f32       c = math_cos(radians);
    f32       s = math_sin(radians);
    SimdF32x2 x = transform.x;
    SimdF32x2 y = transform.y;
    transform.x = x * c + y * s;
    transform.y = y * c - x * s;
    return transform;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
