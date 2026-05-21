#include "joystick_setup_widget.h"
#include "joystick_ble_client.h"
#include "shared_joy_state.h"

#include <QBluetoothDeviceInfo>
#include <QRadioButton>

#include <QComboBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QShowEvent>
#include <QHideEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <atomic>

namespace {

static constexpr double kJoyAxisScale = 1100.0;
/** 中立学习采样次数（连接后保持摇杆居中片刻即可完成） */
static constexpr int kLearnSamples = 12;

constexpr int kMaxRxBytes = 8192;
constexpr int kMaxLinesPerChunk = 800;

static const QRegularExpression kJoyLineRe(
    QStringLiteral("^JOY\\s+(-?\\d+)\\s+(-?\\d+)\\s+([0-9a-fA-F]{2})$"));

class StickVisualizer : public QWidget
{
public:
    explicit StickVisualizer(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(260, 260);
        setMaximumSize(400, 400);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setStickNormalized(double nx, double ny)
    {
        m_nx = qBound(-1.0, nx, 1.0);
        m_ny = qBound(-1.0, ny, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRect r = rect().adjusted(4, 4, -4, -4);
        QRadialGradient g(r.center(), r.width() / 2.0);
        g.setColorAt(0.0, QColor(90, 12, 18));
        g.setColorAt(1.0, QColor(28, 4, 8));
        p.fillRect(r, g);

        p.setPen(QPen(QColor(160, 40, 50), 2));
        const QPoint c = r.center();
        p.drawLine(c.x(), r.top(), c.x(), r.bottom());
        p.drawLine(r.left(), c.y(), r.right(), c.y());

        const double rx = (r.width() / 2.0 - 18) * m_nx;
        const double ry = -(r.height() / 2.0 - 18) * m_ny;
        const QPoint pos(qRound(c.x() + rx), qRound(c.y() + ry));

        p.setPen(QPen(QColor(220, 200, 120), 2));
        p.setBrush(QColor(255, 230, 140));
        p.drawEllipse(pos, 14, 14);

        p.setPen(QColor(200, 180, 160));
        p.setFont(QFont(QStringLiteral("Segoe UI"), 9));
        p.drawText(r.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("归一化偏移 dx/k、dy/k"));
    }

private:
    double m_nx = 0.0;
    double m_ny = 0.0;
};

} // namespace

JoystickSetupWidget::JoystickSetupWidget(SharedJoyState *state, QWidget *parent)
    : QWidget(parent)
    , m_state(state)
{
    qRegisterMetaType<QBluetoothDeviceInfo>();
    setFocusPolicy(Qt::StrongFocus);

    m_parseDrainTimer = new QTimer(this);
    m_parseDrainTimer->setSingleShot(true);
    m_parseDrainTimer->setInterval(0);
    connect(m_parseDrainTimer, &QTimer::timeout, this, &JoystickSetupWidget::drainParseContinue);

    m_joyCoalesceTimer = new QTimer(this);
    m_joyCoalesceTimer->setSingleShot(true);
    /* 与 ~120fps 游戏节拍对齐：纯摇杆位移合并间隔略短，减轻高负载帧抖动时的滞后感 */
    m_joyCoalesceTimer->setInterval(8);
    connect(m_joyCoalesceTimer, &QTimer::timeout, this, &JoystickSetupWidget::flushCoalescedJoy);

    auto *root = new QHBoxLayout(this);

    auto *leftCol = new QVBoxLayout();

    auto *linkBox = new QGroupBox(QStringLiteral("连接方式（串口 / 蓝牙二选一）"));
    auto *linkLay = new QVBoxLayout(linkBox);
    auto *modeRow = new QHBoxLayout();
    m_radioSerial = new QRadioButton(QStringLiteral("USB 串口"));
    m_radioBle = new QRadioButton(QStringLiteral("蓝牙 BLE"));
    m_radioSerial->setChecked(true);
    modeRow->addWidget(m_radioSerial);
    modeRow->addWidget(m_radioBle);
    modeRow->addStretch();
    linkLay->addLayout(modeRow);

    m_serialPanel = new QWidget();
    auto *serialLay = new QHBoxLayout(m_serialPanel);
    m_portCombo = new QComboBox();
    m_btnConn = new QPushButton(QStringLiteral("连接"));
    serialLay->addWidget(new QLabel(QStringLiteral("端口:")));
    serialLay->addWidget(m_portCombo, 1);
    serialLay->addWidget(m_btnConn);
    linkLay->addWidget(m_serialPanel);

    m_blePanel = new QWidget();
    auto *bleLay = new QHBoxLayout(m_blePanel);
    m_bleDeviceCombo = new QComboBox();
    m_bleDeviceCombo->setMinimumWidth(180);
    m_btnBleScan = new QPushButton(QStringLiteral("扫描"));
    m_btnBleConn = new QPushButton(QStringLiteral("连接"));
    bleLay->addWidget(new QLabel(QStringLiteral("设备:")));
    bleLay->addWidget(m_bleDeviceCombo, 1);
    bleLay->addWidget(m_btnBleScan);
    bleLay->addWidget(m_btnBleConn);
    m_blePanel->setVisible(false);
    linkLay->addWidget(m_blePanel);

    connect(m_radioSerial, &QRadioButton::toggled, this, &JoystickSetupWidget::onLinkModeChanged);
    connect(m_btnBleScan, &QPushButton::clicked, this, &JoystickSetupWidget::onBleScan);
    connect(m_btnBleConn, &QPushButton::clicked, this, &JoystickSetupWidget::onHostConnectToggle);

    auto *valBox = new QGroupBox(QStringLiteral("JOY 行解析"));
    auto *valLay = new QGridLayout(valBox);
    valLay->addWidget(new QLabel(QStringLiteral("dx / raw X:")), 0, 0);
    m_lblX = new QLabel(QStringLiteral("—"));
    valLay->addWidget(m_lblX, 0, 1);
    valLay->addWidget(new QLabel(QStringLiteral("dy / raw Y:")), 1, 0);
    m_lblY = new QLabel(QStringLiteral("—"));
    valLay->addWidget(m_lblY, 1, 1);
    valLay->addWidget(new QLabel(QStringLiteral("按键掩码:")), 2, 0);
    m_lblMask = new QLabel(QStringLiteral("—"));
    valLay->addWidget(m_lblMask, 2, 1);

    auto *calBox = new QGroupBox(QStringLiteral("摇杆中立"));
    auto *calLay = new QVBoxLayout(calBox);
    m_btnCalibrateStick = new QPushButton(QStringLiteral("摇杆回中后重新校准"));
    m_lblCenterInfo = new QLabel(
        QStringLiteral("连接后自动采样：若数值约 600–3700 则按「原始 ADC」减去中心；接近 0 则视为固件已发偏移。"));
    m_lblCenterInfo->setWordWrap(true);
    calLay->addWidget(m_btnCalibrateStick);
    calLay->addWidget(m_lblCenterInfo);

    const QString btnNames[] = {
        QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"),
        QStringLiteral("D"), QStringLiteral("E"), QStringLiteral("F"), QStringLiteral("K"),
    };
    auto *btnBox = new QGroupBox(QStringLiteral("按键"));
    auto *btnGrid = new QGridLayout(btnBox);
    for (int i = 0; i < 7; ++i) {
        m_btnLed[i] = new QLabel(btnNames[i]);
        m_btnLed[i]->setAlignment(Qt::AlignCenter);
        m_btnLed[i]->setMinimumWidth(36);
        m_btnLed[i]->setStyleSheet(QStringLiteral(
            "QLabel { background:#333; color:#888; border:1px solid #555; border-radius:4px; padding:6px; }"));
        btnGrid->addWidget(m_btnLed[i], i / 4, i % 4);
    }

    m_log = new QPlainTextEdit();
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(140);
    m_log->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#1a1a1a; color:#b0d0b0; font-family:Consolas; font-size:11px; }"));

    auto *joyHint = new QLabel(QStringLiteral(
        "串口与蓝牙同时仅可连接一种；切换方式前请先断开当前连接。"
        "蓝牙设备名：QTgame-Joy。键盘 B / ↑↓：连接与校准；Enter / 摇杆 A：确认。"));
    joyHint->setWordWrap(true);

    leftCol->addWidget(linkBox);
    leftCol->addWidget(valBox);
    leftCol->addWidget(calBox);
    leftCol->addWidget(btnBox);
    leftCol->addWidget(new QLabel(QStringLiteral("原始行:")));
    leftCol->addWidget(m_log);
    leftCol->addWidget(joyHint);

    m_focusButtons[0] = m_btnConn;
    m_focusButtons[1] = m_btnCalibrateStick;
    updateFocusHighlight();

    auto *stick = new StickVisualizer(this);
    m_stick = stick;

    root->addLayout(leftCol, 1);
    root->addWidget(m_stick, 0, Qt::AlignCenter);

    connect(m_btnConn, &QPushButton::clicked, this, &JoystickSetupWidget::onHostConnectToggle);
    connect(m_btnCalibrateStick, &QPushButton::clicked, this, &JoystickSetupWidget::onRecalibrateStick);

    refreshPorts();
    onLinkModeChanged();
    updateLinkUiState();

    m_emitTelemetry = isVisible();
}

JoystickSetupWidget::~JoystickSetupWidget()
{
    m_applicationQuitting = true;
    const bool was = isHostConnected();
    disconnectGraceful();
    if (was) {
        applyHostConnectionChanged(false);
    }
}

void JoystickSetupWidget::prepareForApplicationQuit()
{
    m_applicationQuitting = true;
    if (m_parseDrainTimer) {
        m_parseDrainTimer->stop();
    }
    if (m_joyCoalesceTimer) {
        m_joyCoalesceTimer->stop();
    }
    if (m_ble) {
        QObject::disconnect(m_ble, nullptr, this, nullptr);
        m_ble->prepareForShutdown();
        m_ble->drainPending(3000);
    }
    m_parseDrainScheduled = false;
    m_rxBuf.clear();
    disconnectGraceful();
}

void JoystickSetupWidget::openSerialPort(const QString &portName)
{
    closeSerialPort();

    m_serial = new QSerialPort();
    m_serial->setPortName(portName);
    m_serial->setBaudRate(QSerialPort::Baud115200);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        const QString err = m_serial->errorString();
        delete m_serial;
        m_serial = nullptr;
        applyHostConnectionChanged(false);
        QMessageBox::critical(this, QStringLiteral("串口"),
                              QStringLiteral("无法打开串口：\n%1").arg(err));
        return;
    }

