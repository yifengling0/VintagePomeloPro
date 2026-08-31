#include "graphics/gles_direct_policy.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace winehua;
    const GlesBufferOwner original{1, 2, 3, 4, 800, 600, 1};
    assert(original == original);
    auto changed = original;
    ++changed.surfaceKey; assert(!(original == changed)); changed = original;
    ++changed.generation; assert(!(original == changed)); changed = original;
    ++changed.display; assert(!(original == changed)); changed = original;
    ++changed.context; assert(!(original == changed)); changed = original;
    ++changed.width; assert(!(original == changed)); changed = original;
    ++changed.height; assert(!(original == changed)); changed = original;
    ++changed.format; assert(!(original == changed));
    assert(HasGlExtension("EGL_A EGL_B EGL_C", "EGL_A"));
    assert(HasGlExtension("EGL_A EGL_B EGL_C", "EGL_B"));
    assert(HasGlExtension("EGL_A EGL_B EGL_C", "EGL_C"));
    assert(!HasGlExtension("EGL_AB XEGL_A", "EGL_A"));
    assert(!HasGlExtension("", "EGL_A"));
    assert(!HasGlExtension("EGL_A", ""));
    assert(!HasGlExtension("EGL_A EGL_B", "EGL_A EGL_B"));
    std::cout << "gles_direct_policy PASS (owner isolation, extension tokens)\n";
}
