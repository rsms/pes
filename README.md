# Playbit Entertainment System

This is a miniature game engine

See `pes.h` for API and run examples in this directory with `pb run <filename>`, e.g. `pb run bouncing-ball.c`

## Example

`pb run bounce.c`

```c
// bounce.c
#include "pes.h"
#include "pes.c"

void main(void) {
    pes_init("Bounce", 0, 0, rgb(40, 43, 48));
    Vec2 pos = { 100, 100 }, vel = { 2, 4 };
    f32  radius = 40;
    f32 dt;
    while (pes_poll(&dt)) {
        pos = vec2_add(pos, vec2_scale(vel, 90.0f * dt));
        if (pos.x - radius <= 0.0f) {
            vel.x = -vel.x;
            pos.x = radius;
        } else if (pos.x + radius >= pes.screen.width) {
            vel.x = -vel.x;
            pos.x = pes.screen.width - radius;
        }
        if (pos.y - radius <= 0.0f) {
            vel.y = -vel.y;
            pos.y = radius;
        } else if (pos.y + radius >= pes.screen.height) {
            vel.y = -vel.y;
            pos.y = pes.screen.height - radius;
        }
        shape_fill(draw_circle(pos, radius), rgb(255, 200, 120));
    }
}
```

## Testing

1. Build with `pb build --debug -j1 -Xc,-DPES_DEBUG=1 -o o/example.wasm example.c`
2. Run with [automation](https://playbit.app/docs/tools/automation)

```
{
    printf '%s\n' '{"command":"wait","ms":250}'
    printf '%s\n' '{"command":"key_down","key":"Right","deviceKey":"Right"}'
    printf '%s\n' '{"command":"wait","ms":250}'
    printf '%s\n' '{"command":"key_up","key":"Right","deviceKey":"Right"}'
    printf '%s\n' '{"command":"wait","ms":2500}'
    printf '%s\n' '{"command":"screenshot","id":"a","format":"png"}'
} | /Applications/Playbit.app/Contents/MacOS/Playbit \
    --remote-control o/example.wasm
```