    m_serial->clear(QSerialPort::AllDirections);
    m_rxBuf.clear();
    m_parseDrainScheduled = false;

    connect(m_serial, &QSerialPort::readyRead, this, &JoystickSetupWidget::onSerialReadyRead);
    m_hostLink = HostLinkKind::Serial;
    applyHostConnectionChanged(true);
    writeHostLine(QByteArrayLiteral("MODE CAL\n"));
}

void JoystickSetupWidget::closeSerialPort()
{
    if (m_parseDrainTimer) {
        m_parseDrainTimer->stop();
    }
    resetJoyCoalesceState();
    m_parseDrainScheduled = false;
    m_rxBuf.clear();
    if (m_serial) {
        disconnect(m_serial, nullptr, this, nullptr);
        if (m_serial->isOpen()) {
            m_serial->close();
        }
        delete m_serial;
        m_serial = nullptr;
    }
}

void JoystickSetupWidget::disconnectGraceful()
{
    if (m_hostLink == HostLinkKind::Ble) {
        closeBleConnection();
    }
    if (m_serial && m_serial->isOpen()) {
        m_serial->blockSignals(true);
        disconnect(m_serial, nullptr, this, nullptr);
        m_serial->write(QByteArrayLiteral("MODE SILENT\n"));
        (void)m_serial->waitForBytesWritten(500);
    }
    closeSerialPort();
    m_hostLink = HostLinkKind::None;
}

void JoystickSetupWidget::resetJoyCoalesceState()
{
    if (m_joyCoalesceTimer) {
        m_joyCoalesceTimer->stop();
    }
    m_joyMaskInitialized = false;
    m_lastEmittedMask = 0;
}

void JoystickSetupWidget::pushJoySample(int vx, int vy, uint8_t mask)
{
    m_pendingVx = vx;
    m_pendingVy = vy;
    m_pendingMask = mask;

    const bool maskChanged = m_joyMaskInitialized && (mask != m_lastEmittedMask);
    if (!m_joyMaskInitialized) {
        m_joyMaskInitialized = true;
        m_lastEmittedMask = mask;
        applyParsedJoy(vx, vy, mask);
        return;
    }

    if (maskChanged) {
        m_lastEmittedMask = mask;
        m_joyCoalesceTimer->stop();
        applyParsedJoy(vx, vy, mask);
        return;
    }

    if (!m_joyCoalesceTimer->isActive()) {
        m_joyCoalesceTimer->start();
    }
}

void JoystickSetupWidget::flushCoalescedJoy()
{
    if (m_applicationQuitting) {
        return;
    }
    applyParsedJoy(m_pendingVx, m_pendingVy, m_pendingMask);
}

void JoystickSetupWidget::trimRxOverflow()
{
    while (m_rxBuf.size() > kMaxRxBytes) {
        const int nl = m_rxBuf.indexOf('\n');
        if (nl >= 0) {
            m_rxBuf.remove(0, nl + 1);
        } else {
            m_rxBuf.remove(0, m_rxBuf.size() - kMaxRxBytes / 2);
        }
    }
}

void JoystickSetupWidget::scheduleParseDrain()
{
    if (m_parseDrainScheduled) {
        return;
    }
    m_parseDrainScheduled = true;
    if (m_parseDrainTimer && !m_parseDrainTimer->isActive()) {
        m_parseDrainTimer->start();
    }
}

void JoystickSetupWidget::drainParseContinue()
{
    m_parseDrainScheduled = false;
    if (m_applicationQuitting || m_rxBuf.isEmpty()) {
        return;
    }
    tryParseLines();
}

void JoystickSetupWidget::onSerialReadyRead()
{
    if (m_applicationQuitting || !m_serial || m_hostLink != HostLinkKind::Serial) {
        return;
    }
    appendHostRx(m_serial->readAll());
}

void JoystickSetupWidget::onBleReadyRead(const QByteArray &data)
{
    if (m_applicationQuitting || m_hostLink != HostLinkKind::Ble) {
        return;
    }
    appendHostRx(data);
}

void JoystickSetupWidget::tryParseLines()
{
    if (m_applicationQuitting) {
        return;
    }

    int budget = kMaxLinesPerChunk;

    while (budget-- > 0) {
        const int nl = m_rxBuf.indexOf('\n');
        if (nl < 0) {
            break;
        }
        if (nl > 2048) {
            m_rxBuf.remove(0, nl + 1);
            continue;
        }
        QByteArray line = m_rxBuf.left(nl);
        m_rxBuf.remove(0, nl + 1);
        if (!line.isEmpty() && line.endsWith('\r')) {
            line.chop(1);
        }

        const QString s = QString::fromUtf8(line).trimmed();
        if (m_emitTelemetry && !s.isEmpty() && m_log) {
            if (isVisible()) {
                if (m_log->document()->blockCount() > 40) {
                    m_log->clear();
                }
                m_log->appendPlainText(s);
                m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
            }
        }

        const QRegularExpressionMatch m = kJoyLineRe.match(s);
        if (m.hasMatch()) {
            const int vx = m.captured(1).toInt();
            const int vy = m.captured(2).toInt();
            bool ok = false;
            const auto mask = static_cast<uint8_t>(m.captured(3).toUInt(&ok, 16));
            if (ok) {
                pushJoySample(vx, vy, mask);
            }
        }
    }

    trimRxOverflow();
    if (m_rxBuf.indexOf('\n') >= 0) {
        scheduleParseDrain();
    }
}

void JoystickSetupWidget::ensureBleClient()
{
    if (m_ble) {
        return;
    }
    m_ble = new JoyBleClient(this);
    connect(m_ble, &JoyBleClient::deviceDiscovered, this, &JoystickSetupWidget::onBleDeviceDiscovered);
    connect(m_ble, &JoyBleClient::discoveryFinished, this, &JoystickSetupWidget::onBleDiscoveryFinished);
    connect(m_ble, &JoyBleClient::connected, this, &JoystickSetupWidget::onBleConnected);
    connect(m_ble, &JoyBleClient::disconnected, this, &JoystickSetupWidget::onBleDisconnected);
    connect(m_ble, &JoyBleClient::errorOccurred, this, &JoystickSetupWidget::onBleError);
    connect(m_ble, &JoyBleClient::bytesReceived, this, &JoystickSetupWidget::onBleReadyRead);
}

bool JoystickSetupWidget::isHostConnected() const
{
    return m_hostLink != HostLinkKind::None;
}

void JoystickSetupWidget::writeHostLine(const QByteArray &line)
{
    if (m_hostLink == HostLinkKind::Serial && m_serial && m_serial->isOpen()) {
        m_serial->write(line);
        (void)m_serial->waitForBytesWritten(500);
    } else if (m_hostLink == HostLinkKind::Ble && m_ble) {
        m_ble->writeLine(line);
    }
}

void JoystickSetupWidget::appendHostRx(const QByteArray &data)
{
    if (data.isEmpty()) {
        return;
    }
    m_rxBuf.append(data);
    trimRxOverflow();
    tryParseLines();
}

void JoystickSetupWidget::updateLinkUiState()
{
    const bool connected = isHostConnected();
    const bool serialMode = m_radioSerial && m_radioSerial->isChecked();

    if (m_radioSerial) {
        m_radioSerial->setEnabled(!connected);
    }
    if (m_radioBle) {
        m_radioBle->setEnabled(!connected);
    }
    if (m_serialPanel) {
        m_serialPanel->setEnabled(!connected || serialMode);
    }
    if (m_blePanel) {
        m_blePanel->setEnabled(!connected || !serialMode);
    }
    if (m_portCombo && m_btnConn) {
        m_portCombo->setEnabled(!connected && serialMode);
        m_btnConn->setEnabled(serialMode);
    }
    if (m_bleDeviceCombo && m_btnBleScan && m_btnBleConn) {
        const bool bleMode = !serialMode;
        m_bleDeviceCombo->setEnabled(!connected && bleMode);
        m_btnBleScan->setEnabled(!connected && bleMode);
        m_btnBleConn->setEnabled(bleMode);
        m_btnBleConn->setText(connected && m_hostLink == HostLinkKind::Ble
                                  ? QStringLiteral("断开")
                                  : QStringLiteral("连接"));
    }
    if (m_btnConn) {
        m_btnConn->setText(connected && m_hostLink == HostLinkKind::Serial
                               ? QStringLiteral("断开")
                               : QStringLiteral("连接"));
    }
    if (m_btnCalibrateStick) {
        m_btnCalibrateStick->setEnabled(connected);
    }
}

void JoystickSetupWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_emitTelemetry = true;
}

