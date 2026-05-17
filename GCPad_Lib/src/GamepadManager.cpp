#include "GamepadManager.h"
#include "sdl_device.h"

#ifdef _WIN32
#include "hid_device.h"
#include "ps_device.h"
#include "xbox_device.h"
#include "nintendo_device.h"
#include "xinput_device.h"
#include "dinput_device.h"
#endif

#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <set>

namespace gcpad {

// GamepadManager implementation
class GamepadManagerImpl : public GamepadManager {
public:
    GamepadManagerImpl();
    ~GamepadManagerImpl() override;

    // GamepadManager interface
    bool initialize() override;
    void shutdown() override;

    int getMaxGamepads() const override { return MAX_GAMEPADS; }
    int getConnectedGamepadCount() const override;
    std::vector<int> getConnectedGamepadIndices() const override;

    GamepadDevice* getGamepad(int index) override;
    const GamepadDevice* getGamepad(int index) const override;

    void setGamepadConnectedCallback(GamepadConnectedCallback callback) override;
    void setGamepadDisconnectedCallback(GamepadDisconnectedCallback callback) override;

    void updateAll() override;

    void setRemapper(std::shared_ptr<Remapper> remapper) override;
    std::shared_ptr<Remapper> getRemapper() const override;

    std::string getLastError() const override { return last_error_; }

private:
    static constexpr int MAX_GAMEPADS = 4;

    std::vector<std::unique_ptr<GamepadDevice>> gamepads_;
    std::vector<std::string> connected_device_paths_;
    std::shared_ptr<Remapper> global_remapper_;

    std::thread hotplug_thread_;
    std::atomic<bool> hotplug_running_;
    mutable std::timed_mutex mutex_;

    GamepadConnectedCallback connected_callback_;
    GamepadDisconnectedCallback disconnected_callback_;

    mutable std::string last_error_;

    // Internal methods
    void hotplug_detection_loop();
    void check_for_disconnected_devices();
    void check_for_sdl_devices();
    int find_available_slot() const;
    int find_slot_for_path(const std::string& path) const;
    void apply_remapper_to_all();

#ifdef _WIN32
    void check_for_new_devices();
    void check_for_xinput_devices();
    void check_for_dinput_devices();
    bool is_xinput_slot(int slot) const;
    bool is_supported_device(uint16_t vendor_id, uint16_t product_id) const;
    std::unique_ptr<GamepadDevice> create_device(std::unique_ptr<internal::HidDevice> hid_device, const internal::HidDeviceAttributes& attributes, int slot);

    // Track which XInput indices are already assigned
    int xinput_slot_map_[4] = {-1, -1, -1, -1};

    // Track DInput instance IDs to avoid re-adding
    std::set<std::string> dinput_guids_;

    // Subsystem init state
    bool dinput_initialized_ = false;
#endif

