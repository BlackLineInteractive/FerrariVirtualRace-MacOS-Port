// First-run configuration: the Windows version runs FVR_Setup.exe to write Config.ini.
// We write an equivalent file for the Mac renderer instead.
#include "rt.h"
#include "win32ui.h"
#include <sys/stat.h>
#include <string>

static void mkdirs(const std::string &p) {
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
}

void ensure_default_config() {
    std::string dir = g_user_dir + "/docs/Synetic/FerrariVR";
    std::string path = dir + "/Config.ini";
    struct stat sb;
    if (stat(path.c_str(), &sb) == 0) return;
    mkdirs(dir);
    ui_ensure_sdl();
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    int dw = dm ? dm->w : 1920, dh = dm ? dm->h : 1080;
    // start windowed at a size that fits the screen
    int w = 1280, h = 720;
    if (dw >= 1920 && dh >= 1200) { w = 1600; h = 900; }
    FILE *f = fopen(path.c_str(), "w");
    if (!f) { rtlog("cannot write %s", path.c_str()); return; }
    fprintf(f,
            "[Resolution]\r\n"
            "DeviceID=0x10029442\r\n"        // must match the adapter identifier reported by d3d9.cpp
            "Xres=%d\r\nYres=%d\r\nWinXres=%d\r\n"
            "ColorMode=22\r\nTexRes=2\r\nEnvMode=1\r\nViewDist=2\r\nGrassDetail=2\r\nCarShadow3D=1\r\n"
            "FullScreen=0\r\nWideWindow=1\r\nAntiAlias=0\r\nMotionBlur=1\r\nDoCenterWindow=1\r\n"
            "ShadowMapSize=2048\r\nCubeMapSize=256\r\n"
            "[Sound]\r\nSoundEnable=1\r\nMusicEnable=1\r\nQuadroOut=0\r\nSampleRate=48000\r\n"
            "[Videos]\r\nPlayVideos=1\r\n",
            w, h, w);
    fclose(f);
    rtlog("wrote default %s (%dx%d windowed)", path.c_str(), w, h);
}
