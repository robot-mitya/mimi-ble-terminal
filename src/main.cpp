#include "ble_uart_client.h"
#include <iostream>

int main() {
    BleUartClient client;
    auto devices = client.listPairedDevices();

    std::cout << "🔍 Paired devices:\n";
    for (const auto& d : devices) {
        std::cout << " - " << d.alias << " [" << d.address << "]\n";
    }

    // std::cout << "Enter robot alias to connect: ";
    std::string name = "BBC micro:bit";
    // std::getline(std::cin, name);

    if (!client.connectTo(name, nullptr)) {
        std::cerr << "❌ Failed to connect.\n";
        return 1;
    }

    std::cout << "💬 Type commands to send to robot. Type 'q' to quit.\n";
    std::string line;
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, line);
        if (line == "q") break;
        if (!line.empty() && line.back() != '\n') line += "\n";
        client.send(line);
    }

    std::cout << "🔌 Done.\n";
    return 0;
}
