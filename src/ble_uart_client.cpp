// ReSharper disable CppTooWideScopeInitStatement
#include "ble_uart_client.h"
#include <iomanip>
#include <thread>
#include <utility>
#include <sdbus-c++/sdbus-c++.h>

using namespace sdbus;
using namespace mimi;

BleUartClient::~BleUartClient() {
    disconnect();
    delete connection_;
}

void BleUartClient::setCallbacks(
        ConnectCallback connectCallback,
        DisconnectCallback disconnectCallback,
        StateChangedCallback stateChangedCallback,
        ErrorCallback errorCallback,
        ReceiveCallback receiveCallback) {
    connectCallback_ = std::move(connectCallback);
    disconnectCallback_ = std::move(disconnectCallback);
    stateChangedCallback_ = std::move(stateChangedCallback);
    errorCallback_ = std::move(errorCallback);
    receiveCallback_ = std::move(receiveCallback);
}

BleUartClient::State BleUartClient::getState() const {
    return state_;
}

void BleUartClient::setState(const State& state) {
    if (state == state_) return;
    state_ = state;
    postStateChanged(state);
    processCallbacks();
}

std::vector<PairedDevice> BleUartClient::listPairedDevices() {
    std::vector<PairedDevice> devices;

    const auto connection = createSystemBusConnection();
    const auto proxy = createProxy(*connection, "org.bluez", "/");
    proxy->finishRegistration();

    using VariantMap = std::map<std::string, Variant>;
    using InterfaceMap = std::map<std::string, VariantMap>;
    using ObjectMap = std::map<ObjectPath, InterfaceMap>;

    ObjectMap managedObjects;
    const auto method = proxy->createMethodCall("org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto reply = proxy->callMethod(method);
    reply >> managedObjects;

    for (const auto& [objectPath, interfaces] : managedObjects) {
        auto devIt = interfaces.find("org.bluez.Device1");
        if (devIt != interfaces.end()) {
            const auto& props = devIt->second;

            const auto pairedIt = props.find("Paired");
            const auto aliasIt  = props.find("Alias");
            const auto addrIt   = props.find("Address");

            if (pairedIt != props.end() && pairedIt->second.get<bool>()) {
                const std::string alias   = aliasIt != props.end() ? aliasIt->second.get<std::string>() : "(unknown)";
                const std::string address = addrIt != props.end() ? addrIt->second.get<std::string>() : "(no address)";
                devices.push_back({ alias, address, objectPath });
            }
        }
    }

    return devices;
}

bool BleUartClient::connect(const std::string& alias, const bool keepConnection) {
    if (state_ != State::Disconnected) return true;
    deviceAlias_ = alias;
    keepConnection_ = keepConnection;
    const bool successfullyConnected = doConnect();
    if (!successfullyConnected && keepConnection_) {
        setState(State::Connected);
        processCallbacks();
        startReconnectLoop();
        return true;
    }
    if (successfullyConnected) {
        setState(State::Connected);
        postConnect(str("Connected to \'", deviceAlias_, "\'"), false);
        processCallbacks();
        return true;
    }
    setState(State::Disconnected);
    processCallbacks();
    return false;
}

bool BleUartClient::findDevice(std::vector<PairedDevice> pairedDevices, PairedDevice& pairedDevice) {
    const auto device = std::find_if(pairedDevices.begin(), pairedDevices.end(), [&](const PairedDevice& d) {
        return d.alias == deviceAlias_;
    });
    if (device == pairedDevices.end()) {
        postError(str("Device with alias '", deviceAlias_, "' not found"), "", state_); //❌
        return false;
    }
    pairedDevice = *device;
    return true;
}

bool BleUartClient::connectGatt(const PairedDevice &pairedDevice) {
    connection_ = createSystemBusConnection().release();
    deviceProxy_ = createProxy(*connection_, "org.bluez", pairedDevice.path);
    deviceProxy_->finishRegistration();
    try {
        deviceProxy_->callMethod("Connect").onInterface("org.bluez.Device1");
    } catch (const Error& e) {
        postError(str("Failed to connect: ", e.getMessage()), e.getName(), state_); //❌
        return false;
    }
    connection_->enterEventLoopAsync();
    return true;
}

bool BleUartClient::discoverCharacteristics() {
    using ObjectMap = std::map<ObjectPath, std::map<std::string, std::map<std::string, Variant>>>;
    ObjectMap objects;

    const auto objMgr = createProxy(*connection_, "org.bluez", "/");
    objMgr->finishRegistration();

    const auto method = objMgr->createMethodCall("org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto reply = objMgr->callMethod(method);
    reply >> objects;

    const std::string TX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
    const std::string RX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

    for (const auto& [path, ifaces] : objects) {
        auto itGatt = ifaces.find("org.bluez.GattCharacteristic1");
        if (itGatt != ifaces.end()) {
            const auto& props = itGatt->second;
            auto uuidIt = props.find("UUID");
            if (uuidIt != props.end()) {
                auto uuid = uuidIt->second.get<std::string>();
                std::transform(uuid.begin(), uuid.end(), uuid.begin(), tolower);

                if (uuid == TX_UUID && txCharPath_.empty())
                    txCharPath_ = path;

                if (uuid == RX_UUID && rxCharPath_.empty())
                    rxCharPath_ = path;
            }
        }
    }

    if (txCharPath_.empty() || rxCharPath_.empty()) {
        postError(str("TX or RX characteristic not found"), "", state_); //❌
        return false;
    }

    return true;
}

bool BleUartClient::setupReceiveNotifications() {
    rxProxy_ = createProxy(*connection_, "org.bluez", rxCharPath_);
    rxProxy_->uponSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .call([this](const std::string& interface,
                     const std::map<std::string, Variant>& changed,
                     const std::vector<std::string>&) {
            if (interface == "org.bluez.GattCharacteristic1") {
                const auto value = changed.find("Value");
                if (value != changed.end()) {
                    const auto& vec = value->second.get<std::vector<uint8_t>>();
                    const std::string fragment(vec.begin(), vec.end());
                    rxAssembleBuffer_ += fragment;

                    size_t pos;
                    while ((pos = rxAssembleBuffer_.find('\n')) != std::string::npos) {
                        std::string rawMessage = rxAssembleBuffer_.substr(0, pos);
                        rxAssembleBuffer_.erase(0, pos + 1);

                        std::string message;
                        std::copy_if(rawMessage.begin(), rawMessage.end(), std::back_inserter(message),
                                     [](const char c) { return c != '\r'; });

                        postReceive(message);
                    }
                }
            }
        });
    rxProxy_->finishRegistration();

    try {
        rxProxy_->callMethod("StartNotify").onInterface("org.bluez.GattCharacteristic1");
    } catch (const Error& e) {
        postError(str("Failed to start notifications: ", e.getMessage()), e.getName(), state_); //❌
        return false;
    }

    return true;
}

void BleUartClient::setupConnectionMonitor() {
    deviceProxy_->uponSignal("PropertiesChanged")
    .onInterface("org.freedesktop.DBus.Properties")
    .call([this](const std::string& interface,
                 const std::map<std::string, Variant>& changed,
                 const std::vector<std::string>&) {
        if (interface == "org.bluez.Device1") {
            const auto it = changed.find("Connected");
            if (it != changed.end() && !it->second.get<bool>()) {
                // Соединение незапланированно потеряно
                postDisconnect(str("Disconnected from \'", deviceAlias_, "\'"), true);
                if (keepConnection_) {
                    startReconnectLoop();
                }
            }
        }
    });
    deviceProxy_->finishRegistration();
}

bool BleUartClient::doConnect() {
    const std::vector<PairedDevice> devices = listPairedDevices();

    PairedDevice device;
    if (!findDevice(devices, device)) return false;

    if (!connectGatt(device)) return false;

    if (!discoverCharacteristics()) return false;

    setupReceiveNotifications();
    setupConnectionMonitor();

    return true;
}

void BleUartClient::startReconnectLoop() {
    if (state_ != State::Connected) return;
    setState(State::Reconnecting);

    reconnectThread_ = std::thread([this] {
        while (keepConnection_ && state_ == State::Reconnecting) {
            std::this_thread::sleep_for(std::chrono::seconds(ReconnectIntervalInSeconds));

            if (state_ == State::Reconnecting && doConnect()) {
                postConnect(str("Reconnected to \'", deviceAlias_, "\'"), true);
                setState(State::Connected);
                return;
            }
        }
    });

    reconnectThread_.detach();
}

bool BleUartClient::disconnect() {
    if (state_ == State::Disconnected) return false;

    if (rxProxy_) {
        try {
            rxProxy_->callMethod("StopNotify").onInterface("org.bluez.GattCharacteristic1");
        } catch (...) { /* ignore */ }
        rxProxy_.reset();
    }

    if (deviceProxy_) {
        try {
            deviceProxy_->callMethod("Disconnect").onInterface("org.bluez.Device1");
        } catch (...) { }
        deviceProxy_.reset();
    }

    setState(State::Disconnected);
    postDisconnect(str("Disconnected from \'", deviceAlias_, "\'"), false);
    processCallbacks();
    return true;
}

bool BleUartClient::send(const std::string& text) {
    if (state_ != State::Connected || txCharPath_.empty()) {
        postError("Not connected", "", state_); //❌
        return false;
    }

    const auto charProxy = createProxy(*connection_, "org.bluez", txCharPath_);
    charProxy->finishRegistration();

    try {
        constexpr long maxChunkSize = 19;
        for (long offset = 0; offset < text.size(); offset += maxChunkSize) {
            if (offset > 0) {
                // Make a pause between chunks:
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            const long chunkLen = std::min(maxChunkSize, static_cast<long>(text.size()) - offset);
            std::vector<uint8_t> chunk(text.begin() + offset, text.begin() + offset + chunkLen);

            charProxy->callMethod("WriteValue")
                .onInterface("org.bluez.GattCharacteristic1")
                .withArguments(chunk, std::map<std::string, Variant>{});
        }

        return true;
    } catch (const Error& e) {
        postError(str("Send failed: ", e.getMessage()), e.getName(), state_); //❌
        return false;
    }
}

void BleUartClient::processCallbacks() {
    std::queue<std::function<void()>> pending;
    {
        std::lock_guard lock(callbackQueueMutex_);
        std::swap(pending, callbackQueue_);
    }
    while (!pending.empty()) {
        pending.front()();
        pending.pop();
    }
}

void BleUartClient::postConnect(const std::string& message, const bool afterFailure) {
    std::lock_guard lock(callbackQueueMutex_);
    callbackQueue_.emplace([=] { if (connectCallback_) connectCallback_(deviceAlias_, message, afterFailure); });
}

void BleUartClient::postDisconnect(const std::string& message, const bool isFailure) {
    std::lock_guard lock(callbackQueueMutex_);
    callbackQueue_.emplace([=] { if (disconnectCallback_) disconnectCallback_(deviceAlias_, message, isFailure); });
}

void BleUartClient::postStateChanged(const State& state) {
    std::lock_guard lock(callbackQueueMutex_);
    callbackQueue_.emplace([=] { if (stateChangedCallback_) stateChangedCallback_(deviceAlias_, state); });
}

void BleUartClient::postReceive(const std::string& message) {
    std::lock_guard lock(callbackQueueMutex_);
    callbackQueue_.emplace([=] { if (receiveCallback_) receiveCallback_(deviceAlias_, message); });
}

void BleUartClient::postError(const std::string& message, const std::string& sdbusErrorName, const State& state) {
    std::lock_guard lock(callbackQueueMutex_);
    callbackQueue_.emplace([=] { if (errorCallback_) errorCallback_(deviceAlias_, message, sdbusErrorName, state); });
}
