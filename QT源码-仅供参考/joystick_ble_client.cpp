#include "joystick_ble_client.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothLocalDevice>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QPointer>

JoyBleClient::JoyBleClient(QObject *parent)
    : QObject(parent)
    , m_agent(new QBluetoothDeviceDiscoveryAgent(this))
{
    m_agent->setLowEnergyDiscoveryTimeout(8000);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
            this, &JoyBleClient::onDeviceDiscovered);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::finished,
            this, &JoyBleClient::onDiscoveryFinished);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred,
            this, &JoyBleClient::onDiscoveryError);
}

JoyBleClient::~JoyBleClient()
{
    prepareForShutdown();
    drainPending(2000);
}

QBluetoothUuid JoyBleClient::nusServiceUuid()
{
    return QBluetoothUuid(QStringLiteral("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"));
}

QBluetoothUuid JoyBleClient::nusRxUuid()
{
    return QBluetoothUuid(QStringLiteral("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"));
}

QBluetoothUuid JoyBleClient::nusTxUuid()
{
    return QBluetoothUuid(QStringLiteral("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"));
}

void JoyBleClient::prepareForShutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    m_connected = false;

    if (m_agent) {
        m_agent->disconnect(this);
        if (m_agent->isActive()) {
            m_agent->stop();
        }
    }

    scheduleControllerRelease();
}

bool JoyBleClient::isBusy() const
{
    if (m_shuttingDown) {
        return m_controller != nullptr;
    }
    if (m_agent && m_agent->isActive()) {
        return true;
    }
    return m_controller != nullptr;
}

void JoyBleClient::drainPending(int maxMs)
{
    QElapsedTimer timer;
    timer.start();
    while (isBusy() && timer.elapsed() < maxMs) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    }
}

void JoyBleClient::startDiscovery()
{
    if (m_shuttingDown) {
        return;
    }
    QBluetoothLocalDevice local;
    if (!local.isValid()) {
        emit errorOccurred(QStringLiteral("本机蓝牙不可用，请在 Windows 设置中打开蓝牙。"));
        return;
    }
    m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void JoyBleClient::stopDiscovery()
{
    if (!m_agent) {
        return;
    }
    if (m_agent->isActive()) {
        m_agent->stop();
    }
}

bool JoyBleClient::isDiscovering() const
{
    return m_agent && m_agent->isActive();
}

void JoyBleClient::onDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
    if (m_shuttingDown) {
        return;
    }
    if (!(info.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration)) {
        return;
    }
    const QString name = info.name();
    if (name.contains(QStringLiteral("QTgame"), Qt::CaseInsensitive)) {
        emit deviceDiscovered(info);
    }
}

void JoyBleClient::onDiscoveryFinished()
{
    if (m_shuttingDown) {
        return;
    }
    emit discoveryFinished();
}

void JoyBleClient::onDiscoveryError(QBluetoothDeviceDiscoveryAgent::Error error)
{
    Q_UNUSED(error);
    if (m_shuttingDown) {
        return;
    }
    emit errorOccurred(QStringLiteral("蓝牙扫描失败：%1").arg(m_agent->errorString()));
}

void JoyBleClient::scheduleControllerRelease()
{
    m_connected = false;
    m_chrRx = QLowEnergyCharacteristic();
    m_chrTx = QLowEnergyCharacteristic();

    QLowEnergyService *service = m_service;
    QLowEnergyController *controller = m_controller;
    m_service = nullptr;
    m_controller = nullptr;

    if (service) {
        service->disconnect(this);
        service->deleteLater();
    }

    if (!controller) {
        return;
    }

    controller->disconnect(this);

    if (controller->state() == QLowEnergyController::UnconnectedState) {
        controller->deleteLater();
        return;
    }

    QObject::connect(controller, &QLowEnergyController::disconnected, controller,
                     [controller]() { controller->deleteLater(); }, Qt::SingleShotConnection);
    controller->disconnectFromDevice();
}

