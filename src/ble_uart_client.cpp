// ReSharper disable CppTooWideScopeInitStatement
#include "ble_uart_client.h"

#include <iomanip>
#include <iostream>
#include <thread>
#include <utility>
#include <sdbus-c++/sdbus-c++.h>

using namespace sdbus;

template<typename... Args>
std::string str(Args&&... args)
{
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    return oss.str();
}

BleUartClient::BleUartClient(
        ConnectCallback connectCallback,
        DisconnectCallback disconnectCallback,
        ErrorCallback errorCallback,
        ReceiveCallback receiveCallback) :
    connectCallback_(std::move(connectCallback)),
    disconnectCallback_(std::move(disconnectCallback)),
    errorCallback_(std::move(errorCallback)),
    receiveCallback_(std::move(receiveCallback)) {}

BleUartClient::~BleUartClient() {
    disconnect();
    delete connection_;
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
    if (isConnected_) return true;
    deviceAlias_ = alias;
    keepConnection_ = keepConnection;
    const bool result = doConnect();
    if (keepConnection_) {
        isConnected_ = true;
        //TODO Start calling doConnect() iterations after delay...
    } else {
        if (result) {
            isConnected_ = true;
            connectCallback_(str("Connected to ", deviceAlias_));
        }
    }
    return result;
}

bool BleUartClient::doConnect() {
    auto devices = listPairedDevices();
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const PairedDevice& d) {
        return d.alias == deviceAlias_;
    });

    if (it == devices.end()) {
        errorCallback_(str("Device with alias '", deviceAlias_, "' not found")); //❌
        return false;
    }

    const std::string& devPath = it->path;
    connection_ = createSystemBusConnection().release();
    deviceProxy_ = createProxy(*connection_, "org.bluez", devPath);
    deviceProxy_->finishRegistration();

    try {
        deviceProxy_->callMethod("Connect").onInterface("org.bluez.Device1");
    } catch (const Error& e) {
        errorCallback_(str("Failed to connect: ", e.getMessage(), " [", e.getName(), "]")); //❌
        return false;
    }

    // Ищем TX и RX characteristics
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
        errorCallback_(str("TX or RX characteristic not found")); //❌
        return false;
    }

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
                    std::string rawMessage(vec.begin(), vec.end());
                    std::string message;
                    std::copy_if(rawMessage.begin(), rawMessage.end(), std::back_inserter(message),
                    [](const char c) { return c != '\r'; });
                    {
                        std::lock_guard lock(rxQueueMutex_);
                        rxQueue_.push(message);
                    }
                }
            }
        });
    rxProxy_->finishRegistration();

    try {
        rxProxy_->callMethod("StartNotify").onInterface("org.bluez.GattCharacteristic1");
    } catch (const Error& e) {
        errorCallback_(str("Failed to start notifications: ", e.getMessage(), " [", e.getName(), "]")); //❌
        return false;
    }

    connection_->enterEventLoopAsync();
    return true;
}

void BleUartClient::disconnect() {
    if (!isConnected_) return;

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

    isConnected_ = false;
    disconnectCallback_("Disconnected", false);
}

bool BleUartClient::send(const std::string& text) const {
    if (!isConnected_ || txCharPath_.empty()) {
        errorCallback_("Not connected"); //❌
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
        errorCallback_(str("Send failed: ", e.getMessage(), " [", e.getName(), "]")); //❌
        return false;
    }
}

void BleUartClient::processIncomingMessages() {
    std::queue<std::string> pending;

    {
        std::lock_guard lock(rxQueueMutex_);
        std::swap(pending, rxQueue_);
    }

    while (!pending.empty()) {
        const std::string& fragment = pending.front();
        rxAssembleBuffer_ += fragment;

        size_t pos;
        while ((pos = rxAssembleBuffer_.find('\n')) != std::string::npos) {
            std::string messageText = rxAssembleBuffer_.substr(0, pos);
            if (receiveCallback_)
                receiveCallback_(messageText);

            rxAssembleBuffer_.erase(0, pos + 1); // delete message substr including '\n'
        }

        pending.pop();
    }
}