    // Track SDL instance IDs to avoid re-adding
    std::set<int> sdl_indices_;
    bool sdl_initialized_ = false;
};

GamepadManagerImpl::GamepadManagerImpl()
    : hotplug_running_(false) {
    gamepads_.resize(MAX_GAMEPADS);
    connected_device_paths_.resize(MAX_GAMEPADS);
}

GamepadManagerImpl::~GamepadManagerImpl() {
    shutdown();
}

bool GamepadManagerImpl::initialize() {
    std::lock_guard<std::timed_mutex> lock(mutex_);

    // Initialize SDL2 first so we know which devices SDL has already
    // claimed before we try to open them via raw HID. On Windows, both
    // SDL's HIDAPI backend and our raw HID enumerator will happily open
    // the same DualShock 4 / DualSense, fight over read/write access,
    // and produce garbled or no input. Letting SDL win and skipping the
    // raw HID path for PlayStation controllers avoids that race.
    sdl_initialized_ = internal::initializeSDL();
    if (sdl_initialized_) {
        check_for_sdl_devices();
    }

#ifdef _WIN32
    auto hid_devices = internal::enumerate_hid_devices();

    for (auto& hid_device : hid_devices) {
        std::string current_path = hid_device->device_path();
        if (find_slot_for_path(current_path) != -1) {
            continue;
        }

        // Must open device before reading attributes
        if (!hid_device->open()) {
            continue;
        }

        auto attributes = hid_device->get_attributes();
        if (!is_supported_device(attributes.vendor_id, attributes.product_id)) {
            hid_device->close();
            continue;
        }

        // Skip PlayStation devices on Windows when SDL2 is active — SDL has
        // already claimed them via HIDAPI and a second raw HID open would
        // fight for shared read/write access (see comment in initialize()).
        if (sdl_initialized_) {
            auto ps_infos = internal::getPlayStationDeviceInfos();
            if (std::any_of(ps_infos.begin(), ps_infos.end(), [&](const internal::PlayStationDeviceInfo& info) {
                    return info.vendor_id == attributes.vendor_id && info.product_id == attributes.product_id;
                })) {
                hid_device->close();
                continue;
            }
        }

        int slot = find_available_slot();
        if (slot == -1) {
            hid_device->close();
            continue;
        }

        // Close before handing to device (device re-opens in updateState)
        hid_device->close();

        gamepads_[slot] = create_device(std::move(hid_device), attributes, slot);
        connected_device_paths_[slot] = current_path;

        if (global_remapper_ && gamepads_[slot]) {
            gamepads_[slot]->setRemapper(global_remapper_);
        }

        if (connected_callback_) {
            connected_callback_(slot);
        }
    }

    // Also scan for XInput devices (Xbox controllers that don't expose via raw HID)
    check_for_xinput_devices();

    // Initialize DirectInput and scan for non-HID, non-XInput controllers
    // DirectInput initialization causes hangs when loaded from DLL - disable for now
    //dinput_initialized_ = internal::initializeDInput();
    //if (dinput_initialized_) {
    //    check_for_dinput_devices();
    //}
#endif

    hotplug_running_ = true;
    hotplug_thread_ = std::thread(&GamepadManagerImpl::hotplug_detection_loop, this);

    return true;
}

void GamepadManagerImpl::shutdown() {
    // Signal the hotplug thread to stop BEFORE locking the mutex.
    // The hotplug thread also acquires this mutex, so joining while
    // holding it would deadlock.
    hotplug_running_ = false;
    if (hotplug_thread_.joinable()) {
        hotplug_thread_.join();
    }

    // Now safe to lock — hotplug thread is dead
    std::lock_guard<std::timed_mutex> lock(mutex_);

    for (auto& gamepad : gamepads_) {
        gamepad.reset();
    }
    std::fill(connected_device_paths_.begin(), connected_device_paths_.end(), std::string());
    sdl_indices_.clear();
#ifdef _WIN32
    dinput_guids_.clear();
#endif

    // Shutdown subsystems
    if (sdl_initialized_) {
        internal::shutdownSDL();
        sdl_initialized_ = false;
    }
#ifdef _WIN32
    // DirectInput disabled - no need to shutdown
    //if (dinput_initialized_) {
    //    internal::shutdownDInput();
    //    dinput_initialized_ = false;
    //}
#endif
}

int GamepadManagerImpl::getConnectedGamepadCount() const {
    std::lock_guard<std::timed_mutex> lock(mutex_);

    int count = 0;
    for (const auto& gamepad : gamepads_) {
        if (gamepad && gamepad->isConnected()) {
            ++count;
        }
    }
    return count;
}

std::vector<int> GamepadManagerImpl::getConnectedGamepadIndices() const {
    std::lock_guard<std::timed_mutex> lock(mutex_);

    std::vector<int> indices;
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (gamepads_[i] && gamepads_[i]->isConnected()) {
            indices.push_back(i);
        }
    }
    return indices;
}

GamepadDevice* GamepadManagerImpl::getGamepad(int index) {
    std::lock_guard<std::timed_mutex> lock(mutex_);

    if (index >= 0 && index < MAX_GAMEPADS && gamepads_[index]) {
        return gamepads_[index].get();
    }
    return nullptr;
}

