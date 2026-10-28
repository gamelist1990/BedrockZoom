#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/logger.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include "zoom/config.hpp"
#include "zoom/controller.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {
enum class Status : unsigned { NotStarted, Loading, Running, Stopped, Failed };

std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};
std::atomic<std::shared_ptr<const zoom::Config>> liveConfig{
    std::make_shared<const zoom::Config>()};
std::atomic<bool> configSavePending{false};
zoom::Controller controller;
bedrock::GameApi gameApi;
std::unique_ptr<bedrock::GameHooks> gameHooks;
std::mutex diagnosticMutex;
std::ofstream diagnosticLog;
std::atomic<bool> loggedFov{false};

template <class... Args>
void diagnostic(Args&&... args) {
    std::lock_guard lock(diagnosticMutex);
    if (!diagnosticLog) return;
    (diagnosticLog << ... << std::forward<Args>(args));
    diagnosticLog << '\n';
    diagnosticLog.flush();
}

void onMouseEvent(const bedrock::MouseEvent& event) {
    // MouseDevice::feed uses action 4 for the wheel and carries the native
    // wheel delta in state (+120/-120). Consume only an active Zoom wheel so
    // Minecraft never advances the hotbar for an adjustment.
    constexpr std::int8_t mouseWheel = 4;
    auto config = liveConfig.load(std::memory_order_acquire);
    if (!event.synthetic && event.action == mouseWheel && event.state != 0 && config->scrollAdjustment &&
        controller.isZoomActive(*config)) {
        for (;;) {
            auto updated = std::make_shared<zoom::Config>(*config);
            const float direction = event.state > 0 ? 1.0f : -1.0f;
            updated->zoomMultiplier = std::clamp(
                updated->zoomMultiplier + direction * updated->scrollStep, 1.0f, 50.0f);
            std::shared_ptr<const zoom::Config> desired = std::move(updated);
            if (liveConfig.compare_exchange_weak(config, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                configSavePending.store(true, std::memory_order_release);
                event.cancel();
                return;
            }
            if (!config->scrollAdjustment || !controller.isZoomActive(*config)) break;
        }
    }

}

void onFovEvent(const bedrock::FovEvent& event) {
    if (!loggedFov.exchange(true, std::memory_order_acq_rel))
        diagnostic("LevelRendererPlayer::getFov active");

    static thread_local auto previous = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - previous).count();
    previous = now;
    const auto config = liveConfig.load(std::memory_order_acquire);
    event.setResultFov(zoom::zoomFovDegrees(event.inputFov,
                                           controller.tick(*config, delta)));
}

std::filesystem::path configPath() {
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents)))
        return {};
    std::filesystem::path path{documents};
    CoTaskMemFree(documents);
    return path / L"PEXData" / L"Zoom" / L"config.json";
}

bool writeConfig(const std::filesystem::path& path, const zoom::Config& config,
                 std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not create config.json";
        return false;
    }
    output << zoom::configJson(config);
    return static_cast<bool>(output);
}

bool writeDefaultConfig(const std::filesystem::path& path, std::string& error) {
    return writeConfig(path, zoom::Config{}, error);
}

bool loadConfig(const std::filesystem::path& path, bedrock::Logger& logger) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream text;
    text << input.rdbuf();
    std::string error;
    auto parsed = zoom::parseConfig(text.str(), error);
    if (!error.empty()) {
        logger.error("Ignoring invalid config.json: ", error);
        return false;
    }
    const auto old = liveConfig.load(std::memory_order_acquire);
    if (old->key != parsed.key || old->mode != parsed.mode) controller.resetInput();
    liveConfig.store(std::make_shared<const zoom::Config>(parsed), std::memory_order_release);
    return true;
}

