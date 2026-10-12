#include "postfx_parameters.h"

const VitaPostFXParameter vita_postfx_parameters[] = {
    {"daltonic", "Protanopia", "Protanopia", "vita_fx_protanopia", 2.0f, 0.0f, 2.0f, 0.10f},
    {"daltonic", "Deuteranopia", "Deuteranopia", "vita_fx_deuteranopia", 1.0f, 0.0f, 2.0f, 0.10f},
    {"daltonic", "Tritanopia", "Tritanopia", "vita_fx_tritanopia", 1.0f, 0.0f, 2.0f, 0.10f},
    {"gamma", "Gamma", "Gamma", "vita_fx_gamma", 1.0f, 0.10f, 2.0f, 0.05f},
    {"hue", "Hue", "Hue", "vita_fx_hue", 1.0f, 0.0f, 2.0f, 0.05f},
    {"hue", "Saturation", "Saturation", "vita_fx_saturation", 1.0f, 0.0f, 2.0f, 0.05f},
    {"vignette", "Radius", "Radius", "vita_fx_radius", 1.0f, 0.0f, 2.0f, 0.05f},
    {"vignette", "Softness", "Softness", "vita_fx_softness", 1.0f, 0.05f, 2.0f, 0.05f},
    {"vignette", "Opacity", "Opacity", "vita_fx_opacity", 1.0f, 0.0f, 2.0f, 0.05f}
};

const size_t vita_postfx_parameter_count =
    sizeof(vita_postfx_parameters) / sizeof(vita_postfx_parameters[0]);
