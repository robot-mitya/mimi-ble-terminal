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

bool prompt_has_been_shown = false;

void output_command_prompt() {
    if (prompt_has_been_shown) std::cout << "> " << std::flush;
}

int main(const int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf);  // автоматический flush

    const auto devices = BleUartClient::listPairedDevices();
    std::cout << "📌 Paired devices (output format \"<name> [MAC-address]\"):\n";
    for (const auto& d : devices) {
        std::cout << " • " << d.alias << " [" << d.address << "]\n";
    }

    const std::string name = get_robot_name_from_args(argc, argv);
    // if (name.empty()) name = "BBC micro:bit";
    if (name.empty()) {
        const std::string execName = std::filesystem::path(argv[0]).filename().string();
        std::cout << "\nUsage: " << execName << " --robot-name=\"<alias>\"" << std::endl;
        return EXIT_FAILURE;
    }

    BleUartClient client(
        [](const std::string& connectedText, const bool afterFailure) {
            std::cout << "\r✅ " << connectedText << std::endl;
            if (afterFailure) {
                output_command_prompt();
            }
        },
        [](const std::string& disconnectedText, const bool isFailure) {
            if (isFailure) {
                std::cout << "\r❌ " << disconnectedText << std::endl;
                output_command_prompt();
            } else {
                std::cout << "\r❎ " << disconnectedText << std::endl;
            }
        },
        [](const std::string& errorText, const bool isConnected) {
            std::cout << "\r❌ " << errorText << std::endl;
            if (isConnected) {
                output_command_prompt();
            }
        },
        [](const std::string& receivedMessage) {
            std::cout << "\r🤖 " << receivedMessage << std::endl;
            output_command_prompt();
        }
    );

    std::cout << "\n🛜 Connecting to \'" << name << "\'..." << std::endl;
    const bool isConnected = client.connect(name, true);
    if (!isConnected) {
        return EXIT_FAILURE;
    }

    prompt_has_been_shown = true;
    std::cout << "\n💬 Type commands to send to robot. Type 'q' to quit." << std::endl;
    output_command_prompt();

    std::string inputBuffer;
    // Настроим stdin в неблокирующий режим
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    while (true) {
        // Обработка входящих BLE-сообщений
        client.processCallbacks();

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
                        if (client.send(inputBuffer + "\n")) {
                            output_command_prompt();
                        }
                    }
                    inputBuffer.clear();
                } else {
                    inputBuffer += ch;
                }
            }
        }

        // небольшая задержка для снижения нагрузки на CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    client.disconnect();
    std::cout << "\n";
    return EXIT_SUCCESS;
}
