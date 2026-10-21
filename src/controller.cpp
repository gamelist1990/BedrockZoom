#include "zoom/controller.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace zoom {

void Controller::onMinecraftKey(int key, bool down, const Config& config) noexcept {
    if (key != config.key) return;
    const bool wasDown = keyHeld_.exchange(down, std::memory_order_acq_rel);
    if (down && !wasDown && config.mode == ActivationMode::Toggle &&
        gameplayActive_.load(std::memory_order_acquire)) {
        toggled_.store(!toggled_.load(std::memory_order_relaxed),
                       std::memory_order_release);
    }
}

void Controller::setGameplayActive(bool active) noexcept {
    gameplayActive_.store(active, std::memory_order_release);
    if (!active) resetInput();
}

void Controller::resetInput() noexcept {
    keyHeld_.store(false, std::memory_order_release);
    toggled_.store(false, std::memory_order_release);
}

float Controller::tick(const Config& config, float deltaSeconds) noexcept {
    const bool requested = config.mode == ActivationMode::Toggle
        ? toggled_.load(std::memory_order_acquire)
        : keyHeld_.load(std::memory_order_acquire);
    const bool active = config.enabled &&
        gameplayActive_.load(std::memory_order_acquire) && requested;
    const float target = active ? config.zoomMultiplier : 1.0f;

    if (!config.smooth) {
        currentMultiplier_ = target;
        return currentMultiplier_;
    }
    const float dt = std::clamp(deltaSeconds, 0.0f, 0.25f);
    const float alpha = 1.0f - std::exp(-config.animationSpeed * dt);
    currentMultiplier_ += (target - currentMultiplier_) * alpha;
    if (std::abs(target - currentMultiplier_) < 0.0005f) currentMultiplier_ = target;
    return currentMultiplier_;
}

float zoomFovDegrees(float fovDegrees, float multiplier) noexcept {
    if (!std::isfinite(fovDegrees) || !std::isfinite(multiplier) || multiplier <= 1.0f)
        return fovDegrees;
    constexpr float toRadians = std::numbers::pi_v<float> / 180.0f;
    constexpr float toDegrees = 180.0f / std::numbers::pi_v<float>;
    const float half = std::clamp(fovDegrees, 1.0f, 179.0f) * toRadians * 0.5f;
    return 2.0f * std::atan(std::tan(half) / multiplier) * toDegrees;
}

} // namespace zoom
