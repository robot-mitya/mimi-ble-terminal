#ifndef BLE_UART_CLIENT_H
#define BLE_UART_CLIENT_H

#include <vector>
#include <functional>
#include <queue>
#include <sstream>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>

namespace mimi {

template<typename... Args>
std::string str(Args&&... args)
{
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    return oss.str();
}

struct PairedDevice {
    std::string alias;
    std::string address;
    std::string path;
};

class BleUartClient {
public:
    enum class State {
        Disconnected,
        Connected,
        Reconnecting,
    };

    using ConnectCallback = std::function<void(const std::string& deviceAlias, const std::string& message, bool afterFailure)>;
    using DisconnectCallback = std::function<void(const std::string& deviceAlias, const std::string& message, bool isFailure)>;
    using StateChangedCallback = std::function<void(const std::string& deviceAlias, const State& state)>;
    using ErrorCallback = std::function<void(const std::string& deviceAlias, const std::string& message, const std::string& sdbusErrorName, const State& state)>;
    using ReceiveCallback = std::function<void(const std::string& deviceAlias, const std::string& receivedText)>;

    ~BleUartClient();

    void setCallbacks(
        ConnectCallback connectCallback,
        DisconnectCallback disconnectCallback,
        StateChangedCallback stateChangedCallback,
        ErrorCallback errorCallback,
        ReceiveCallback receiveCallback);

    static std::vector<PairedDevice> listPairedDevices();
    bool connect(const std::string& alias, bool keepConnection);
    bool disconnect();
    [[nodiscard]] State getState() const;
    [[nodiscard]] bool send(const std::string& text);
    void processCallbacks();

    static const char* stateToString(const State& state) {
        switch (state) {
            case State::Disconnected: return "Disconnected";
            case State::Connected:    return "Connected";
            case State::Reconnecting: return "Reconnecting";
        }
        return "Unknown";
    }
private:
    ConnectCallback connectCallback_ = nullptr;
    DisconnectCallback disconnectCallback_ = nullptr;
    StateChangedCallback stateChangedCallback_ = nullptr;
    ErrorCallback errorCallback_ = nullptr;
    ReceiveCallback receiveCallback_ = nullptr;

    std::string deviceAlias_;
    bool keepConnection_ = false;
    std::atomic<State> state_ = State::Disconnected;
    void setState(const State& state);

    std::thread reconnectThread_;
    void startReconnectLoop();
    static constexpr int ReconnectIntervalInSeconds = 30;

    sdbus::IConnection* connection_ = nullptr;
    std::unique_ptr<sdbus::IProxy> deviceProxy_;
    std::unique_ptr<sdbus::IProxy> rxProxy_;
    std::string txCharPath_;
    std::string rxCharPath_;
    std::string rxAssembleBuffer_;

    std::queue<std::function<void()>> callbackQueue_;
    std::mutex callbackQueueMutex_;
    void postConnect(const std::string& message, bool afterFailure);
    void postDisconnect(const std::string& message, bool isFailure);
    void postStateChanged(const State& state);
    void postReceive(const std::string& message);
    void postError(const std::string& message, const std::string& sdbusErrorName, const State& state);

    bool doConnect();
    bool findDevice(std::vector<PairedDevice> pairedDevices, PairedDevice& pairedDevice);
    bool connectGatt(const PairedDevice &pairedDevice);
    bool discoverCharacteristics();
    bool setupReceiveNotifications();
    void setupConnectionMonitor();
};

} // namespace mimi

#endif //BLE_UART_CLIENT_H
