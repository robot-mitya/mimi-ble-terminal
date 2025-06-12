// ReSharper disable CppTooWideScopeInitStatement
#include "ble_uart_client.h"

#include <iomanip>
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

BleUartClient::BleUartClient(InfoCallback infoCallback, ErrorCallback errorCallback) :
    infoCallback_(std::move(infoCallback)), errorCallback_(std::move(errorCallback)) {}

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

bool BleUartClient::connectTo(const std::string& alias, ReceiveCallback onReceive) {
    auto devices = listPairedDevices();
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const PairedDevice& d) {
        return d.alias == alias;
    });

    if (it == devices.end()) {
        errorCallback_(str("❌ Device with alias '", alias, "' not found\n"));
        return false;
    }

    const std::string& devPath = it->path;
    connection_ = createSystemBusConnection().release();
    deviceProxy_ = createProxy(*connection_, "org.bluez", devPath);
    deviceProxy_->finishRegistration();

    try {
        infoCallback_(str("📡 Connecting to ", alias, "...\n"));
        deviceProxy_->callMethod("Connect").onInterface("org.bluez.Device1");
    } catch (const Error& e) {
        errorCallback_(str("❌ Failed to connect: ", e.getName(), " - ", e.getMessage(), "\n"));
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
        errorCallback_(str("❌ TX or RX characteristic not found\n"));
        return false;
    }

    receiveCallback_ = std::move(onReceive);

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
                                 [](const char c) { return c != '\r' && c != '\n'; });
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
        errorCallback_(str("⚠️ Failed to start notifications: ", e.getMessage(), "\n"));
        return false;
    }

    connection_->enterEventLoopAsync();

    connected_ = true;
    infoCallback_(str("✅ Connected to ", alias, "\n"));
    return true;
}

void BleUartClient::disconnect() {
    if (!connected_) return;

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

    connected_ = false;
}

bool BleUartClient::send(const std::string& text) const {
    if (!connected_ || txCharPath_.empty()) return false;

    const auto charProxy = createProxy(*connection_, "org.bluez", txCharPath_);
    charProxy->finishRegistration();

    std::vector<uint8_t> data(text.begin(), text.end());

    try {
        charProxy->callMethod("WriteValue")
            .onInterface("org.bluez.GattCharacteristic1")
            .withArguments(data, std::map<std::string, Variant>{});
        return true;
    } catch (const Error& e) {
        errorCallback_(str("❌ Send failed: ", e.getMessage(), "\n"));
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
        if (receiveCallback_) receiveCallback_(pending.front());
        pending.pop();
    }
}