void JoystickSetupWidget::hideEvent(QHideEvent *event)
{
    m_emitTelemetry = false;
    QWidget::hideEvent(event);
}

void JoystickSetupWidget::applyHostConnectionChanged(bool connected)
{
    if (connected) {
        resetStickCenterLearning();
        if (m_state) {
            m_state->serialConnected.store(true, std::memory_order_relaxed);
        }
        QTimer::singleShot(100, this, [this]() {
            if (m_applicationQuitting || !isHostConnected()
                || m_hostLink != HostLinkKind::Serial) {
                return;
            }
            writeHostLine(QByteArrayLiteral("MODE CAL\n"));
        });
    } else {
        m_hostLink = HostLinkKind::None;
        resetStickCenterLearning();
        if (m_lblCenterInfo) {
            m_lblCenterInfo->setText(QStringLiteral("已断开。请选择连接方式后重新连接。"));
        }
        m_prevNavMask = 0;
        m_focusButtonIndex = 0;
        updateFocusHighlight();
        if (m_state) {
            m_state->serialConnected.store(false, std::memory_order_relaxed);
            m_state->nx.store(0.f, std::memory_order_relaxed);
            m_state->ny.store(0.f, std::memory_order_relaxed);
            m_state->buttonMask.store(0, std::memory_order_relaxed);
        }
        m_joyDedupeValid = false;
        m_rxBuf.clear();
    }
    updateLinkUiState();
}

