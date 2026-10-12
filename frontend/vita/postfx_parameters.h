#ifndef PCSX_VITA_POSTFX_PARAMETERS_H
#define PCSX_VITA_POSTFX_PARAMETERS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VitaPostFXParameter {
    const char *effect;
    const char *label;
    const char *uniform;
    const char *setting;
    float default_value;
    float minimum;
    float maximum;
    float step;
} VitaPostFXParameter;

extern const VitaPostFXParameter vita_postfx_parameters[];
extern const size_t vita_postfx_parameter_count;

#ifdef __cplusplus
}
#endif

#endif
