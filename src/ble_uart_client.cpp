#include "ble_uart_client.h"

#include <iomanip>
#include <sdbus-c++/sdbus-c++.h>

using namespace sdbus;

BleUartClient::BleUartClient() = default;

BleUartClient::~BleUartClient() {
    disconnect();
    delete connection_;
}

std::vector<PairedDevice> BleUartClient::listPairedDevices() {
    std::vector<PairedDevice> devices;

    auto connection = sdbus::createSystemBusConnection();
    auto proxy = sdbus::createProxy(*connection, "org.bluez", "/");
    proxy->finishRegistration();

    using VariantMap = std::map<std::string, sdbus::Variant>;
    using InterfaceMap = std::map<std::string, VariantMap>;
    using ObjectMap = std::map<sdbus::ObjectPath, InterfaceMap>;

    ObjectMap managedObjects;
    auto method = proxy->createMethodCall("org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto reply = proxy->callMethod(method);
    reply >> managedObjects;

    for (const auto& [objectPath, interfaces] : managedObjects) {
        auto devIt = interfaces.find("org.bluez.Device1");
        if (devIt != interfaces.end()) {
            const auto& props = devIt->second;

            auto pairedIt = props.find("Paired");
            auto aliasIt  = props.find("Alias");
            auto addrIt   = props.find("Address");

            if (pairedIt != props.end() && pairedIt->second.get<bool>()) {
                std::string alias   = (aliasIt != props.end()) ? aliasIt->second.get<std::string>() : "(unknown)";
                std::string address = (addrIt != props.end()) ? addrIt->second.get<std::string>() : "(no address)";
                devices.push_back({ alias, address, objectPath });
            }
        }
    }

    return devices;
}

bool BleUartClient::connectTo(const std::string& alias, ReceiveCallback onReceive) {
    auto devices = listPairedDevices();
    auto it = std::find_if(devices.begin(), devices.end(), [&](const PairedDevice& d) {
        return d.alias == alias;
    });

    if (it == devices.end()) {
        std::cerr << "❌ Device with alias '" << alias << "' not found\n";
        return false;
    }

    const std::string& devPath = it->path;
    connection_ = sdbus::createSystemBusConnection().release();
    deviceProxy_ = sdbus::createProxy(*connection_, "org.bluez", devPath);
    deviceProxy_->finishRegistration();

    try {
        std::cout << "📡 Connecting to " << alias << "...\n";
        deviceProxy_->callMethod("Connect").onInterface("org.bluez.Device1");
    } catch (const sdbus::Error& e) {
        std::cerr << "❌ Failed to connect: " << e.getName() << " - " << e.getMessage() << "\n";
        return false;
    }

    // Ищем TX и RX characteristics
    using ObjectMap = std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>>;
    ObjectMap objects;

    auto objMgr = sdbus::createProxy(*connection_, "org.bluez", "/");
    objMgr->finishRegistration();

    auto method = objMgr->createMethodCall("org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
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
                std::transform(uuid.begin(), uuid.end(), uuid.begin(), ::tolower);

                if (uuid == TX_UUID && txCharPath_.empty())
                    txCharPath_ = path;

                if (uuid == RX_UUID && rxCharPath_.empty())
                    rxCharPath_ = path;
            }
        }
    }

    if (txCharPath_.empty() || rxCharPath_.empty()) {
        std::cerr << "❌ TX or RX characteristic not found\n";
        return false;
    }

    receiveCallback_ = std::move(onReceive);

    rxProxy_ = sdbus::createProxy(*connection_, "org.bluez", rxCharPath_);
    rxProxy_->uponSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .call([this](const std::string& interface,
                     const std::map<std::string, sdbus::Variant>& changed,
                     const std::vector<std::string>&) {
            if (interface == "org.bluez.GattCharacteristic1") {
                auto it = changed.find("Value");
                if (it != changed.end()) {
                    const auto& vec = it->second.get<std::vector<uint8_t>>();
                    std::string rawMessage(vec.begin(), vec.end());
                    std::string message;
                    std::copy_if(rawMessage.begin(), rawMessage.end(), std::back_inserter(message),
                                 [](char c) { return c != '\r' && c != '\n'; });
                    {
                        std::lock_guard<std::mutex> lock(rxQueueMutex_);
                        rxQueue_.push(message);
                    }
                }
            }
        });
    rxProxy_->finishRegistration();

    try {
        rxProxy_->callMethod("StartNotify").onInterface("org.bluez.GattCharacteristic1");
    } catch (const sdbus::Error& e) {
        std::cerr << "⚠️ Failed to start notifications: " << e.getMessage() << "\n";
        return false;
    }

    connection_->enterEventLoopAsync();

    connected_ = true;
    std::cout << "✅ Connected to " << alias << "\n";
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

bool BleUartClient::send(const std::string& text) {
    if (!connected_ || txCharPath_.empty()) return false;

    auto charProxy = sdbus::createProxy(*connection_, "org.bluez", txCharPath_);
    charProxy->finishRegistration();

    std::vector<uint8_t> data(text.begin(), text.end());

    try {
        charProxy->callMethod("WriteValue")
            .onInterface("org.bluez.GattCharacteristic1")
            .withArguments(data, std::map<std::string, sdbus::Variant>{});
        return true;
    } catch (const sdbus::Error& e) {
        std::cerr << "❌ Send failed: " << e.getMessage() << "\n";
        return false;
    }
}

void BleUartClient::processIncomingMessages(std::ostream& out) {
    std::queue<std::string> pending;

    {
        std::lock_guard<std::mutex> lock(rxQueueMutex_);
        std::swap(pending, rxQueue_);
    }

    while (!pending.empty()) {
        if (receiveCallback_) receiveCallback_(pending.front());
        pending.pop();
    }
}

sdbus::IConnection& BleUartClient::connection() {
    return *connection_;
}
