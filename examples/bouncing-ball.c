#include "../pes.h"
#include "../pes.c"

Vec2  pos = { 100, 100 }, vel = { 2, 4 };
Color color = { 255, 200, 120, 255 };
f32   radius = 40;

void main(void) {
    pes_init("Bouncing Ball", 0, 0, rgb(40, 43, 48));
    f32 dt;
    while (pes_poll(&dt)) {
        // update state
        pos = vec2_add(pos, vec2_scale(vel, 90.0f * dt));
        bool change_color = false;
        if (pos.x - radius <= 0.0f) {
            vel.x = -vel.x;
            pos.x = radius;
            change_color = true;
        } else if (pos.x + radius >= pes.screen.width) {
            vel.x = -vel.x;
            pos.x = pes.screen.width - radius;
            change_color = true;
        }
        if (pos.y - radius <= 0.0f) {
            vel.y = -vel.y;
            pos.y = radius;
            change_color = true;
        } else if (pos.y + radius >= pes.screen.height) {
            vel.y = -vel.y;
            pos.y = pes.screen.height - radius;
            change_color = true;
        }
        if (change_color)
            color = hsl((u32)(rand() % 360000), 0.5f + (u32)(rand() % 100) * 0.0005, 0.5f);

        // draw
        Shape sh = draw_circle(pos, radius);
        shape_fill(sh, color);
    }
}
