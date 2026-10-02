// Aspect-fit math for the host viewport. Compile: cl /std:c++17 /EHsc host_view_tests.cpp ..\system\host_view.cpp
#include <cstdio>
#include "host_view.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do{ ++g_checks; if(!(c)){ ++g_fail; std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);} }while(0)

int main() {
    // Exact fit: 640x400 into 640x400 -> full, no bars.
    HostViewRect r = host_fit_viewport(640, 400, 640, 400);
    CHECK(r.x==0 && r.y==0 && r.w==640 && r.h==400);

    // Wider window than 16:10 -> pillarbox (bars left/right), full height.
    r = host_fit_viewport(1000, 400, 640, 400);
    CHECK(r.h==400); CHECK(r.w==640); CHECK(r.x==180); CHECK(r.y==0);

    // Taller window -> letterbox (bars top/bottom), full width.
    r = host_fit_viewport(640, 800, 640, 400);
    CHECK(r.w==640); CHECK(r.h==400); CHECK(r.x==0); CHECK(r.y==200);

    // Integer 2x: 1280x800 -> 1280x800 exactly.
    r = host_fit_viewport(1280, 800, 640, 400);
    CHECK(r.w==1280 && r.h==800 && r.x==0 && r.y==0);

    // PET presentation is 4:3 (640x480 base): a 16:9 fullscreen desktop must
    // pillarbox to 1440x1080, NOT fill 1728+ wide like the old 16:10 base.
    r = host_fit_viewport(1920, 1080, 640, 480);
    CHECK(r.w==1440); CHECK(r.h==1080); CHECK(r.x==240); CHECK(r.y==0);

    // 4:3 window shows the 4:3 image edge to edge.
    r = host_fit_viewport(640, 480, 640, 480);
    CHECK(r.x==0 && r.y==0 && r.w==640 && r.h==480);

    // Degenerate inputs -> empty rect.
    r = host_fit_viewport(0, 400, 640, 400);
    CHECK(r.x==0 && r.y==0 && r.w==0 && r.h==0);

    std::printf("\nhost_view tests: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