/** 游戏页与准备页均使用按需低帧率 MODE GAME；仅「采样学习 / 重新校准」时用 MODE CAL */
void JoystickSetupWidget::setEspUartGameStream(bool gameActive)
{
    Q_UNUSED(gameActive);
    writeHostLine(QByteArrayLiteral("MODE GAME\n"));
}

void JoystickSetupWidget::cycleSetupFocus()
{
    m_focusButtonIndex = (m_focusButtonIndex + 1) % 2;
    updateFocusHighlight();
}

void JoystickSetupWidget::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_B:
        cycleSetupFocus();
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_focusButtons[m_focusButtonIndex]) {
            m_focusButtons[m_focusButtonIndex]->click();
        }
        event->accept();
        return;
    case Qt::Key_Up:
    case Qt::Key_Down:
        cycleSetupFocus();
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void JoystickSetupWidget::updateFocusHighlight()
{
    const QString base = QStringLiteral("QPushButton { padding: 6px; }");
    const QString hi =
        QStringLiteral("QPushButton { padding: 6px; border: 2px solid #e6c200; background: #353025; }");
    for (int i = 0; i < 2; ++i) {
        if (!m_focusButtons[i]) {
            continue;
        }
        m_focusButtons[i]->setStyleSheet((i == m_focusButtonIndex) ? hi : base);
    }
}

