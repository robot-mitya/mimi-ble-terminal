#include "ble_uart_client.h"
#include <iostream>

int main() {
    BleUartClient client;
    auto devices = client.listPairedDevices();

    std::cout << "🔍 Paired devices:\n";
    for (const auto& d : devices) {
        std::cout << " - " << d.alias << " [" << d.address << "]\n";
    }

    return 0;
}
