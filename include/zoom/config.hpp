#pragma once

#include <string>
#include <string_view>

namespace zoom {

enum class ActivationMode { Hold, Toggle };

struct Config {
    bool enabled = true;
    int key = 'C';
    ActivationMode mode = ActivationMode::Hold;
    float zoomMultiplier = 4.0f;
    bool smooth = true;
    float animationSpeed = 12.0f;
    unsigned reloadIntervalMs = 250;
};

int parseKey(std::string_view name) noexcept;
Config parseConfig(std::string_view jsonText, std::string& error) noexcept;
std::string defaultConfigJson();

} // namespace zoom