void JoystickSetupWidget::handleSetupJoyEdges(quint32 mask)
{
    if (!m_state || !m_state->serialConnected.load(std::memory_order_relaxed)) {
        m_prevNavMask = mask;
        return;
    }

    const quint32 rising = mask & ~m_prevNavMask;
    m_prevNavMask = mask;
    if (rising == 0) {
        return;
    }

    if (rising & JoyMask::B) {
        cycleSetupFocus();
    }
    if (rising & JoyMask::A) {
        if (m_focusButtons[m_focusButtonIndex]) {
            m_focusButtons[m_focusButtonIndex]->click();
        }
    }
}

void JoystickSetupWidget::resetStickCenterLearning()
{
    m_centerReady = false;
    m_learnN = 0;
    m_learnSumX = 0;
    m_learnSumY = 0;
    m_centerX = 0;
    m_centerY = 0;
    m_joyDedupeValid = false;
    if (m_lblCenterInfo) {
        m_lblCenterInfo->setText(QStringLiteral("采样中…请保持摇杆居中"));
    }
}

void JoystickSetupWidget::onRecalibrateStick()
{
    if (!isHostConnected()) {
        QMessageBox::information(this, QStringLiteral("校准"),
                                 QStringLiteral("请先连接串口或蓝牙。"));
        return;
    }
    resetStickCenterLearning();
    writeHostLine(QByteArrayLiteral("MODE CAL\n"));
}

