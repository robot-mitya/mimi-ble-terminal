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
    using ConnectCallback = std::function<void(const std::string&)>;
    using DisconnectCallback = std::function<void(const std::string&, bool isFailure)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using ReceiveCallback = std::function<void(const std::string&)>;

    BleUartClient(
        ConnectCallback connectCallback,
        DisconnectCallback disconnectCallback,
        ErrorCallback errorCallback,
        ReceiveCallback receiveCallback);
    ~BleUartClient();

    static std::vector<PairedDevice> listPairedDevices();
    bool connect(const std::string& alias, bool keepConnection);
    void disconnect();
    [[nodiscard]] bool isConnected() const { return isConnected_; }
    [[nodiscard]] bool send(const std::string& text) const;
    void processIncomingMessages();
private:
    ConnectCallback connectCallback_;
    DisconnectCallback disconnectCallback_;
    ErrorCallback errorCallback_;
    ReceiveCallback receiveCallback_;

    std::string deviceAlias_;
    bool keepConnection_ = false;
    bool isConnected_ = false;

    sdbus::IConnection* connection_ = nullptr;
    std::unique_ptr<sdbus::IProxy> deviceProxy_;
    std::unique_ptr<sdbus::IProxy> rxProxy_;
    std::string txCharPath_;
    std::string rxCharPath_;

    std::mutex rxQueueMutex_;
    std::string rxAssembleBuffer_;
    std::queue<std::string> rxQueue_;

    bool doConnect();
};

#endif //BLE_UART_CLIENT_H
