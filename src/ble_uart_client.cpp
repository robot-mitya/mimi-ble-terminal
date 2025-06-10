#include "ble_uart_client.h"
#include <sdbus-c++/sdbus-c++.h>
#include <iostream>

using namespace sdbus;

BleUartClient::BleUartClient() = default;
BleUartClient::~BleUartClient() = default;

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

    // Ищем TX characteristic по UUID
    sdbus::ObjectPath txPath;
    using ObjectMap = std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>>;
    ObjectMap objects;

    auto objMgr = sdbus::createProxy(*connection_, "org.bluez", "/");
    objMgr->finishRegistration();

    auto method = objMgr->createMethodCall("org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto reply = objMgr->callMethod(method);
    reply >> objects;

    const std::string TX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";

    for (const auto& [path, ifaces] : objects) {
        auto itGatt = ifaces.find("org.bluez.GattCharacteristic1");
        if (itGatt != ifaces.end()) {
            const auto& props = itGatt->second;
            auto uuidIt = props.find("UUID");
            if (uuidIt != props.end()) {
                std::string uuid = uuidIt->second.get<std::string>();
                std::transform(uuid.begin(), uuid.end(), uuid.begin(), ::tolower);
                if (uuid == TX_UUID) {
                    txCharPath_ = path;
                    break;
                }
            }
        }
    }

    if (txCharPath_.empty()) {
        std::cerr << "❌ TX Characteristic not found\n";
        return false;
    }

    std::cout << "✅ Connected to " << alias << "\n";
    connected = true;
    return true;
}

void BleUartClient::disconnect() {
    // Заглушка
    connected = false;
}

bool BleUartClient::send(const std::string& text) {
    if (!connected || txCharPath_.empty()) return false;

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
