#include "zoom/config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace zoom {
namespace {
std::string upper(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return result;
}

template <class T>
T numberOr(const nlohmann::json& root, const char* key, T fallback) {
    const auto it = root.find(key);
    if (it == root.end() || !it->is_number()) return fallback;
    return it->get<T>();
}
} // namespace

int parseKey(std::string_view name) noexcept {
    const std::string key = upper(name);
    if (key.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(key.front());
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    if (key.size() >= 2 && key.front() == 'F') {
        try {
            const int number = std::stoi(key.substr(1));
            if (number >= 1 && number <= 12) return 0x70 + number - 1;
        } catch (...) {
        }
    }
    if (key == "SPACE") return 0x20;
    if (key == "TAB") return 0x09;
    if (key == "ENTER" || key == "RETURN") return 0x0D;
    if (key == "ESC" || key == "ESCAPE") return 0x1B;
    if (key == "SHIFT") return 0x10;
    if (key == "CTRL" || key == "CONTROL") return 0x11;
    if (key == "ALT") return 0x12;
    return 'C';
}

Config parseConfig(std::string_view jsonText, std::string& error) noexcept {
    Config config;
    error.clear();
    try {
        const auto root = nlohmann::json::parse(jsonText);
        if (!root.is_object()) throw std::runtime_error("config root must be an object");
        config.enabled = root.value("enabled", config.enabled);
        config.key = parseKey(root.value("key", std::string{"C"}));
        const auto mode = upper(root.value("mode", std::string{"hold"}));
        config.mode = mode == "TOGGLE" ? ActivationMode::Toggle : ActivationMode::Hold;
        config.zoomMultiplier = std::clamp(
            numberOr<float>(root, "zoom_multiplier", config.zoomMultiplier), 1.0f, 50.0f);
        config.smooth = root.value("smooth", config.smooth);
        config.animationSpeed = std::clamp(
            numberOr<float>(root, "animation_speed", config.animationSpeed), 0.1f, 60.0f);
        config.reloadIntervalMs = std::clamp(
            numberOr<unsigned>(root, "reload_interval_ms", config.reloadIntervalMs), 50u, 5000u);
        if (!std::isfinite(config.zoomMultiplier) || !std::isfinite(config.animationSpeed))
            throw std::runtime_error("zoom values must be finite");
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return config;
}

std::string defaultConfigJson() {
    return R"({
  "enabled": true,
  "key": "C",
  "mode": "hold",
  "zoom_multiplier": 4.0,
  "smooth": true,
  "animation_speed": 12.0,
  "reload_interval_ms": 250
}
)";
}

} // namespace zoom
