#include "ble_uart_client.h"
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <chrono>
#include <thread>

int main() {
    std::cout.setf(std::ios::unitbuf);  // автоматический flush
    BleUartClient client;

    auto devices = client.listPairedDevices();
    std::cout << "🔍 Paired devices:\n";
    for (const auto& d : devices) {
        std::cout << " - " << d.alias << " [" << d.address << "]\n";
    }

    // std::cout << "Enter robot alias to connect: ";
    const std::string name = "BBC micro:bit";
    // std::getline(std::cin, name);

    if (!client.connectTo(name, [](const std::string& msg) {
        std::cout << "\r🤖 " << msg << "\n> " << std::flush;
    })) {
        std::cerr << "❌ Failed to connect.\n";
        return 1;
    }

    // Настроим stdin в неблокирующий режим
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    std::string inputBuffer;
    std::cout << "💬 Type commands to send to robot. Type 'q' to quit.\n";
    std::cout << "> " << std::flush;

    while (true) {
        // Обработка входящих BLE-сообщений
        client.processIncomingMessages();

        // Проверка наличия ввода
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  // 10 ms

        int ret = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            char ch;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n > 0) {
                if (ch == '\n') {
                    if (inputBuffer == "q") {
                        break;
                    }
                    if (!inputBuffer.empty()) {
                        client.send(inputBuffer + "\n");
                    }
                    inputBuffer.clear();
                    std::cout << "> " << std::flush;
                } else {
                    inputBuffer += ch;
                }
            }
        }

        // небольшая задержка для снижения нагрузки на CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "\n🔌 Done.\n";
    return 0;
}
