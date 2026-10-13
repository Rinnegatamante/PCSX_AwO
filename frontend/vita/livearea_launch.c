#include "livearea_launch.h"

#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int VitaLiveArea_ParseGamePath(const char *app_param, char *out_path, size_t out_size)
{
    const char *uri;
    const char *param;
    size_t length = 0;

    if (!out_path || !out_size)
        return 0;
    out_path[0] = '\0';
    if (!app_param)
        return 0;

    uri = strstr(app_param, "psgm:play?");
    if (!uri)
        return 0;
    param = strstr(uri, "&param=");
    if (!param)
        return 0;
    param += sizeof("&param=") - 1;

    while (*param && *param != '&') {
        unsigned char c = (unsigned char)*param++;
        if (c == '%') {
            int high = hex_value(param[0]);
            int low = param[0] ? hex_value(param[1]) : -1;
            if (high < 0 || low < 0)
                goto invalid;
            c = (unsigned char)((high << 4) | low);
            param += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c < 32 || c == 127 || length + 1 >= out_size)
            goto invalid;
        out_path[length++] = (char)c;
    }

    out_path[length] = '\0';
    return length != 0;

invalid:
    out_path[0] = '\0';
    return 0;
}