void JoystickSetupWidget::refreshPorts()
{
    m_portCombo->clear();
    int prefer = -1;
    const auto ports = QSerialPortInfo::availablePorts();
    for (int i = 0; i < ports.size(); ++i) {
        const QSerialPortInfo &info = ports.at(i);
        QString label = info.portName();
        if (!info.description().isEmpty()) {
            label += QStringLiteral(" — ") + info.description();
        }
        m_portCombo->addItem(label, info.portName());
        if (info.portName().compare(QStringLiteral("COM7"), Qt::CaseInsensitive) == 0) {
            prefer = i;
        }
    }
    if (prefer >= 0) {
        m_portCombo->setCurrentIndex(prefer);
    }
}

void JoystickSetupWidget::onLinkModeChanged()
{
    if (isHostConnected()) {
        return;
    }
    const bool serial = m_radioSerial && m_radioSerial->isChecked();
    if (m_serialPanel) {
        m_serialPanel->setVisible(serial);
    }
    if (m_blePanel) {
        m_blePanel->setVisible(!serial);
    }
    updateLinkUiState();
}

void JoystickSetupWidget::onHostConnectToggle()
{
    if (m_radioBle && m_radioBle->isChecked()) {
        if (m_hostLink == HostLinkKind::Ble) {
            disconnectGraceful();
            applyHostConnectionChanged(false);
            return;
        }
        if (isHostConnected()) {
            QMessageBox::information(this, QStringLiteral("连接"),
                                     QStringLiteral("请先断开当前连接，再切换方式。"));
            return;
        }
        openBleConnection();
        return;
    }

    if (m_hostLink == HostLinkKind::Serial) {
        disconnectGraceful();
        applyHostConnectionChanged(false);
        return;
    }
    if (isHostConnected()) {
        QMessageBox::information(this, QStringLiteral("连接"),
                                 QStringLiteral("请先断开当前连接，再切换方式。"));
        return;
    }
    if (m_portCombo->count() == 0) {
        QMessageBox::warning(this, QStringLiteral("串口"),
                             QStringLiteral("未发现串口设备。"));
        return;
    }
    openSerialPort(m_portCombo->currentData().toString());
}