const GamepadDevice* GamepadManagerImpl::getGamepad(int index) const {
    std::lock_guard<std::timed_mutex> lock(mutex_);

    if (index >= 0 && index < MAX_GAMEPADS && gamepads_[index]) {
        return gamepads_[index].get();
    }
    return nullptr;
}

void GamepadManagerImpl::setGamepadConnectedCallback(GamepadConnectedCallback callback) {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    connected_callback_ = callback;
}

void GamepadManagerImpl::setGamepadDisconnectedCallback(GamepadDisconnectedCallback callback) {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    disconnected_callback_ = callback;
}

void GamepadManagerImpl::updateAll() {
    for (auto& gamepad : gamepads_) {
        if (gamepad) {
            if (!gamepad->updateState()) {
            }
        }
    }
}

void GamepadManagerImpl::setRemapper(std::shared_ptr<Remapper> remapper) {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    global_remapper_ = std::move(remapper);
    apply_remapper_to_all();
}

std::shared_ptr<Remapper> GamepadManagerImpl::getRemapper() const {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    return global_remapper_;
}

void GamepadManagerImpl::apply_remapper_to_all() {
    for (auto& gamepad : gamepads_) {
        if (gamepad && global_remapper_) {
            gamepad->setRemapper(global_remapper_);
        }
    }
}

void GamepadManagerImpl::hotplug_detection_loop() {
    while (hotplug_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // FIX: Use try_lock with timeout to avoid blocking game launch.
        // SDL enumeration can freeze momentarily when games are starting.
        std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(100))) {
            // Skip this cycle if mutex is held by another thread
            continue;
        }

#ifdef _WIN32
        check_for_new_devices();
        check_for_xinput_devices();
        // DirectInput disabled - causes hangs when loaded from DLL
        //if (dinput_initialized_) check_for_dinput_devices();
#endif
        if (sdl_initialized_) check_for_sdl_devices();
        check_for_disconnected_devices();
    }
}

#ifdef _WIN32
void GamepadManagerImpl::check_for_new_devices() {
    auto hid_devices = internal::enumerate_hid_devices();

    for (auto& hid_device : hid_devices) {
        auto path = hid_device->device_path();
        if (find_slot_for_path(path) != -1) {
            continue;
        }

        if (!hid_device->open()) {
            continue;
        }

        auto attributes = hid_device->get_attributes();
        if (!is_supported_device(attributes.vendor_id, attributes.product_id)) {
            hid_device->close();
            continue;
        }

        // Same SDL/raw-HID double-claim guard as initialize() — skip PS
        // VID/PIDs on Windows when SDL is the active backend.
        if (sdl_initialized_) {
            auto ps_infos = internal::getPlayStationDeviceInfos();
            if (std::any_of(ps_infos.begin(), ps_infos.end(), [&](const internal::PlayStationDeviceInfo& info) {
                    return info.vendor_id == attributes.vendor_id && info.product_id == attributes.product_id;
                })) {
                hid_device->close();
                continue;
            }
        }

        int slot = find_available_slot();
        if (slot == -1) {
            hid_device->close();
            continue;
        }

        hid_device->close();

        gamepads_[slot] = create_device(std::move(hid_device), attributes, slot);
        connected_device_paths_[slot] = path;

        if (global_remapper_ && gamepads_[slot]) {
            gamepads_[slot]->setRemapper(global_remapper_);
        }

        if (connected_callback_) {
            connected_callback_(slot);
        }
    }
}

void GamepadManagerImpl::check_for_xinput_devices() {
    auto xinput_indices = internal::getConnectedXInputIndices();

    for (int xi : xinput_indices) {
        // Skip if this XInput index already has a slot
        if (xinput_slot_map_[xi] >= 0 && gamepads_[xinput_slot_map_[xi]]) {
            continue;
        }

        int slot = find_available_slot();
        if (slot == -1) break;

        gamepads_[slot] = internal::createXInputDevice(xi, slot);
        connected_device_paths_[slot] = "xinput:" + std::to_string(xi);
        xinput_slot_map_[xi] = slot;

        if (global_remapper_ && gamepads_[slot]) {
            gamepads_[slot]->setRemapper(global_remapper_);
        }

        if (connected_callback_) {
            connected_callback_(slot);
        }
    }
}

