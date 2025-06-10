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
    // Заглушка: реализуем позже
    receiveCallback = onReceive;
    std::cerr << "[NOT IMPLEMENTED] connectTo() is not ready yet\n";
    return false;
}

void BleUartClient::disconnect() {
    // Заглушка
    connected = false;
}

bool BleUartClient::send(const std::string& text) {
    // Заглушка
    return false;
}
