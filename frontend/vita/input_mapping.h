#ifndef VITA_INPUT_MAPPING_H
#define VITA_INPUT_MAPPING_H

#include <stdint.h>
#include <string.h>

enum {
    VITA_TOUCH_FRONT_TOP_LEFT = 1u << 0,
    VITA_TOUCH_FRONT_TOP_RIGHT = 1u << 1,
    VITA_TOUCH_FRONT_BOTTOM_LEFT = 1u << 2,
    VITA_TOUCH_FRONT_BOTTOM_RIGHT = 1u << 3,
    VITA_TOUCH_REAR_TOP_LEFT = 1u << 4,
    VITA_TOUCH_REAR_TOP_RIGHT = 1u << 5,
    VITA_TOUCH_REAR_BOTTOM_LEFT = 1u << 6,
    VITA_TOUCH_REAR_BOTTOM_RIGHT = 1u << 7
};

enum {
    VITA_MULTITAP_OFF,
    VITA_MULTITAP_PORT1,
    VITA_MULTITAP_PORT2,
    VITA_MULTITAP_BOTH
};

enum {
    VITA_SIM_DPAD_UP = 1u << 0,
    VITA_SIM_DPAD_DOWN = 1u << 1,
    VITA_SIM_DPAD_LEFT = 1u << 2,
    VITA_SIM_DPAD_RIGHT = 1u << 3
};

static inline unsigned vita_simulated_dpad(uint8_t lx, uint8_t ly, unsigned previous)
{
    // Separate press and release thresholds keep the virtual D-pad stable near the deadzone.
    const int press = 48;
    const int release = 32;
    const int x = (int)lx - 128;
    const int y = (int)ly - 128;
    unsigned result = 0;

    if (x <= -((previous & VITA_SIM_DPAD_LEFT) ? release : press))
        result |= VITA_SIM_DPAD_LEFT;
    else if (x >= ((previous & VITA_SIM_DPAD_RIGHT) ? release : press))
        result |= VITA_SIM_DPAD_RIGHT;

    if (y <= -((previous & VITA_SIM_DPAD_UP) ? release : press))
        result |= VITA_SIM_DPAD_UP;
    else if (y >= ((previous & VITA_SIM_DPAD_DOWN) ? release : press))
        result |= VITA_SIM_DPAD_DOWN;

    return result;
}

static inline int vita_psx_port_for_player(unsigned player, unsigned multitap)
{
    if (player >= 4 || (multitap == VITA_MULTITAP_OFF && player >= 2))
        return -1;
    if (multitap == VITA_MULTITAP_BOTH && player >= 2)
        return (int)player + 2;
    return (int)player;
}

static inline int vita_player_for_psx_port(unsigned port, unsigned multitap)
{
    for (unsigned player = 0; player < 4; ++player)
        if (vita_psx_port_for_player(player, multitap) == (int)port)
            return (int)player;
    return -1;
}

static inline int16_t vita_analog_axis(uint8_t value)
{
    const int delta = (int)value - 128;
    return (delta >= -12 && delta <= 12) ? 0 : (int16_t)(delta * 256);
}

static inline unsigned vita_touch_corner_at(int x, int y,
    int min_x, int max_x, int min_y, int max_y, int rear)
{
    if (min_x >= max_x || min_y >= max_y ||
        x < min_x || x > max_x || y < min_y || y > max_y)
        return 0;
    const unsigned right = x >= min_x + (max_x - min_x) / 2;
    const unsigned bottom = y >= min_y + (max_y - min_y) / 2;
    return 1u << ((rear ? 4 : 0) + bottom * 2 + right);
}

static inline unsigned vita_touch_binding(const char *setting)
{
    static const char *const names[] = {
        "front_top_left", "front_top_right", "front_bottom_left", "front_bottom_right",
        "rear_top_left", "rear_top_right", "rear_bottom_left", "rear_bottom_right"
    };
    if (setting)
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (strcmp(setting, names[i]) == 0)
                return 1u << i;
    return 0;
}

#endif
