// ReSharper disable CppTooWideScopeInitStatement
#include "ble_uart_client.h"
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <chrono>
#include <filesystem>
#include <thread>

std::string get_robot_name_from_args(const int argc, char* argv[]) {
    const std::string prefix = "--robot-name=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) { // starts with prefix
            return arg.substr(prefix.length());
        }
    }
    return {};
}

int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf);  // автоматический flush

    const auto devices = BleUartClient::listPairedDevices();
    std::cout << "🔍 Paired devices:\n";
    for (const auto& d : devices) {
        std::cout << " - " << d.alias << " [" << d.address << "]\n";
    }

    std::string name = get_robot_name_from_args(argc, argv);
    // if (name.empty()) name = "BBC micro:bit";
    if (name.empty()) {
        std::string execName = std::filesystem::path(argv[0]).filename().string();
        std::cout << "\nUsage: " << execName << " --robot-name=\"<alias>\"" << std::endl;
        return 1;
    }

    BleUartClient client(
        [](const std::string& infoText) {
            std::cout << infoText;
        },
        [](const std::string& errorText) {
            std::cerr << errorText;
        }
    );
    if (!client.connectTo(name, [](const std::string& msg) {
        std::cout << "\r🤖 " << msg << "\n> " << std::flush;
    })) {
        std::cerr << "❌ Failed to connect.\n";
        return 1;
    }

    // Настроим stdin в неблокирующий режим
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
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

        timeval timeout = { 0, 10000 };

        const int ret = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            char ch;
            const ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n > 0) {
                if (ch == '\n') {
                    if (inputBuffer == "q") {
                        break;
                    }
                    if (!inputBuffer.empty()) {
                        const bool sent = client.send(inputBuffer + "\n");
                        if (!sent) {
                            std::cerr << "❌ Failed to send.\n";
                        }
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
