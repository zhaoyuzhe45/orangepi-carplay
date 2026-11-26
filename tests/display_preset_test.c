#include "carplay_capabilities.h"
#include <stdio.h>
#include <string.h>

static int failed;
#define CHECK(c, s) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", s); failed++; } } while (0)
static struct carplay_display_plan plan(unsigned w, unsigned h, int pw, int ph)
{
    struct carplay_session_capabilities caps;
    struct carplay_display_plan p;
    memset(&caps, 0, sizeof(caps));
    memset(&p, 0, sizeof(p));
    caps.display.present = 1;
    caps.display.width_pixels = (int)w;
    caps.display.height_pixels = (int)h;
    caps.display.max_fps = 30;
    CHECK(carplay_capabilities_display_plan(&caps, 800, 480, 0, 0, pw, ph, 0, 60, &p) == 0,
          "receiver display plan accepted");
    return p;
}
int main(void)
{
    struct carplay_display_plan p = plan(800, 480, 0, 0);
    CHECK(p.width == 800 && p.height == 480 && p.physical_width == 800 && p.physical_height == 480 && p.density == 160,
          "800x480 stays native at density160");
    p = plan(1366, 768, 0, 0);
    CHECK(p.width == 1366 && p.height == 768 && p.physical_width == 1280 && p.physical_height == 720,
          "unknown canvas uses closest physical preset but preserves receiver output");
    CHECK(p.density == 240 && p.fps == 30, "density belongs to physical preset; FPS obeys receiver");
    CHECK(p.output_width == 1366 && p.output_height == 768,
          "receiver within encoder limits retains original video dimensions");
    p = plan(950, 432, 0, 0);
    CHECK(p.physical_width == 1024 && p.physical_height == 480 &&
          p.width == 950 && p.height == 432 &&
          p.output_width == 950 && p.output_height == 432,
          "DiPlay small canvas receives exact encoded dimensions from closest preset");
    p = plan(800, 376, 0, 0);
    CHECK(p.width == 800 && p.height == 376 && p.physical_width == 800 && p.physical_height == 480,
          "short canvas stretches from800x480");
    p = plan(1920, 720, 0, 0);
    CHECK(p.width == 1920 && p.height == 720 && p.physical_width == 1920 && p.physical_height == 900,
          "wide canvas retains requested dimensions");
    p = plan(1280, 600, 800, 480);
    CHECK(p.width == 1280 && p.height == 600 && p.physical_width == 800 && p.physical_height == 480,
          "manual physical mode cannot replace receiver video size");
    p = plan(1920, 1080, 0, 0);
    CHECK(p.density == 280 && p.physical_width == 1920 && p.physical_height == 1080,
          "1080p preset keeps known density");
    p = plan(1365, 767, 0, 0);
    CHECK(p.width == 1364 && p.height == 766 && p.physical_width == 1280 && p.physical_height == 720,
          "odd receiver canvas rounds down by at most one pixel for H264 chroma");
    struct carplay_session_capabilities oversized = {0};
    oversized.display.present = 1;
    const unsigned unsupported[][2] = {{3168,1440},{1440,3168},{1922,1080},{1920,1090},{7680,4320}};
    for (unsigned i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); i++) {
        oversized.display.width_pixels = (int)unsupported[i][0];
        oversized.display.height_pixels = (int)unsupported[i][1];
        CHECK(carplay_capabilities_display_plan(&oversized,800,480,0,0,0,0,0,30,&p)<0,
              "unencodable canvas rejected before mode switching or codec allocation");
    }
    p = plan(1920, 1088, 0, 0);
    CHECK(p.output_width == 1920 && p.output_height == 1088,
          "encoder upper boundary remains exact, without implicit receiver scaling");
    printf("display presets: %s\n", failed ? "FAIL" : "PASS");
    return failed != 0;
}
