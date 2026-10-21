#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <MinHook.h>
#include <bedrock/generated_symbols.hpp>
#include <bedrock/logger.hpp>
#include <bedrock/resolver.hpp>
#include "zoom/config.hpp"
#include "zoom/controller.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace {
enum class Status : unsigned { NotStarted, Loading, Running, Stopped, Failed };

using KeyboardInputFn = void*(__fastcall*)(void*, void*, const std::byte*);
using GetFovFn = float(__fastcall*)(void*, float, void*, void*);
using CursorFn = void(__fastcall*)(void*);

std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};
std::atomic<KeyboardInputFn> originalKeyboardInput{nullptr};
std::atomic<GetFovFn> originalGetFov{nullptr};
std::atomic<CursorFn> originalGrabCursor{nullptr};
std::atomic<CursorFn> originalReleaseCursor{nullptr};
std::atomic<bool> cursorStateObserved{false};
std::atomic<std::shared_ptr<const zoom::Config>> liveConfig{
    std::make_shared<const zoom::Config>()};
zoom::Controller controller;
std::array<void*, 4> hookTargets{};
bool ownsMinHook = false;

template <class T>
T readAt(const std::byte* input, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, input + offset, sizeof(value));
    return value;
}

void* __fastcall onKeyboardInput(void* context, void* result, const std::byte* input) {
    if (input) {
        // Minecraft's native key-event callback stores the key message at
        // +0x10 and its InputKey at +0x18. This does not poll Windows state.
        const auto message = readAt<std::uint32_t>(input, 0x10);
        const auto inputKey = readAt<std::uint32_t>(input, 0x18);
        const bool down = message == 0x0100u || message == 0x0104u;
        const bool up = message == 0x0101u || message == 0x0105u;
        if (down || up) {
            const auto config = liveConfig.load(std::memory_order_acquire);
            controller.onMinecraftKey(static_cast<int>(inputKey), down, *config);
        }
    }
    if (auto original = originalKeyboardInput.load(std::memory_order_acquire))
        return original(context, result, input);
    return result;
}

float __fastcall onGetFov(void* self, float inputFov, void* a3, void* a4) {
    auto original = originalGetFov.load(std::memory_order_acquire);
    const float fov = original ? original(self, inputFov, a3, a4) : inputFov;
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

bool writeDefaultConfig(const std::filesystem::path& path, std::string& error) {
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
    output << zoom::defaultConfigJson();
    return static_cast<bool>(output);
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
    logger.info("Zoom config applied");
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
    if (!loadConfig(path, logger)) {
        logger.error("Initial config.json is invalid or unreadable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    bedrock::Resolver resolver;
    if (!resolver.loadJson(bedrock::generated::kSymbolDatabase, error) ||
        !resolver.attachWindowsMainModule(error) || !resolver.exactBuild()) {
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    const auto keyboard = resolver.resolve("GameCore::handleKeyboardInput");
    const auto fov = resolver.resolve("LevelRendererPlayer::getFov");
    const auto grab = resolver.resolve("ClientInstance::grabCursor", true);
    const auto release = resolver.resolve("ClientInstance::releaseCursor", true);
    if (!exactRva(keyboard) || !keyboard->verified || !exactRva(fov) || !fov->verified ||
        !exactRva(grab) || !exactRva(release)) {
        logger.error("Required Minecraft native input/FOV symbols are unavailable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    hookTargets = {reinterpret_cast<void*>(keyboard->address),
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
        createHook(hookTargets[0], reinterpret_cast<void*>(&onKeyboardInput), originalKeyboardInput) &&
        createHook(hookTargets[1], reinterpret_cast<void*>(&onGetFov), originalGetFov) &&
        createHook(hookTargets[2], reinterpret_cast<void*>(&onGrabCursor), originalGrabCursor) &&
        createHook(hookTargets[3], reinterpret_cast<void*>(&onReleaseCursor), originalReleaseCursor);
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

    status.store(Status::Running, std::memory_order_release);
    logger.info("Minecraft native InputKey Zoom ready: ", path.string());
    auto lastWrite = std::filesystem::last_write_time(path);
    while (!stopRequested.load(std::memory_order_acquire)) {
        const auto delay = liveConfig.load(std::memory_order_acquire)->reloadIntervalMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
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