bool GamepadManagerImpl::is_xinput_slot(int slot) const {
    for (int i = 0; i < 4; ++i) {
        if (xinput_slot_map_[i] == slot) return true;
    }
    return false;
}

void GamepadManagerImpl::check_for_dinput_devices() {
    auto di_devices = internal::enumerateDInputDevices();

    for (const auto& di_info : di_devices) {
        // Skip if already tracked
        if (dinput_guids_.count(di_info.instance_guid) > 0) {
            continue;
        }
        // Skip if this VID/PID is already connected via HID or XInput
        if (find_slot_for_path("dinput:" + di_info.instance_guid) != -1) {
            continue;
        }

        int slot = find_available_slot();
        if (slot == -1) break;

        auto device = internal::createDInputDevice(di_info.instance_guid, slot);
        if (!device) continue;

        gamepads_[slot] = std::move(device);
        connected_device_paths_[slot] = "dinput:" + di_info.instance_guid;
        dinput_guids_.insert(di_info.instance_guid);

        if (global_remapper_ && gamepads_[slot]) {
            gamepads_[slot]->setRemapper(global_remapper_);
        }

        if (connected_callback_) {
            connected_callback_(slot);
        }
    }
}

#endif // _WIN32

void GamepadManagerImpl::check_for_sdl_devices() {
    auto sdl_devices = internal::enumerateSDLDevices();

    for (const auto& sdl_info : sdl_devices) {
        // Skip if already tracked
        if (sdl_indices_.count(sdl_info.sdl_joystick_index) > 0) {
            continue;
        }
        // Skip if a device with this VID/PID is already connected via another backend
        std::string sdl_path = "sdl:" + std::to_string(sdl_info.sdl_joystick_index);
        if (find_slot_for_path(sdl_path) != -1) {
            continue;
        }

        int slot = find_available_slot();
        if (slot == -1) break;

        auto device = internal::createSDLDevice(sdl_info.sdl_joystick_index, slot);
        if (!device) continue;

        gamepads_[slot] = std::move(device);
        connected_device_paths_[slot] = sdl_path;
        sdl_indices_.insert(sdl_info.sdl_joystick_index);

        if (global_remapper_ && gamepads_[slot]) {
            gamepads_[slot]->setRemapper(global_remapper_);
        }

        if (connected_callback_) {
            connected_callback_(slot);
        }
    }
}

void GamepadManagerImpl::check_for_disconnected_devices() {
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (gamepads_[i] && !gamepads_[i]->isConnected()) {
#ifdef _WIN32
            // Clear XInput slot mapping if applicable
            for (int xi = 0; xi < 4; ++xi) {
                if (xinput_slot_map_[xi] == i) {
                    xinput_slot_map_[xi] = -1;
                    break;
                }
            }

            // Clear DInput GUID tracking
            const auto& path_win = connected_device_paths_[i];
            if (path_win.rfind("dinput:", 0) == 0) {
                dinput_guids_.erase(path_win.substr(7));
            }
#endif
            // Clear SDL index tracking
            const auto& path = connected_device_paths_[i];
            if (path.rfind("sdl:", 0) == 0) {
                try { sdl_indices_.erase(std::stoi(path.substr(4))); } catch (...) {}
            }

            gamepads_[i].reset();
            connected_device_paths_[i].clear();
            if (disconnected_callback_) {
                disconnected_callback_(i);
            }
        }
    }
}

int GamepadManagerImpl::find_available_slot() const {
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (!gamepads_[i]) {
            return i;
        }
    }
    return -1;
}

int GamepadManagerImpl::find_slot_for_path(const std::string& path) const {
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (!connected_device_paths_[i].empty() && connected_device_paths_[i] == path) {
            return i;
        }
    }
    return -1;
}

