#include "../pes.h"
#include "../pes.c"

typedef struct {
    Vec2  pos;
    Vec2  vel;
    Color color;
    f32   radius;
} Ball;

Ball balls[512] = { 0 };
int  ball_count = 0;

void remove_ball(void) {
    if (ball_count > 0)
        ball_count--;
}

void add_ball(void) {
    if (ball_count == (int)countof(balls))
        return;
    int       i = ball_count++;
    const f32 kMaxRadius = 150;
    const f32 kMaxVelocity = 90;
    balls[i].radius = math_random_range(2, kMaxRadius);
    balls[i].pos = vec2(
        math_random_range(balls[i].radius, pes.screen.width - balls[i].radius),
        math_random_range(balls[i].radius, pes.screen.height - balls[i].radius));

    // small balls move fast, big move slow
    f32 velocity = clamp(kMaxRadius - balls[i].radius, 1, kMaxVelocity);
    velocity = velocity * 0.1f + 1.0f;
    balls[i].vel = vec2(velocity * math_random_range(-1, 1), velocity * math_random_range(-1, 1));

    balls[i].color = (Color){ 255, 200, 120, 255 };
}

void main(void) {
    pes_init("Bouncing Balls", 0, 0, rgb(40, 43, 48));
    f32 dt;
    while (pes_poll(&dt)) {
        if (pes.events & EV_INPUT) {
            if (gamepad_button_pressed(0, GamepadButton_A) || key_pressed(Key_Space))
                add_ball();
            if (gamepad_button_pressed(0, GamepadButton_B))
                remove_ball();
        }

        for (int i = 0; i < ball_count; i++) {
            Ball* ball = &balls[i];
            ball->pos = vec2_add(ball->pos, vec2_scale(ball->vel, 90.0f * dt));
            bool change_color = false;
            if (ball->pos.x - ball->radius <= 0.0f) {
                ball->vel.x = -ball->vel.x;
                ball->pos.x = ball->radius;
                change_color = true;
            } else if (ball->pos.x + ball->radius >= pes.screen.width) {
                ball->vel.x = -ball->vel.x;
                ball->pos.x = pes.screen.width - ball->radius;
                change_color = true;
            }
            if (ball->pos.y - ball->radius <= 0.0f) {
                ball->vel.y = -ball->vel.y;
                ball->pos.y = ball->radius;
                change_color = true;
            } else if (ball->pos.y + ball->radius >= pes.screen.height) {
                ball->vel.y = -ball->vel.y;
                ball->pos.y = pes.screen.height - ball->radius;
                change_color = true;
            }
            if (change_color) {
                ball->color = hsl(
                    (u32)(rand() % 360000), 0.5f + (u32)(rand() % 100) * 0.0005, 0.5f);
            }
        }

        for (int i = 0; i < ball_count; i++) {
            Shape sh = draw_circle(balls[i].pos, balls[i].radius);
            shape_fill(sh, balls[i].color);
            shape_stroke(sh, rgb(40, 43, 48), 4.0f);
        }
    }
}
