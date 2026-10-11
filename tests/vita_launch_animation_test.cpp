#include <cassert>
#include <cmath>
#include "../frontend/vita/launch_animation.h"

int main()
{
    using namespace vita_launch_animation;
    const Pose start = sample(0.0f);
    assert(start.drop_y == 0.0f);
    assert(start.roll_degrees == 0.0f);
    assert(start.face_blend == 0.0f);
    assert(!start.iris_active && !start.finished);

    assert(sample(0.41f).roll_degrees == 360.0f);
    assert(sample(0.42f).drop_y == 0.0f);
    assert(sample(0.75f).drop_y == 345.0f);
    assert(sample(1.70f).drop_y == 384.0f);
    assert(sample(0.15f).face_blend == 1.0f);
    assert(!sample(kIrisStart - 0.01f).iris_active);
    assert(sample(kIrisStart).iris_radius == kIrisInitialRadius);
    assert(sample(kIrisEnd).iris_radius == 0.0f);
    assert(sample(kFinish).finished);
    assert(!sample(kFinish - 0.01f).finished);

    float prev_drop = 0.0f;
    float prev_roll = 0.0f;
    float prev_radius = kIrisInitialRadius;
    for (int i = 0; i <= 210; ++i) {
        const Pose p = sample(i * 0.01f);
        assert(p.drop_y >= prev_drop && p.drop_y <= 384.0f);
        assert(p.roll_degrees >= prev_roll && p.roll_degrees <= 360.0f);
        assert(p.face_blend >= 0.0f && p.face_blend <= 1.0f);
        assert(p.iris_radius <= prev_radius && p.iris_radius >= 0.0f);
        prev_drop = p.drop_y;
        prev_roll = p.roll_degrees;
        prev_radius = p.iris_radius;
    }
    return 0;
}