DWORD WINAPI startClient(void*) {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&startClient), &pinned)) {
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    status.store(Status::Loading, std::memory_order_release);
    bedrock::Logger logger{"Zoom"};
    const auto path = configPath();
    std::string error;
    if (path.empty() || (!std::filesystem::exists(path) && !writeDefaultConfig(path, error))) {
        logger.error("Config initialization failed: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnosticLog.open(path.parent_path() / L"Zoom.log",
                       std::ios::binary | std::ios::trunc);
    diagnostic("start");
    if (!loadConfig(path, logger)) {
        diagnostic("config load failed");
        logger.error("Initial config.json is invalid or unreadable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnostic("config ready");

    bedrock::ProfileChoice version;
    std::string profile;
    diagnostic("profile inspect game");
    if (!bedrock::fetchSupportedProfile(profile, version, error,
            [](std::string_view stage) { diagnostic("profile ", stage); })) {
        logger.error("Unsupported Minecraft build or profile download failed: ", error);
        diagnostic("Version profile unavailable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnostic("profile selected ", version.version, " ",
               bedrock::profileSourceName(version.source));
    bedrock::Resolver resolver;
    diagnostic("resolver load");
    if (!resolver.loadJson(profile, error)) {
        diagnostic("resolver load failed: ", error);
        logger.error("Invalid Minecraft symbol profile: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnostic("resolver attach");
    if (!resolver.attachWindowsMainModule(version.identity, error)) {
        diagnostic("resolver attach failed: ", error);
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    if (!resolver.exactBuild()) {
        diagnostic("resolver build mismatch");
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnostic("resolver ready");
    gameHooks = std::make_unique<bedrock::GameHooks>(gameApi);
    gameApi.Event.Enum.Keyboard.then([](const bedrock::KeyEvent& event) {
        const auto config = liveConfig.load(std::memory_order_acquire);
        controller.onMinecraftKey(static_cast<int>(event.inputKey()), event.pressed, *config);
    });
    gameApi.Event.Enum.Mouse.then(&onMouseEvent);
    gameApi.Event.Enum.Fov.then(&onFovEvent);
    gameApi.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& event) {
        controller.setGameplayActive(event.active);
    });
    const auto installHooks = [&] {
        return gameHooks->installInput(resolver, error) &&
            gameHooks->installFov(resolver, error) &&
            gameHooks->installGameplay(resolver, error);
    };
    bool hooksReady = installHooks();
    // An exact-build cache can precede new hook definitions. Retire partial
    // subscriptions before refreshing and retrying with the matching profile.
    if (!hooksReady && version.source != bedrock::ProfileSource::Remote) {
        diagnostic("cached profile lacks usable hooks; checking GitHub");
        gameHooks.reset();
        std::string refreshedProfile;
        bedrock::ProfileChoice refreshedVersion;
        if (bedrock::refreshSupportedProfile(version.identity, refreshedProfile, refreshedVersion, error,
                [](std::string_view stage) { diagnostic("profile ", stage); }) &&
            resolver.loadJson(refreshedProfile, error) &&
            resolver.attachWindowsMainModule(refreshedVersion.identity, error) &&
            resolver.exactBuild()) {
            profile = std::move(refreshedProfile);
            version = std::move(refreshedVersion);
            gameHooks = std::make_unique<bedrock::GameHooks>(gameApi);
            hooksReady = installHooks();
        }
    }
    if (!hooksReady) {
        diagnostic("SDK GameHooks install failed: ", error);
        gameHooks.reset();
        logger.error("Could not subscribe Zoom to shared hook runtime");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnostic("shared hooks registered");
    diagnostic("ready ", version.version, " ", bedrock::profileSourceName(version.source));

    status.store(Status::Running, std::memory_order_release);
    logger.info("Minecraft native InputKey Zoom ready: ", path.string());
    auto lastWrite = std::filesystem::last_write_time(path);
    while (!stopRequested.load(std::memory_order_acquire)) {
        const auto delay = liveConfig.load(std::memory_order_acquire)->reloadIntervalMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        if (configSavePending.exchange(false, std::memory_order_acq_rel)) {
            const auto currentConfig = liveConfig.load(std::memory_order_acquire);
            std::string saveError;
            if (writeConfig(path, *currentConfig, saveError)) {
                std::error_code writeTimeError;
                const auto savedWrite = std::filesystem::last_write_time(path, writeTimeError);
                if (!writeTimeError) lastWrite = savedWrite;
            } else {
                logger.error("Could not save Zoom multiplier: ", saveError);
                configSavePending.store(true, std::memory_order_release);
            }
        }
        std::error_code ec;
        const auto current = std::filesystem::last_write_time(path, ec);
        if (!ec && current != lastWrite) {
            if (loadConfig(path, logger)) lastWrite = current;
        }
    }

    controller.setGameplayActive(false);
    gameHooks.reset();
    status.store(Status::Stopped, std::memory_order_release);
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) unsigned Zoom_GetStatus() {
    return static_cast<unsigned>(status.load(std::memory_order_acquire));
}

extern "C" __declspec(dllexport) void Zoom_Stop() {
    stopRequested.store(true, std::memory_order_release);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, &startClient, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else status.store(Status::Failed, std::memory_order_release);
    } else if (reason == DLL_PROCESS_DETACH) {
        stopRequested.store(true, std::memory_order_release);
    }
    return TRUE;
}
#endif