void JoystickSetupWidget::onBleScan()
{
    if (isHostConnected()) {
        return;
    }
    ensureBleClient();
    m_bleDeviceCombo->clear();
    m_ble->startDiscovery();
    m_btnBleScan->setEnabled(false);
    m_btnBleScan->setText(QStringLiteral("扫描中…"));
}

void JoystickSetupWidget::onBleDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
    if (!info.name().contains(QStringLiteral("QTgame"), Qt::CaseInsensitive)) {
        return;
    }
    const QString label =
        info.name().isEmpty()
            ? info.address().toString()
            : QStringLiteral("%1 (%2)").arg(info.name(), info.address().toString());
    m_bleDeviceCombo->addItem(label, QVariant::fromValue(info));
}

void JoystickSetupWidget::onBleDiscoveryFinished()
{
    m_btnBleScan->setEnabled(true);
    m_btnBleScan->setText(QStringLiteral("扫描"));
    if (m_bleDeviceCombo->count() == 0) {
        QMessageBox::information(this, QStringLiteral("蓝牙"),
                                 QStringLiteral("未发现 QTgame-Joy，请确认固件已上电并在广播。"));
    }
}

void JoystickSetupWidget::onBleConnected()
{
    m_hostLink = HostLinkKind::Ble;
    applyHostConnectionChanged(true);
}

void JoystickSetupWidget::onBleDisconnected()
{
    if (m_hostLink == HostLinkKind::Ble) {
        applyHostConnectionChanged(false);
    }
}

void JoystickSetupWidget::onBleError(const QString &msg)
{
    m_btnBleScan->setEnabled(true);
    m_btnBleScan->setText(QStringLiteral("扫描"));
    QMessageBox::warning(this, QStringLiteral("蓝牙"), msg);
}

void JoystickSetupWidget::openBleConnection()
{
    ensureBleClient();
    if (m_bleDeviceCombo->count() <= 0) {
        QMessageBox::warning(this, QStringLiteral("蓝牙"),
                             QStringLiteral("请先扫描并选择设备。"));
        return;
    }
    const QVariant v = m_bleDeviceCombo->currentData();
    if (!v.canConvert<QBluetoothDeviceInfo>()) {
        return;
    }
    m_ble->connectToDevice(v.value<QBluetoothDeviceInfo>());
}

void JoystickSetupWidget::closeBleConnection()
{
    if (!m_ble) {
        return;
    }
    writeHostLine(QByteArrayLiteral("MODE SILENT\n"));
    m_ble->disconnectDevice();
}

