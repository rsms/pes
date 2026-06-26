# Playbit Entertainment System

This is a miniature game engine

See `pes.h` for API and run examples in this directory with `pb run <filename>`, e.g. `pb run bouncing-ball.c`

Some code uses SIMD when available. For WASM this must be enabled manually by passing `-Xc,-msimd128` to `pb`. WASM SIMD is not enabled by default since not all WASM runtimes support the SIMD extension.

> Don't have `pb` in PATH? Add `export PATH=/Applications/Playbit.app/Contents/SharedSupport/bin:$PATH` to your shell's init file, like for example `~/.zshenv` or `~/.profile`

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
