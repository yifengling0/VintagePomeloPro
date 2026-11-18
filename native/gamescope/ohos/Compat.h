// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <algorithm>
#include <functional>
#include <iterator>

namespace gamescope {
// The OHOS libc++ snapshot lacks ranges algorithms. Preserve the projection
// semantics used to find ReShade annotations without extending namespace std.
template<typename Range, typename Predicate, typename Projection>
auto FindIf(Range&& range, Predicate predicate, Projection projection) {
    return std::find_if(std::begin(range), std::end(range), [&](const auto& value) {
        return std::invoke(predicate, std::invoke(projection, value));
    });
}
}
