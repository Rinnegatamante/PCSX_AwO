#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../frontend/vita/livearea_launch.h"

static void expect_path(const char *param, const char *expected)
{
    char path[1024];
    assert(VitaLiveArea_ParseGamePath(param, path, sizeof(path)) == 1);
    assert(strcmp(path, expected) == 0);
}

static void expect_invalid(const char *param)
{
    char path[1024] = "old path";
    assert(VitaLiveArea_ParseGamePath(param, path, sizeof(path)) == 0);
    assert(path[0] == '\0');
}

int main(void)
{
    const char *raw = "type=LAUNCH_APP_BY_URI&uri=psgm:play?titleid=PCSXR0001&param=";
    char long_uri[1200];
    char short_path[8];

    expect_path("type=LAUNCH_APP_BY_URI&uri=psgm:play?titleid=PCSXR0001&param=ux0:data/pcsx-awo/roms/CTR - Crash Team Racing.cue",
                "ux0:data/pcsx-awo/roms/CTR - Crash Team Racing.cue");
    expect_path("psgm:play?titleid=PCSXR0001&param=ux0%3A%2Fdata%2FMetal%20Gear%20Solid%20%26%20VR%20Missions.chd",
                "ux0:/data/Metal Gear Solid & VR Missions.chd");
    expect_path("psgm:play?titleid=PCSXR0001&param=uma0%3aroms%2fSpyro+the+Dragon%2bExtra.cue&unused=yes",
                "uma0:roms/Spyro the Dragon+Extra.cue");
    expect_invalid(NULL);
    expect_invalid("");
    expect_invalid("psgm:play?titleid=PCSXR0001");
    expect_invalid("psgm:play?titleid=PCSXR0001&param=");
    expect_invalid("psgm:play?titleid=PCSXR0001&param=ux0%3");
    expect_invalid("psgm:play?titleid=PCSXR0001&param=ux0%G0");
    expect_invalid("psgm:play?titleid=PCSXR0001&param=ux0%00file.cue");
    expect_invalid("psgm:play?titleid=PCSXR0001&param=ux0%0Afile.cue");
    expect_invalid("type=LAUNCH_APP_BY_URI&uri=other:play?titleid=PCSXR0001&param=game.cue");
    assert(VitaLiveArea_ParseGamePath("psgm:play?titleid=PCSXR0001&param=foo.cue",
                short_path, sizeof(short_path)) == 1);
    assert(VitaLiveArea_ParseGamePath("psgm:play?titleid=PCSXR0001&param=foo-long.cue",
                short_path, sizeof(short_path)) == 0);
    assert(short_path[0] == '\0');
    strcpy(long_uri, raw);
    memset(long_uri + strlen(raw), 'X', 1100);
    long_uri[strlen(raw) + 1100] = '\0';
    expect_invalid(long_uri);
    puts("LiveArea launch parsing OK");
    return 0;
}