void JoystickSetupWidget::applyParsedJoy(int vx, int vy, uint8_t mask)
{
    if (!m_state) {
        return;
    }

    if (m_centerReady && m_joyDedupeValid && vx == m_joyDedupeVx && vy == m_joyDedupeVy
        && mask == m_joyDedupeMask) {
        return;
    }

    const bool showUi = isVisible();

    const auto publishAxesMask = [&](float nx, float ny) {
        m_state->buttonMask.store(static_cast<quint32>(mask), std::memory_order_relaxed);
        m_state->nx.store(nx, std::memory_order_relaxed);
        m_state->ny.store(ny, std::memory_order_relaxed);
        m_joyDedupeVx = vx;
        m_joyDedupeVy = vy;
        m_joyDedupeMask = mask;
        m_joyDedupeValid = true;
    };

    if (vx == -1 && vy == -1) {
        publishAxesMask(0.f, 0.f);
        m_joyDedupeValid = false;
        if (!showUi) {
            return;
        }
        m_lblMask->setText(QStringLiteral("0x%1").arg(mask, 2, 16, QChar('0')));
        m_lblX->setText(QStringLiteral("ERR"));
        m_lblY->setText(QStringLiteral("ERR"));
        static_cast<StickVisualizer *>(m_stick)->setStickNormalized(0, 0);
        for (int i = 0; i < 7; ++i) {
            m_btnLed[i]->setStyleSheet(QStringLiteral(
                "QLabel { background:#333; color:#888; border:1px solid #555; border-radius:4px; padding:6px; }"));
        }
        handleSetupJoyEdges(static_cast<quint32>(mask));
        return;
    }

    if (showUi) {
        m_lblMask->setText(QStringLiteral("0x%1").arg(mask, 2, 16, QChar('0')));
        m_lblX->setText(QString::number(vx));
        m_lblY->setText(QString::number(vy));
    }

    if (!m_centerReady) {
        m_learnSumX += vx;
        m_learnSumY += vy;
        m_learnN++;
        if (m_learnN >= kLearnSamples) {
            const int ax = static_cast<int>(m_learnSumX / m_learnN);
            const int ay = static_cast<int>(m_learnSumY / m_learnN);
            if (ax >= 600 && ax <= 3700 && ay >= 600 && ay <= 3700) {
                m_centerX = ax;
                m_centerY = ay;
                if (showUi && m_lblCenterInfo) {
                    m_lblCenterInfo->setText(
                        QStringLiteral("已按原始 ADC 减中心 (%1 , %2)").arg(m_centerX).arg(m_centerY));
                }
            } else {
                m_centerX = 0;
                m_centerY = 0;
                if (showUi && m_lblCenterInfo) {
                    m_lblCenterInfo->setText(QStringLiteral("按偏移量协议（中立≈0），不减中心"));
                }
            }
            m_centerReady = true;
            /* 校准结束后改为按需低帧率上报，减轻串口与上位机负载 */
            writeHostLine(QByteArrayLiteral("MODE GAME\n"));
            if (showUi && m_lblCenterInfo) {
                m_lblCenterInfo->setText(
                    m_lblCenterInfo->text()
                    + QStringLiteral("　·　已切换按需上报（仅按键/摇杆变化时发帧）"));
            }
        }
    }

    // 学习未完成：用当前采样均值作临时中心输出
    if (!m_centerReady) {
        const int cx = (m_learnN > 0) ? static_cast<int>(m_learnSumX / m_learnN) : vx;
        const int cy = (m_learnN > 0) ? static_cast<int>(m_learnSumY / m_learnN) : vy;
        const int pdx = vx - cx;
        const int pdy = vy - cy;
        const double pnx = qBound(-1.0, pdx / kJoyAxisScale, 1.0);
        const double pny = qBound(-1.0, pdy / kJoyAxisScale, 1.0);
        publishAxesMask(static_cast<float>(pnx), static_cast<float>(pny));
        if (!showUi) {
            return;
        }
        static_cast<StickVisualizer *>(m_stick)->setStickNormalized(pnx, pny);
        for (int i = 0; i < 7; ++i) {
            const bool on = (mask & (1u << i)) != 0;
            m_btnLed[i]->setStyleSheet(on
                                           ? QStringLiteral(
                                                 "QLabel { background:#2a6a2a; color:#efe; "
                                                 "border:1px solid #4a9; border-radius:4px; padding:6px; }")
                                           : QStringLiteral(
                                                 "QLabel { background:#333; color:#888; "
                                                 "border:1px solid #555; border-radius:4px; padding:6px; }"));
        }
        handleSetupJoyEdges(static_cast<quint32>(mask));
        return;
    }

    const int dx = vx - m_centerX;
    const int dy = vy - m_centerY;
    if (showUi) {
        m_lblX->setText(QStringLiteral("%1 — Δ%2").arg(vx).arg(dx));
        m_lblY->setText(QStringLiteral("%1 — Δ%2").arg(vy).arg(dy));
    }

    const double nx = qBound(-1.0, dx / kJoyAxisScale, 1.0);
    const double ny = qBound(-1.0, dy / kJoyAxisScale, 1.0);

    publishAxesMask(static_cast<float>(nx), static_cast<float>(ny));

    if (!showUi) {
        return;
    }

    static_cast<StickVisualizer *>(m_stick)->setStickNormalized(nx, ny);

    for (int i = 0; i < 7; ++i) {
        const bool on = (mask & (1u << i)) != 0;
        m_btnLed[i]->setStyleSheet(on
                                       ? QStringLiteral("QLabel { background:#2a6a2a; color:#efe; "
                                                        "border:1px solid #4a9; border-radius:4px; padding:6px; }")
                                       : QStringLiteral("QLabel { background:#333; color:#888; "
                                                        "border:1px solid #555; border-radius:4px; padding:6px; }"));
    }

    handleSetupJoyEdges(static_cast<quint32>(mask));
}
