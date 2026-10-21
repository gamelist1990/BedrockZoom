#include "zoom/config.hpp"
#include "zoom/controller.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    std::string error;
    auto config = zoom::parseConfig(R"({
      "key":"Z", "mode":"hold", "zoom_multiplier":8,
      "smooth":false, "animation_speed":9, "reload_interval_ms":10
    })", error);
    assert(error.empty());
    assert(config.key == 'Z');
    assert(config.zoomMultiplier == 8.0f);
    assert(config.reloadIntervalMs == 50);

    zoom::Controller controller;
    controller.setGameplayActive(true);
    controller.onMinecraftKey('Z', true, config);
    assert(controller.tick(config, 0.016f) == 8.0f);
    controller.onMinecraftKey('Z', false, config);
    assert(controller.tick(config, 0.016f) == 1.0f);

    config.mode = zoom::ActivationMode::Toggle;
    controller.onMinecraftKey('Z', true, config);
    controller.onMinecraftKey('Z', true, config); // native repeat must not retoggle
    assert(controller.tick(config, 0.016f) == 8.0f);
    controller.onMinecraftKey('Z', false, config);
    controller.onMinecraftKey('Z', true, config);
    assert(controller.tick(config, 0.016f) == 1.0f);

    config.smooth = true;
    config.mode = zoom::ActivationMode::Hold;
    controller.onMinecraftKey('Z', false, config);
    controller.onMinecraftKey('Z', true, config);
    const float animated = controller.tick(config, 0.016f);
    assert(animated > 1.0f && animated < 8.0f);
    const float narrowed = zoom::zoomFovDegrees(70.0f, 4.0f);
    assert(narrowed > 0.0f && narrowed < 70.0f);

    assert(zoom::parseKey("C") == 'C');
    assert(zoom::parseKey("f5") == 0x74);
    assert(zoom::parseKey("bad") == 'C');
    std::cout << "zoom_test: PASS\n";
}
