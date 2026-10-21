#pragma once

#include "zoom/config.hpp"

#include <atomic>

namespace zoom {

class Controller {
public:
    void onMinecraftKey(int key, bool down, const Config& config) noexcept;
    void setGameplayActive(bool active) noexcept;
    void resetInput() noexcept;
    float tick(const Config& config, float deltaSeconds) noexcept;
    float currentMultiplier() const noexcept { return currentMultiplier_; }

private:
    std::atomic<bool> gameplayActive_{false};
    std::atomic<bool> keyHeld_{false};
    std::atomic<bool> toggled_{false};
    float currentMultiplier_ = 1.0f;
};

float zoomFovDegrees(float fovDegrees, float multiplier) noexcept;

} // namespace zoom
