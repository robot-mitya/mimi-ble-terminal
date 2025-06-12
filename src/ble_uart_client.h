#ifndef BLE_UART_CLIENT_H
#define BLE_UART_CLIENT_H

#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <queue>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>

struct PairedDevice {
    std::string alias;
    std::string address;
    std::string path;
};

class BleUartClient {
public:
    using ReceiveCallback = std::function<void(const std::string&)>;

    BleUartClient();
    ~BleUartClient();

    std::vector<PairedDevice> listPairedDevices();
    bool connectTo(const std::string& alias, ReceiveCallback onReceive);
    void disconnect();
    sdbus::IConnection& connection();
    bool send(const std::string& text);
    void processIncomingMessages(std::ostream& out = std::cout);
private:
    sdbus::IConnection* connection_ = nullptr;
    std::unique_ptr<sdbus::IProxy> deviceProxy_;
    std::unique_ptr<sdbus::IProxy> rxProxy_;
    std::string txCharPath_;
    std::string rxCharPath_;
    bool connected_ = false;

    std::mutex rxQueueMutex_;
    std::queue<std::string> rxQueue_;
    std::function<void(const std::string&)> receiveCallback_;
};

#endif //BLE_UART_CLIENT_H
