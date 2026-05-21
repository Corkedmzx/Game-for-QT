#ifndef JOYSTICK_SETUP_WIDGET_H
#define JOYSTICK_SETUP_WIDGET_H

#include <QBluetoothDeviceInfo>
#include <QByteArray>
#include <QtGlobal>
#include <QWidget>
#include <cstdint>

struct SharedJoyState;
class JoyBleClient;
class QComboBox;
class QRadioButton;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QWidget;
class QKeyEvent;
class QShowEvent;
class QHideEvent;
class QSerialPort;
class QTimer;

class JoystickSetupWidget : public QWidget
{
    Q_OBJECT

public:
    explicit JoystickSetupWidget(SharedJoyState *state, QWidget *parent = nullptr);
    ~JoystickSetupWidget() override;

    /** true=游戏页 MODE GAME；false=回准备页亦用 MODE GAME（低流量）；校准中由本类发 MODE CAL */
    void setEspUartGameStream(bool gameActive);

    /** 进程退出前调用：停解析定时器、静默关闭串口，避免关闭窗口时 readyRead 与析构竞态导致堆损坏 */
    void prepareForApplicationQuit();

private slots:
    void onHostConnectToggle();
    void onLinkModeChanged();
    void refreshPorts();
    void onRecalibrateStick();
    void onBleScan();
    void onBleDeviceDiscovered(const QBluetoothDeviceInfo &info);
    void onBleDiscoveryFinished();
    void onBleConnected();
    void onBleDisconnected();
    void onBleError(const QString &msg);
    void onBleReadyRead(const QByteArray &data);

    void onSerialReadyRead();
    void drainParseContinue();
    void flushCoalescedJoy();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void applyParsedJoy(int vx, int vy, uint8_t mask);
    void resetStickCenterLearning();
    void handleSetupJoyEdges(quint32 mask);
    void cycleSetupFocus();
    void updateFocusHighlight();
    void writeHostLine(const QByteArray &line);
    void appendHostRx(const QByteArray &data);
    bool isHostConnected() const;
    void updateLinkUiState();

    void openSerialPort(const QString &portName);
    void closeSerialPort();
    void openBleConnection();
    void closeBleConnection();
    void disconnectGraceful();
    void trimRxOverflow();
    void tryParseLines();
    void scheduleParseDrain();
    void pushJoySample(int vx, int vy, uint8_t mask);
    void resetJoyCoalesceState();
    void applyHostConnectionChanged(bool connected);
    void ensureBleClient();

    enum class HostLinkKind { None, Serial, Ble };

    SharedJoyState *m_state = nullptr;

    HostLinkKind m_hostLink = HostLinkKind::None;
    JoyBleClient *m_ble = nullptr;

    QSerialPort *m_serial = nullptr;
    QByteArray m_rxBuf;
    bool m_emitTelemetry = false;
    bool m_parseDrainScheduled = false;
    QTimer *m_parseDrainTimer = nullptr;
    QTimer *m_joyCoalesceTimer = nullptr;
    int m_pendingVx = 0;
    int m_pendingVy = 0;
    quint8 m_pendingMask = 0;
    quint8 m_lastEmittedMask = 0;
    bool m_joyMaskInitialized = false;

    bool m_applicationQuitting = false;

    QRadioButton *m_radioSerial = nullptr;
    QRadioButton *m_radioBle = nullptr;
    QWidget *m_serialPanel = nullptr;
    QWidget *m_blePanel = nullptr;
    QComboBox *m_bleDeviceCombo = nullptr;
    QPushButton *m_btnBleScan = nullptr;

    QPushButton *m_btnCalibrateStick = nullptr;
    QLabel *m_lblCenterInfo = nullptr;

    int m_centerX = 0;
    int m_centerY = 0;
    bool m_centerReady = false;
    int m_learnN = 0;
    long long m_learnSumX = 0;
    long long m_learnSumY = 0;

    QComboBox *m_portCombo = nullptr;
    QPushButton *m_btnConn = nullptr; /**< 串口连接/断开 */
    QPushButton *m_btnBleConn = nullptr;
    QLabel *m_lblX = nullptr;
    QLabel *m_lblY = nullptr;
    QLabel *m_lblMask = nullptr;
    QLabel *m_btnLed[7] = {};
    QWidget *m_stick = nullptr;
    QPlainTextEdit *m_log = nullptr;

    /** 摇杆 B：在「连接 / 校准」之间切换焦点；连接亦可鼠标点击 */
    int m_focusButtonIndex = 0;
    QPushButton *m_focusButtons[2] = {nullptr, nullptr};
    quint32 m_prevNavMask = 0;

    /** 校准完成后丢弃完全重复的 JOY 包（减轻游戏中 UI 隐藏时的 CPU 压力） */
    int m_joyDedupeVx = 0;
    int m_joyDedupeVy = 0;
    uint8_t m_joyDedupeMask = 0;
    bool m_joyDedupeValid = false;
};

#endif