#ifdef _WIN32
bool GamepadManagerImpl::is_supported_device(uint16_t vendor_id, uint16_t product_id) const {
    auto ps_infos = internal::getPlayStationDeviceInfos();
    for (const auto& info : ps_infos) {
        if (info.vendor_id == vendor_id && info.product_id == product_id) {
            return true;
        }
    }

    auto xbox_infos = internal::getXboxDeviceInfos();
    for (const auto& info : xbox_infos) {
        if (info.vendor_id == vendor_id && info.product_id == product_id) {
            return true;
        }
    }

    auto nintendo_infos = internal::getNintendoDeviceInfos();
    for (const auto& info : nintendo_infos) {
        if (info.vendor_id == vendor_id && info.product_id == product_id) {
            return true;
        }
    }

    return false;
}

std::unique_ptr<GamepadDevice> GamepadManagerImpl::create_device(std::unique_ptr<internal::HidDevice> hid_device, const internal::HidDeviceAttributes& attributes, int slot) {

    auto ps_infos = internal::getPlayStationDeviceInfos();
    if (std::any_of(ps_infos.begin(), ps_infos.end(), [&](const internal::PlayStationDeviceInfo& info) {
            return info.vendor_id == attributes.vendor_id && info.product_id == attributes.product_id;
        })) {
        return internal::createPlayStationDevice(std::move(hid_device), slot);
    }

    auto xbox_infos = internal::getXboxDeviceInfos();
    if (std::any_of(xbox_infos.begin(), xbox_infos.end(), [&](const internal::XboxDeviceInfo& info) {
            return info.vendor_id == attributes.vendor_id && info.product_id == attributes.product_id;
        })) {
        return internal::createXboxDevice(std::move(hid_device), slot);
    }

    auto nintendo_infos = internal::getNintendoDeviceInfos();
    if (std::any_of(nintendo_infos.begin(), nintendo_infos.end(), [&](const internal::NintendoDeviceInfo& info) {
            return info.vendor_id == attributes.vendor_id && info.product_id == attributes.product_id;
        })) {
        return internal::createNintendoDevice(std::move(hid_device), slot);
    }

    return nullptr;
}
#endif // _WIN32

// Factory function
std::unique_ptr<GamepadManager> GamepadManager::create() {
    return std::make_unique<GamepadManagerImpl>();
}

// Convenience functions
std::unique_ptr<GamepadManager> createGamepadManager() {
    return GamepadManager::create();
}

std::vector<HidDeviceInfo> getAllHidDevices() {
    std::vector<HidDeviceInfo> infos;
#ifdef _WIN32
    auto devices = internal::enumerate_hid_devices();

    for (auto& dev : devices) {
        if (!dev->open()) continue;

        auto attrs = dev->get_attributes();
        std::string product_str = dev->get_product_string();
        dev->close();

        if (attrs.vendor_id == 0 && attrs.product_id == 0) continue;

        infos.push_back({
            attrs.vendor_id,
            attrs.product_id,
            product_str,
            dev->device_path()
        });
    }
#endif
    return infos;
}

bool initializeGamepadManager(GamepadManager* manager) {
    return manager ? manager->initialize() : false;
}

void shutdownGamepadManager(GamepadManager* manager) {
    if (manager) {
        manager->shutdown();
    }
}

int getConnectedGamepadCount(GamepadManager* manager) {
    return manager ? manager->getConnectedGamepadCount() : 0;
}

GamepadDevice* getGamepad(GamepadManager* manager, int index) {
    return manager ? manager->getGamepad(index) : nullptr;
}

bool updateGamepad(GamepadDevice* gamepad) {
    return gamepad ? gamepad->updateState() : false;
}

const GamepadState& getGamepadState(GamepadDevice* gamepad) {
    static GamepadState empty_state;
    return gamepad ? gamepad->getState() : empty_state;
}

void setGlobalRemapper(GamepadManager* manager, std::shared_ptr<Remapper> remapper) {
    if (manager) {
        manager->setRemapper(std::move(remapper));
    }
}

std::shared_ptr<Remapper> getGlobalRemapper(GamepadManager* manager) {
    return manager ? manager->getRemapper() : nullptr;
}

} // namespace gcpad