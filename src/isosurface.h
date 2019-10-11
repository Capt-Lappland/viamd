#pragma once

#include <core/types.h>
#include <core/array_types.h>
#include <core/vector_types.h>

#include <vector>

struct IsoSurface {
    const size_t maxCount = 6; // Keep in sync with shader!

    bool enabled = false;
    std::vector<std::pair<float, vec4>> values;

    void add(float v, const vec4& color);
    void clear();
    void sort();

    std::pair<std::vector<float>, std::vector<vec4>> getData() const;
};
