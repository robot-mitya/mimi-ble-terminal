#ifndef BLE_UART_CLIENT_H
#define BLE_UART_CLIENT_H

#include <string>
#include <vector>
#include <functional>
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
    bool send(const std::string& text);

private:
    ReceiveCallback receiveCallback;
    sdbus::IConnection* connection_ = nullptr;
    std::unique_ptr<sdbus::IProxy> deviceProxy_;
    std::string txCharPath_;
    bool connected = false;
};

#endif //BLE_UART_CLIENT_H
