#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <MinHook.h>
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

using KeyboardFeedFn = bool(__fastcall*)(int, bool);
using MouseFeedFn = void(__fastcall*)(void*, std::int8_t, std::int8_t, std::int16_t,
                                     std::int16_t, std::int16_t, std::int16_t, bool);
using GetFovFn = float(__fastcall*)(void*, float, void*, void*);
using CursorFn = void(__fastcall*)(void*);

std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};
std::atomic<KeyboardFeedFn> originalKeyboardFeed{nullptr};
std::atomic<MouseFeedFn> originalMouseFeed{nullptr};
std::atomic<GetFovFn> originalGetFov{nullptr};
std::atomic<CursorFn> originalGrabCursor{nullptr};
std::atomic<CursorFn> originalReleaseCursor{nullptr};
std::atomic<bool> cursorStateObserved{false};
std::atomic<std::shared_ptr<const zoom::Config>> liveConfig{
    std::make_shared<const zoom::Config>()};
std::atomic<bool> configSavePending{false};
zoom::Controller controller;
std::array<void*, 5> hookTargets{};
bool ownsMinHook = false;
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

bool __fastcall onKeyboardFeed(int inputKey, bool down) {
    // This is Minecraft's own KeyboardManager dispatch path. InputKey and its
    // transition arrive as direct arguments; no Windows key-state API is used.
    const auto config = liveConfig.load(std::memory_order_acquire);
    controller.onMinecraftKey(inputKey, down, *config);
    if (auto original = originalKeyboardFeed.load(std::memory_order_acquire))
        return original(inputKey, down);
    return false;
}

void __fastcall onMouseFeed(void* self, std::int8_t action, std::int8_t state,
                            std::int16_t x, std::int16_t y, std::int16_t dx,
                            std::int16_t dy, bool forceMotionlessPointer) {
    // MouseDevice::feed uses action 4 for the wheel and carries the native
    // wheel delta in state (+120/-120). Consume only an active Zoom wheel so
    // Minecraft never advances the hotbar for an adjustment.
    constexpr std::int8_t mouseWheel = 4;
    auto config = liveConfig.load(std::memory_order_acquire);
    if (action == mouseWheel && state != 0 && config->scrollAdjustment &&
        controller.isZoomActive(*config)) {
        for (;;) {
            auto updated = std::make_shared<zoom::Config>(*config);
            const float direction = state > 0 ? 1.0f : -1.0f;
            updated->zoomMultiplier = std::clamp(
                updated->zoomMultiplier + direction * updated->scrollStep, 1.0f, 50.0f);
            std::shared_ptr<const zoom::Config> desired = std::move(updated);
            if (liveConfig.compare_exchange_weak(config, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                configSavePending.store(true, std::memory_order_release);
                return;
            }
            if (!config->scrollAdjustment || !controller.isZoomActive(*config)) break;
        }
    }

    if (auto original = originalMouseFeed.load(std::memory_order_acquire))
        original(self, action, state, x, y, dx, dy, forceMotionlessPointer);
}

float __fastcall onGetFov(void* self, float inputFov, void* a3, void* a4) {
    auto original = originalGetFov.load(std::memory_order_acquire);
    const float fov = original ? original(self, inputFov, a3, a4) : inputFov;
    if (!loggedFov.exchange(true, std::memory_order_acq_rel))
        diagnostic("LevelRendererPlayer::getFov active");
    if (!cursorStateObserved.load(std::memory_order_acquire))
        controller.setGameplayActive(true);

    static thread_local auto previous = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - previous).count();
    previous = now;
    const auto config = liveConfig.load(std::memory_order_acquire);
    return zoom::zoomFovDegrees(fov, controller.tick(*config, delta));
}

void __fastcall onGrabCursor(void* self) {
    if (auto original = originalGrabCursor.load(std::memory_order_acquire)) original(self);
    cursorStateObserved.store(true, std::memory_order_release);
    controller.setGameplayActive(true);
}

void __fastcall onReleaseCursor(void* self) {
    cursorStateObserved.store(true, std::memory_order_release);
    controller.setGameplayActive(false);
    if (auto original = originalReleaseCursor.load(std::memory_order_acquire)) original(self);
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

bool exactRva(const std::optional<bedrock::Resolution>& symbol) {
    return symbol && symbol->exact_build && symbol->method == "exact-rva";
}

template <class Original>
bool createHook(void* target, void* detour, std::atomic<Original>& original) {
    Original captured = nullptr;
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&captured)) != MH_OK)
        return false;
    original.store(captured, std::memory_order_release);
    return true;
}

void removeHooks() noexcept {
    for (void* target : hookTargets) if (target) MH_DisableHook(target);
    for (void* target : hookTargets) if (target) MH_RemoveHook(target);
    hookTargets.fill(nullptr);
    if (ownsMinHook) MH_Uninitialize();
    ownsMinHook = false;
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
        logger.error("Initial config.json is invalid or unreadable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    bedrock::ProfileChoice version;
    std::string profile;
    if (!bedrock::fetchSupportedProfile(profile, version, error)) {
        logger.error("Unsupported Minecraft build or profile download failed: ", error);
        diagnostic("Version profile unavailable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    bedrock::Resolver resolver;
    if (!resolver.loadJson(profile, error) ||
        !resolver.attachWindowsMainModule(error) || !resolver.exactBuild()) {
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    const auto keyboard = resolver.resolve("Keyboard::feed");
    const auto mouse = resolver.resolve("MouseDevice::feed");
    const auto fov = resolver.resolve("LevelRendererPlayer::getFov");
    const auto grab = resolver.resolve("ClientInstance::grabCursor", true);
    const auto release = resolver.resolve("ClientInstance::releaseCursor", true);
    if (!exactRva(keyboard) || !keyboard->verified ||
        !exactRva(mouse) || !mouse->verified ||
        !exactRva(fov) || !fov->verified || !exactRva(grab) || !exactRva(release)) {
        logger.error("Required Minecraft native input/FOV symbols are unavailable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    hookTargets = {reinterpret_cast<void*>(keyboard->address),
                   reinterpret_cast<void*>(mouse->address),
                   reinterpret_cast<void*>(fov->address),
                   reinterpret_cast<void*>(grab->address),
                   reinterpret_cast<void*>(release->address)};
    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        logger.error("MinHook initialization failed");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    ownsMinHook = initialized == MH_OK;
    const bool created =
        createHook(hookTargets[0], reinterpret_cast<void*>(&onKeyboardFeed), originalKeyboardFeed) &&
        createHook(hookTargets[1], reinterpret_cast<void*>(&onMouseFeed), originalMouseFeed) &&
        createHook(hookTargets[2], reinterpret_cast<void*>(&onGetFov), originalGetFov) &&
        createHook(hookTargets[3], reinterpret_cast<void*>(&onGrabCursor), originalGrabCursor) &&
        createHook(hookTargets[4], reinterpret_cast<void*>(&onReleaseCursor), originalReleaseCursor);
    if (!created) {
        removeHooks();
        logger.error("Could not create Zoom hooks");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    bool queued = true;
    for (void* target : hookTargets) queued = queued && MH_QueueEnableHook(target) == MH_OK;
    if (!queued || MH_ApplyQueued() != MH_OK) {
        removeHooks();
        logger.error("Could not enable Zoom hooks");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
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
    removeHooks();
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
