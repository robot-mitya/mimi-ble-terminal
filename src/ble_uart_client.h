#ifndef BLE_UART_CLIENT_H
#define BLE_UART_CLIENT_H

#include <string>
#include <vector>
#include <functional>
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
    using InfoCallback = std::function<void(const std::string&)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using ReceiveCallback = std::function<void(const std::string&)>;

    BleUartClient(InfoCallback infoCallback, ErrorCallback errorCallback);
    ~BleUartClient();

    static std::vector<PairedDevice> listPairedDevices();
    bool connectTo(const std::string& alias, ReceiveCallback onReceive);
    void disconnect();
    [[nodiscard]] bool send(const std::string& text) const;
    void processIncomingMessages();
private:
    sdbus::IConnection* connection_ = nullptr;
    std::unique_ptr<sdbus::IProxy> deviceProxy_;
    std::unique_ptr<sdbus::IProxy> rxProxy_;
    std::string txCharPath_;
    std::string rxCharPath_;
    bool connected_ = false;

    std::mutex rxQueueMutex_;
    std::queue<std::string> rxQueue_;
    std::function<void(const std::string&)> infoCallback_;
    std::function<void(const std::string&)> errorCallback_;
    std::function<void(const std::string&)> receiveCallback_;
};

#endif //BLE_UART_CLIENT_H
