#ifndef VITA_INPUT_MAPPING_H
#define VITA_INPUT_MAPPING_H

#include <stdint.h>

enum {
    VITA_REAR_L2 = 1,
    VITA_REAR_R2 = 2
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

static inline unsigned vita_rear_trigger_at(int x, int y,
    int min_x, int max_x, int min_y, int max_y)
{
    if (min_x >= max_x || min_y >= max_y ||
        x < min_x || x > max_x || y < min_y ||
        y >= min_y + (max_y - min_y) / 2)
        return 0;
    return x >= min_x + (max_x - min_x) / 2 ? VITA_REAR_R2 : VITA_REAR_L2;
}

#endif
