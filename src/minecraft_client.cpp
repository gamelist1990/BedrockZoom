#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <bedrock/hook_runtime.h>
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
std::array<BDS_HookToken, 5> hookTokens{};
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

void BDS_HOOK_CALL keyboardMiddleware(void*, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_KeyboardArgs*>(raw);
    const auto previous = keyboardFrame; keyboardFrame = {next, frame};
    args->result = onKeyboardFeed(args->key, args->down != 0) ? 1 : 0;
    keyboardFrame = previous;
}
void BDS_HOOK_CALL mouseMiddleware(void*, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_MouseArgs*>(raw);
    const auto previous = mouseFrame; mouseFrame = {next, frame};
    onMouseFeed(args->self, args->action, args->state, args->x, args->y,
                args->dx, args->dy, args->force_motionless_pointer != 0);
    mouseFrame = previous;
}
void BDS_HOOK_CALL fovMiddleware(void*, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_FovArgs*>(raw);
    const auto previous = fovFrame; fovFrame = {next, frame};
    args->result = onGetFov(args->self, args->input_fov, args->a3, args->a4);
    fovFrame = previous;
}
void BDS_HOOK_CALL cursorMiddleware(void* user, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_CursorArgs*>(raw);
    const bool grabbing = reinterpret_cast<std::uintptr_t>(user) == 1;
    auto& slot = grabbing ? grabFrame : releaseFrame;
    const auto previous = cursorFrameSlot; cursorFrameSlot = &slot;
    const auto old = slot; slot = {next, frame};
    if (grabbing) onGrabCursor(args->self); else onReleaseCursor(args->self);
    slot = old; cursorFrameSlot = previous;
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

struct NextFrame { BDS_HookNext next{}; void* frame{}; };
thread_local NextFrame keyboardFrame, mouseFrame, fovFrame, grabFrame, releaseFrame;
thread_local NextFrame* cursorFrameSlot{};
bool __fastcall callNextKeyboard(int key, bool down) {
    if (!keyboardFrame.next) return false;
    BDS_KeyboardArgs args{key, down ? 1 : 0, 0};
    keyboardFrame.next(keyboardFrame.frame, &args);
    return args.result != 0;
}
void __fastcall callNextMouse(void* self, std::int8_t action, std::int8_t state,
    std::int16_t x, std::int16_t y, std::int16_t dx, std::int16_t dy, bool force) {
    if (!mouseFrame.next) return;
    BDS_MouseArgs args{self, action, state, x, y, dx, dy, force ? 1 : 0};
    mouseFrame.next(mouseFrame.frame, &args);
}
float __fastcall callNextFov(void* self, float fov, void* a3, void* a4) {
    if (!fovFrame.next) return fov;
    BDS_FovArgs args{self, fov, a3, a4, fov};
    fovFrame.next(fovFrame.frame, &args);
    return args.result;
}
void __fastcall callNextCursor(void* self) {
    auto* slot = cursorFrameSlot;
    if (slot && slot->next) { BDS_CursorArgs args{self}; slot->next(slot->frame, &args); }
}

void removeHooks() noexcept {
    for (auto& token : hookTokens) {
        if (token) BDS_HookUnsubscribe(token);
        token = nullptr;
    }
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
    std::optional<bedrock::Resolution> keyboard, mouse, fov, grab, release;
    const auto resolveRequired = [&] {
        keyboard = bedrock::resolveSharedHook(resolver, "Keyboard::feed", BDS_HOOK_KEYBOARD_FEED);
        mouse = bedrock::resolveSharedHook(resolver, "MouseDevice::feed", BDS_HOOK_MOUSE_FEED);
        fov = bedrock::resolveSharedHook(resolver, "LevelRendererPlayer::getFov", BDS_HOOK_GET_FOV);
        grab = bedrock::resolveSharedHook(resolver, "ClientInstance::grabCursor", BDS_HOOK_GRAB_CURSOR);
        release = bedrock::resolveSharedHook(resolver, "ClientInstance::releaseCursor", BDS_HOOK_RELEASE_CURSOR);
    };
    const auto valid = [](const auto& value) {
        return value && value->exact_build && value->verified;
    };
    const auto requirementsMet = [&] {
        return valid(keyboard) && valid(mouse) && valid(fov) && valid(grab) && valid(release);
    };
    resolveRequired();
    if (!requirementsMet() && version.source != bedrock::ProfileSource::Remote) {
        diagnostic("required symbols missing in cached profile; refreshing exact-build GitHub definition");
        std::string refreshedProfile;
        bedrock::ProfileChoice refreshedVersion;
        if (bedrock::refreshSupportedProfile(version.identity, refreshedProfile, refreshedVersion, error,
                [](std::string_view stage) { diagnostic("profile ", stage); })) {
            if (resolver.loadJson(refreshedProfile, error) &&
                resolver.attachWindowsMainModule(refreshedVersion.identity, error) &&
                resolver.exactBuild()) {
                profile = std::move(refreshedProfile);
                version = std::move(refreshedVersion);
                resolveRequired();
                diagnostic("latest exact-build profile loaded [",
                           bedrock::profileSourceName(version.source), "]");
            } else {
                diagnostic("refreshed profile attach failed: ", error);
            }
        } else {
            diagnostic("exact-build profile refresh failed: ", error);
        }
    }
    if (!requirementsMet()) {
        diagnostic("required symbols unavailable keyboard=", bool(keyboard),
                   " mouse=", bool(mouse), " fov=", bool(fov),
                   " grab=", bool(grab), " release=", bool(release));
        logger.error("Required Minecraft native input/FOV symbols are unavailable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    originalKeyboardFeed.store(&callNextKeyboard);
    originalMouseFeed.store(&callNextMouse);
    originalGetFov.store(&callNextFov);
    originalGrabCursor.store(&callNextCursor);
    originalReleaseCursor.store(&callNextCursor);
    const bool created =
        BDS_HookSubscribe(BDS_HOOK_KEYBOARD_FEED, reinterpret_cast<void*>(keyboard->address), keyboardMiddleware, nullptr, &hookTokens[0]) &&
        BDS_HookSubscribe(BDS_HOOK_MOUSE_FEED, reinterpret_cast<void*>(mouse->address), mouseMiddleware, nullptr, &hookTokens[1]) &&
        BDS_HookSubscribe(BDS_HOOK_GET_FOV, reinterpret_cast<void*>(fov->address), fovMiddleware, nullptr, &hookTokens[2]) &&
        BDS_HookSubscribe(BDS_HOOK_GRAB_CURSOR, reinterpret_cast<void*>(grab->address), cursorMiddleware, reinterpret_cast<void*>(1), &hookTokens[3]) &&
        BDS_HookSubscribe(BDS_HOOK_RELEASE_CURSOR, reinterpret_cast<void*>(release->address), cursorMiddleware, nullptr, &hookTokens[4]);
    if (!created) {
        diagnostic("shared hook subscription failed");
        removeHooks();
        logger.error("Could not subscribe Zoom to the shared Minecraft hook runtime");
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