void JoyBleClient::connectToDevice(const QBluetoothDeviceInfo &device)
{
    if (m_shuttingDown) {
        return;
    }

    stopDiscovery();
    if (m_controller) {
        disconnectDevice();
        drainPending(2000);
    }
    if (m_shuttingDown) {
        return;
    }

    /* 勿设 parent：由 scheduleControllerRelease 异步 deleteLater */
    m_controller = QLowEnergyController::createCentral(device);
    connect(m_controller, &QLowEnergyController::connected,
            this, &JoyBleClient::onControllerConnected);
    connect(m_controller, &QLowEnergyController::disconnected,
            this, &JoyBleClient::onControllerDisconnected);
    connect(m_controller, &QLowEnergyController::errorOccurred,
            this, &JoyBleClient::onControllerError);
    connect(m_controller, &QLowEnergyController::serviceDiscovered,
            this, &JoyBleClient::onServiceDiscovered);
    connect(m_controller, &QLowEnergyController::discoveryFinished,
            this, &JoyBleClient::onServiceScanDone);

    m_controller->connectToDevice();
}

void JoyBleClient::disconnectDevice()
{
    scheduleControllerRelease();
}

bool JoyBleClient::isConnected() const
{
    return m_connected && !m_shuttingDown;
}

void JoyBleClient::onControllerConnected()
{
    if (m_shuttingDown || !m_controller) {
        return;
    }
    m_controller->discoverServices();
}

void JoyBleClient::onControllerDisconnected()
{
    if (m_shuttingDown) {
        return;
    }
    const bool was = m_connected;
    scheduleControllerRelease();
    if (was) {
        emit disconnected();
    }
}

void JoyBleClient::onControllerError(QLowEnergyController::Error error)
{
    Q_UNUSED(error);
    if (m_shuttingDown) {
        return;
    }
    emit errorOccurred(QStringLiteral("蓝牙连接错误：%1").arg(m_controller ? m_controller->errorString()
                                                                           : QString()));
}

void JoyBleClient::onServiceDiscovered(const QBluetoothUuid &uuid)
{
    if (m_shuttingDown || !m_controller || m_service) {
        return;
    }
    if (uuid != nusServiceUuid()) {
        return;
    }

    m_service = m_controller->createServiceObject(uuid);
    if (!m_service) {
        return;
    }

    QPointer<JoyBleClient> self(this);
    connect(m_service, &QLowEnergyService::stateChanged, this,
            [self](QLowEnergyService::ServiceState st) {
                if (!self || self->m_shuttingDown || !self->m_service) {
                    return;
                }
                if (st != QLowEnergyService::ServiceDiscovered) {
                    return;
                }
                self->m_chrRx = self->m_service->characteristic(nusRxUuid());
                self->m_chrTx = self->m_service->characteristic(nusTxUuid());
                if (!self->m_chrRx.isValid() || !self->m_chrTx.isValid()) {
                    emit self->errorOccurred(QStringLiteral("未找到 Nordic UART 特征。"));
                    return;
                }
                self->subscribeNotify();
            });
    connect(m_service, &QLowEnergyService::characteristicChanged,
            this, &JoyBleClient::onCharChanged);
    m_service->discoverDetails();
}

void JoyBleClient::onServiceScanDone()
{
    if (m_shuttingDown) {
        return;
    }
    if (!m_service) {
        emit errorOccurred(QStringLiteral("未找到 QTgame 蓝牙服务，请确认固件已烧录 BLE 版本。"));
    }
}

void JoyBleClient::subscribeNotify()
{
    if (m_shuttingDown || !m_service) {
        return;
    }
    const QLowEnergyDescriptor cccd = m_chrTx.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
    if (!cccd.isValid()) {
        emit errorOccurred(QStringLiteral("无法订阅摇杆数据通知。"));
        return;
    }
    m_service->writeDescriptor(cccd, QByteArray::fromHex("0100"));
    m_connected = true;
    emit connected();
}

void JoyBleClient::onCharChanged(const QLowEnergyCharacteristic &c, const QByteArray &value)
{
    if (m_shuttingDown) {
        return;
    }
    if (c.uuid() == nusTxUuid() && !value.isEmpty()) {
        emit bytesReceived(value);
    }
}

void JoyBleClient::onDescriptorWritten(const QLowEnergyDescriptor &d, const QByteArray &value)
{
    Q_UNUSED(d);
    Q_UNUSED(value);
}

void JoyBleClient::writeLine(const QByteArray &line)
{
    if (m_shuttingDown || !m_connected || !m_service || !m_chrRx.isValid()) {
        return;
    }
    m_service->writeCharacteristic(m_chrRx, line, QLowEnergyService::WriteWithoutResponse);
}
