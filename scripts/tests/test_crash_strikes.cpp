#include "prefbar/crash_strikes.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main() {
    char dir[] = "/tmp/prefbar_strikes_XXXXXX";
    assert(mkdtemp(dir) != nullptr);

    char path[128];
    prefbar::renderer_crash_strike_path(0x04030000U, dir, path, sizeof path);
    assert(std::string(path) == std::string(dir) + "/shellui_crash_04030000");

    /* no file: zero strikes, never blocked */
    assert(prefbar::renderer_crash_strikes(path) == 0);
    assert(!prefbar::renderer_crash_blocked(path, false));

    /* one strike is not enough to block (could be an unrelated ShellUI restart) */
    assert(prefbar::renderer_crash_strike_add(path) == 1);
    assert(prefbar::renderer_crash_strikes(path) == 1);
    assert(!prefbar::renderer_crash_blocked(path, false));

    /* second strike blocks, force override unblocks */
    assert(prefbar::renderer_crash_strike_add(path) == 2);
    assert(prefbar::renderer_crash_blocked(path, false));
    assert(!prefbar::renderer_crash_blocked(path, true));

    /* strikes are per firmware */
    char other[128];
    prefbar::renderer_crash_strike_path(0x13400000U, dir, other, sizeof other);
    assert(prefbar::renderer_crash_strikes(other) == 0);
    assert(!prefbar::renderer_crash_blocked(other, false));

    /* a successful start clears the counter */
    prefbar::renderer_crash_strikes_clear(path);
    assert(prefbar::renderer_crash_strikes(path) == 0);
    assert(!prefbar::renderer_crash_blocked(path, false));

    /* garbage / huge file content is handled safely */
    {
        std::FILE* fp = std::fopen(path, "w");
        assert(fp);
        std::fputs("not a number\n", fp);
        std::fclose(fp);
        assert(prefbar::renderer_crash_strikes(path) == 0);
        fp = std::fopen(path, "w");
        std::fputs("4000000000\n", fp);
        std::fclose(fp);
        assert(prefbar::renderer_crash_strikes(path) == 1000);
    }

    /* empty / null path is a harmless no-op (unknown SDK) */
    assert(prefbar::renderer_crash_strikes(nullptr) == 0);
    assert(prefbar::renderer_crash_strike_add("") == 0);
    prefbar::renderer_crash_strikes_clear(nullptr);
    assert(!prefbar::renderer_crash_blocked("", false));

    prefbar::renderer_crash_strikes_clear(path);
    rmdir(dir);
    std::cout << "PrefBar crash-strike test: PASS\n";
}
