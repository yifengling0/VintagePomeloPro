#include "common/displayed_fps.h"
#include <cassert>
#include <thread>

int main()
{
    char folder[] = "/tmp/displayed-fps-test-XXXXXX";
    assert(mkdtemp(folder));
    const std::string base = std::string(folder) + "/fps";
    double fps = -1;
    assert(!winehua::ReadDisplayedFpsSample(base.c_str(), 8, 10000000, fps));
    // Concurrent independent renderer processes/windows must keep their values.
    std::thread game([&] {
        for (uint64_t i = 1; i <= 100; ++i)
            assert(winehua::PublishDisplayedFpsSample(base.c_str(), 8, i, 70, 10000000));
    });
    std::thread steam([&] {
        for (uint64_t i = 1; i <= 100; ++i)
            assert(winehua::PublishDisplayedFpsSample(base.c_str(), 29, i, 15, 10000000));
    });
    game.join(); steam.join();
    assert(winehua::ReadDisplayedFpsSample(base.c_str(), 8, 11000000, fps) && fps == 70);
    assert(winehua::ReadDisplayedFpsSample(base.c_str(), 29, 11000000, fps) && fps == 15);
    assert(!winehua::ReadDisplayedFpsSample(base.c_str(), 8, 12500001, fps));
    assert(!winehua::ReadDisplayedFpsSample(base.c_str(), 8, 9000000, fps));
    auto malformed = [&](const char* payload) {
        FILE* file = fopen(winehua::DisplayedFpsPath(base.c_str(), 8).c_str(), "w");
        assert(file); fputs(payload, file); fclose(file);
        assert(!winehua::ReadDisplayedFpsSample(base.c_str(), 8, 11000000, fps));
    };
    malformed("1 15 29 10000000\n");
    malformed("1 nan 8 10000000\n");
    malformed("1 70 8\n"); // Old samples have no freshness timestamp.
    malformed("broken\n");
    assert(winehua::PublishDisplayedFpsSample(base.c_str(), 8, 101, 0, 11000000));
    assert(winehua::ReadDisplayedFpsSample(base.c_str(), 8, 11000000, fps) && fps == 0);
    unlink(winehua::DisplayedFpsPath(base.c_str(), 8).c_str());
    unlink(winehua::DisplayedFpsPath(base.c_str(), 29).c_str());
    rmdir(folder);
    puts("displayed FPS: concurrent windows, freshness, clock reset, wrong ID and malformed samples passed");
}
