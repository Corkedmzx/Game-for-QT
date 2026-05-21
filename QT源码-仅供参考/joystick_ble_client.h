#ifndef JOYSTICK_BLE_CLIENT_H
#define JOYSTICK_BLE_CLIENT_H

#include <QObject>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QLowEnergyService>

/** ESP32 固件 Nordic UART Service BLE 客户端（Windows 需系统蓝牙与 Qt Bluetooth） */
class JoyBleClient : public QObject
{
    Q_OBJECT

public:
    explicit JoyBleClient(QObject *parent = nullptr);
    ~JoyBleClient() override;

    void startDiscovery();
    void stopDiscovery();
    bool isDiscovering() const;

    void connectToDevice(const QBluetoothDeviceInfo &device);
    void disconnectDevice();
    bool isConnected() const;

    /** 退出前调用：停扫描、异步释放控制器，避免 WinRT 回调访问已释放对象 */
    void prepareForShutdown();
    bool isBusy() const;
    void drainPending(int maxMs = 3000);

    void writeLine(const QByteArray &line);

signals:
    void deviceDiscovered(const QBluetoothDeviceInfo &info);
    void discoveryFinished();
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void bytesReceived(const QByteArray &data);

private slots:
    void onDeviceDiscovered(const QBluetoothDeviceInfo &info);
    void onDiscoveryFinished();
    void onDiscoveryError(QBluetoothDeviceDiscoveryAgent::Error error);
    void onControllerConnected();
    void onControllerDisconnected();
    void onControllerError(QLowEnergyController::Error error);
    void onServiceDiscovered(const QBluetoothUuid &uuid);
    void onServiceScanDone();
    void onCharChanged(const QLowEnergyCharacteristic &c, const QByteArray &value);
    void onDescriptorWritten(const QLowEnergyDescriptor &d, const QByteArray &value);

private:
    void scheduleControllerRelease();
    void subscribeNotify();

    static QBluetoothUuid nusServiceUuid();
    static QBluetoothUuid nusRxUuid();
    static QBluetoothUuid nusTxUuid();

    QBluetoothDeviceDiscoveryAgent *m_agent = nullptr;
    QLowEnergyController *m_controller = nullptr;
    QLowEnergyService *m_service = nullptr;
    QLowEnergyCharacteristic m_chrRx;
    QLowEnergyCharacteristic m_chrTx;
    bool m_connected = false;
    bool m_shuttingDown = false;
};

#endif
