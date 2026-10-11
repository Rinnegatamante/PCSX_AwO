#ifndef PCSX_VITA_LAUNCH_ANIMATION_H
#define PCSX_VITA_LAUNCH_ANIMATION_H

namespace vita_launch_animation {

static const float kIrisStart = 1.73f;
static const float kIrisEnd = 1.98f;
static const float kFinish = 2.06f;
static const float kIrisInitialRadius = 1250.0f;

inline float ease(float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

inline float phase(float time, float begin, float end)
{
    return ease((time - begin) / (end - begin));
}

struct Pose {
    float drop_y;
    float roll_degrees;
    float face_blend;
    float iris_radius;
    bool iris_active;
    bool finished;
};

inline Pose sample(float time)
{
    Pose pose;
    pose.face_blend = phase(time, 0.0f, 0.15f);
    pose.roll_degrees = 360.0f * phase(time, 0.04f, 0.41f);
    pose.drop_y = 345.0f * phase(time, 0.43f, 0.75f) +
                  39.0f * phase(time, 0.75f, 1.69f);
    const float iris_time = (time - kIrisStart) / (kIrisEnd - kIrisStart);
    const float iris_remaining = 1.0f -
        (iris_time <= 0.0f ? 0.0f : iris_time >= 1.0f ? 1.0f : iris_time);
    pose.iris_radius = kIrisInitialRadius * iris_remaining *
                       iris_remaining * iris_remaining;
    pose.iris_active = time >= kIrisStart;
    pose.finished = time >= kFinish;
    return pose;
}

} // namespace vita_launch_animation

#endif
