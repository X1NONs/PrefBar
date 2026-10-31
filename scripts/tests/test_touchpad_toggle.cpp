/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "prefbar/touchpad_toggle.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>

using prefbar::TouchpadTripleClick;

static void click(TouchpadTripleClick& d, std::uint64_t t) {
    assert(!d.update(true, t));
    assert(!d.update(true, t + 1)); // held state must not count as another click
    assert(!d.update(false, t + 2));
}

int main() {
    TouchpadTripleClick d;
    click(d, 100);
    click(d, 400);
    assert(d.update(true, 800)); // third distinct press inside 1.5s
    assert(!d.update(false, 802));

    click(d, 2000);
    click(d, 3700); // outside the window: this begins a new sequence
    assert(!d.update(true, 3900)); // second press in the restarted sequence
    assert(!d.update(false, 3901));
    assert(d.update(true, 4100)); // third press toggles

    d.reset();
    click(d, 5000);
    click(d, 5200);
    assert(d.update(true, 5400));
    std::cout << "PrefBar touchpad triple-click test: PASS\n";
}
