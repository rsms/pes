#include "../pes.h"
#include "../pes.c"
#include "../lib/camera_2d.h"
#include "../lib/grid_2d.h"

void main(void) {
    pes_init("Scroll", 0, 0, rgb(200, 200, 200));
    static Grid   grid = { .size = 32, .line_alpha = 90 };
    static Camera camera = CAMERA_INIT;
    for (f32 dt; pes_poll(&dt);) {
        camera_update(&camera, 0.005f, 1.0f, 1.0f);
        grid_draw(&grid, camera.origin, camera.zoom);

        Shape sh;
        draw_rect(camera_tr_rect(camera, rect(256, 256, 64, 64)), rgb(255, 200, 30));
        draw_rect(camera_tr_rect(camera, rect(320, 320, 64, 64)), rgb(225, 90, 60));
        sh = draw_rect(camera_tr_rect(camera, rect(544, 288, 128, 64)), rgb(100, 90, 240));
        shape_corner_radius(sh, 9999);
        sh->opacity = 0.5;
    }
}
