#include "MainWindow.h"
#include "game_2048_window.h"
#include "xiangqi_window.h"
#include "chess_window.h"
#include "joystick_setup_widget.h"
#include "shared_joy_state.h"
#include "tetris_window.h"
#include "thunder_fighter_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFont>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {

const QString kLauncherCardStyleNormal = QStringLiteral(
    "QPushButton { font-size:15px; padding:12px 24px; text-align:left; }"
    "QPushButton:hover { background:#2a3f5f; }");
const QString kLauncherCardStyleSelected = QStringLiteral(
    "QPushButton { font-size:15px; padding:12px 24px; text-align:left; "
    "background:#354a70; border:2px solid #5dacff; }"
    "QPushButton:hover { background:#3d5575; }");

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("QTgame"));
    resize(900, 680);

    m_setup = new JoystickSetupWidget(&m_joyState);

    m_game = new ThunderFighterWindow(&m_joyState);
    m_game2048 = new Game2048Window(&m_joyState);

    // —— 「游戏」标签：项目展示区 + 堆叠的游戏画布 ——
    auto *gameTab = new QWidget();
    auto *gameTabLay = new QVBoxLayout(gameTab);
    gameTabLay->setContentsMargins(16, 16, 16, 16);

    m_gameStack = new QStackedWidget();

    auto *launcherPage = new QWidget();
    auto *launcherLay = new QVBoxLayout(launcherPage);
    launcherLay->setSpacing(20);

    auto *title = new QLabel(QStringLiteral("游戏项目"));
    {
        QFont f = title->font();
        f.setPointSize(18);
        f.setBold(true);
        title->setFont(f);
    }
    launcherLay->addWidget(title);

    auto *sub = new QLabel(QStringLiteral("点击下方卡片进入对应游戏。游玩中菜单与雷霆战机相同：ESC / P 暂停，摇杆 C/D 暂停，摇杆 D 在菜单/结束页返回本页。"));
    sub->setWordWrap(true);
    sub->setStyleSheet(QStringLiteral("color:#bbb;"));
    launcherLay->addWidget(sub);

    m_btnThunder = new QPushButton(QStringLiteral("雷霆战机"));
    m_btnThunder->setMinimumHeight(52);
    launcherLay->addWidget(m_btnThunder);

    m_btnTetris = new QPushButton(QStringLiteral("俄罗斯方块"));
    m_btnTetris->setMinimumHeight(52);
    launcherLay->addWidget(m_btnTetris);

    m_btn2048 = new QPushButton(QStringLiteral("2048"));
    m_btn2048->setMinimumHeight(52);
    launcherLay->addWidget(m_btn2048);

    m_btnXiangqi = new QPushButton(QStringLiteral("中国象棋"));
    m_btnXiangqi->setMinimumHeight(52);
    launcherLay->addWidget(m_btnXiangqi);

    m_btnChess = new QPushButton(QStringLiteral("国际象棋"));
    m_btnChess->setMinimumHeight(52);
    launcherLay->addWidget(m_btnChess);

    m_launcherGameIndex = 0;
    updateLauncherGameVisual();

    launcherLay->addStretch();

    auto *wrapThunder = new QWidget();
    {
        auto *gv = new QVBoxLayout(wrapThunder);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->addStretch();
        gv->addWidget(m_game, 0, Qt::AlignHCenter);
        gv->addStretch();
    }
    m_wrapTetris = new QWidget();
    m_tetrisVBox = new QVBoxLayout(m_wrapTetris);
    m_tetrisVBox->setContentsMargins(0, 0, 0, 0);
    m_tetrisVBox->addStretch();
    m_tetrisVBox->addStretch();
    auto *wrap2048 = new QWidget();
    {
        auto *gv = new QVBoxLayout(wrap2048);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->addStretch();
        gv->addWidget(m_game2048, 0, Qt::AlignHCenter);
        gv->addStretch();
    }
    m_wrapXiangqi = new QWidget();
    {
        auto *gv = new QVBoxLayout(m_wrapXiangqi);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->addStretch();
        gv->addStretch();
    }
    m_wrapChess = new QWidget();
    {
        auto *gv = new QVBoxLayout(m_wrapChess);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->addStretch();
        gv->addStretch();
    }

    m_gameStack->addWidget(launcherPage);
    m_gameStack->addWidget(wrapThunder);
    m_gameStack->addWidget(m_wrapTetris);
    m_gameStack->addWidget(wrap2048);
    m_gameStack->addWidget(m_wrapXiangqi);
    m_gameStack->addWidget(m_wrapChess);

    gameTabLay->addWidget(m_gameStack);

    m_tabs = new QTabWidget();
    m_tabs->addTab(gameTab, QStringLiteral("游戏"));
    m_tabs->addTab(m_setup, QStringLiteral("摇杆"));

    setCentralWidget(m_tabs);

    connect(m_btnThunder, &QPushButton::clicked, this, &MainWindow::enterThunderGame);
    connect(m_btnTetris, &QPushButton::clicked, this, &MainWindow::enterTetrisGame);
    connect(m_btn2048, &QPushButton::clicked, this, &MainWindow::enterGame2048);
    connect(m_btnXiangqi, &QPushButton::clicked, this, &MainWindow::enterXiangqiGame);
    connect(m_btnChess, &QPushButton::clicked, this, &MainWindow::enterChessGame);
    connect(m_game, &ThunderFighterWindow::requestReturnToSetup, this, &MainWindow::onReturnToLauncher);
    connect(m_game2048, &Game2048Window::requestReturnToSetup, this, &MainWindow::onReturnToLauncher);

    m_joyPollTimer = new QTimer(this);
    connect(m_joyPollTimer, &QTimer::timeout, this, &MainWindow::pollLauncherJoyButtons);
    m_joyPollTimer->start(8);

    QTimer::singleShot(0, this, [this]() {
        if (m_teardownDone) {
            return;
        }
        if (m_btnThunder) {
            m_btnThunder->setFocus(Qt::OtherFocusReason);
        }
    });
}

MainWindow::~MainWindow()
{
    /* 正常路径应在 main/closeEvent 已调用 shutdownGameTimers；此处仅兜底 */
    if (!m_teardownDone) {
        teardownBeforeJoyStateDestroyed();
    }
}

void MainWindow::enterThunderGame()
{
    if (!m_game || !m_gameStack || !m_tabs) {
        return;
    }
    m_gameStack->setCurrentIndex(1);
    m_tabs->setCurrentIndex(0);
    m_setup->setEspUartGameStream(true);
    m_game->scheduleJoyEdgeResync();
    m_game->setFocus(Qt::OtherFocusReason);
    syncMainJoyButtonEdges();
}

void MainWindow::ensureTetrisWindow()
{
    if (m_tetris || !m_tetrisVBox) {
        return;
    }
    m_tetris = new TetrisWindow(&m_joyState);
    connect(m_tetris, &TetrisWindow::requestReturnToSetup, this, &MainWindow::onReturnToLauncher);
    m_tetrisVBox->insertWidget(1, m_tetris, 0, Qt::AlignHCenter);
}

void MainWindow::enterTetrisGame()
{
    ensureTetrisWindow();
    if (!m_tetris || !m_gameStack || !m_tabs) {
        return;
    }
    m_gameStack->setCurrentIndex(2);
    m_tabs->setCurrentIndex(0);
    m_setup->setEspUartGameStream(true);
    m_tetris->scheduleJoyEdgeResync();
    m_tetris->setFocus(Qt::OtherFocusReason);
    syncMainJoyButtonEdges();
}

void MainWindow::enterGame2048()
{
    if (!m_game2048 || !m_gameStack || !m_tabs) {
        return;
    }
    m_gameStack->setCurrentIndex(3);
    m_tabs->setCurrentIndex(0);
    m_setup->setEspUartGameStream(true);
    m_game2048->scheduleJoyEdgeResync();
    m_game2048->setFocus(Qt::OtherFocusReason);
    syncMainJoyButtonEdges();
}

void MainWindow::ensureXiangqiWindow()
{
    if (m_xiangqi || !m_wrapXiangqi) {
        return;
    }
    m_xiangqi = new XiangqiWindow(&m_joyState);
    connect(m_xiangqi, &XiangqiWindow::requestReturnToSetup, this, &MainWindow::onReturnToLauncher);
    if (auto *lay = qobject_cast<QVBoxLayout *>(m_wrapXiangqi->layout())) {
        lay->insertWidget(1, m_xiangqi, 0, Qt::AlignHCenter);
    }
}

void MainWindow::enterXiangqiGame()
{
    ensureXiangqiWindow();
    if (!m_xiangqi || !m_gameStack || !m_tabs) {
        return;
    }
    m_gameStack->setCurrentIndex(4);
    m_tabs->setCurrentIndex(0);
    m_setup->setEspUartGameStream(true);
    m_xiangqi->scheduleJoyEdgeResync();
    m_xiangqi->setFocus(Qt::OtherFocusReason);
    syncMainJoyButtonEdges();
}

void MainWindow::ensureChessWindow()
{
    if (m_chess || !m_wrapChess) {
        return;
    }
    m_chess = new ChessWindow(&m_joyState);
    connect(m_chess, &ChessWindow::requestReturnToSetup, this, &MainWindow::onReturnToLauncher);
    if (auto *lay = qobject_cast<QVBoxLayout *>(m_wrapChess->layout())) {
        lay->insertWidget(1, m_chess, 0, Qt::AlignHCenter);
    }
}

void MainWindow::enterChessGame()
{
    ensureChessWindow();
    if (!m_chess || !m_gameStack || !m_tabs) {
        return;
    }
    m_gameStack->setCurrentIndex(5);
    m_tabs->setCurrentIndex(0);
    m_setup->setEspUartGameStream(true);
    m_chess->scheduleJoyEdgeResync();
    m_chess->setFocus(Qt::OtherFocusReason);
    syncMainJoyButtonEdges();
}

void MainWindow::onReturnToLauncher()
{
    if (m_teardownDone || !m_gameStack || !m_tabs) {
        return;
    }
    if (m_game) {
        m_game->returnToLauncher();
    }
    if (m_tetris) {
        m_tetris->returnToLauncher();
    }
    if (m_game2048) {
        m_game2048->returnToLauncher();
    }
    if (m_xiangqi) {
        m_xiangqi->returnToLauncher();
        m_xiangqi->scheduleJoyEdgeResync();
    }
    if (m_chess) {
        m_chess->returnToLauncher();
        m_chess->scheduleJoyEdgeResync();
    }
    m_setup->setEspUartGameStream(false);
    m_gameStack->setCurrentIndex(0);
    m_tabs->setCurrentIndex(0);
    if (QPushButton *b = launcherGameButton(m_launcherGameIndex)) {
        b->setFocus(Qt::OtherFocusReason);
    } else if (m_btnThunder) {
        m_btnThunder->setFocus(Qt::OtherFocusReason);
    }
    syncMainJoyButtonEdges();
}

void MainWindow::syncMainJoyButtonEdges()
{
    if (!m_joyState.serialConnected.load(std::memory_order_relaxed)) {
        m_prevJoyMaskMain = 0;
        return;
    }
    m_prevJoyMaskMain = m_joyState.buttonMask.load(std::memory_order_relaxed);
}

QPushButton *MainWindow::launcherGameButton(int index) const
{
    switch (index) {
    case 0:
        return m_btnThunder;
    case 1:
        return m_btnTetris;
    case 2:
        return m_btn2048;
    case 3:
        return m_btnXiangqi;
    case 4:
        return m_btnChess;
    default:
        return nullptr;
    }
}

void MainWindow::updateLauncherGameVisual()
{
    if (m_teardownDone) {
        return;
    }
    m_launcherGameIndex = qBound(0, m_launcherGameIndex, kLauncherGameCount - 1);
    for (int i = 0; i < kLauncherGameCount; ++i) {
        QPushButton *btn = launcherGameButton(i);
        if (!btn) {
            continue;
        }
        btn->setStyleSheet(i == m_launcherGameIndex ? kLauncherCardStyleSelected : kLauncherCardStyleNormal);
    }
}

void MainWindow::cycleLauncherGameFocus()
{
    if (m_teardownDone) {
        return;
    }
    m_launcherGameIndex = (m_launcherGameIndex + 1) % kLauncherGameCount;
    updateLauncherGameVisual();
    if (QPushButton *b = launcherGameButton(m_launcherGameIndex)) {
        b->setFocus(Qt::OtherFocusReason);
    }
}

void MainWindow::pollLauncherJoyButtons()
{
    if (m_teardownDone) {
        return;
    }
    if (!m_joyState.serialConnected.load(std::memory_order_relaxed)) {
        m_prevJoyMaskMain = 0;
        return;
    }
    if (!m_tabs || !m_gameStack) {
        return;
    }

    const quint32 cur = m_joyState.buttonMask.load(std::memory_order_relaxed);
    const quint32 rising = cur & ~m_prevJoyMaskMain;
    m_prevJoyMaskMain = cur;

    /* 已进入任一游戏画布时由该游戏窗口独占摇杆 */
    if (m_gameStack->currentIndex() != 0) {
        return;
    }

    if (rising == 0) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (rising & JoyMask::C) {
        if (now - m_lastJoyTabMs < 280) {
            return;
        }
        m_lastJoyTabMs = now;
        const int n = m_tabs->count();
        if (n > 0) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % n);
        }
        syncMainJoyButtonEdges();
        return;
    }

    /* B / A 仅在「游戏」标签下的项目展示区 */
    if (m_tabs->currentIndex() != 0 || m_gameStack->currentIndex() != 0) {
        return;
    }

    if (rising & JoyMask::B) {
        if (now - m_lastJoyCycleMs < 220) {
            return;
        }
        m_lastJoyCycleMs = now;
        cycleLauncherGameFocus();
        return;
    }

    if (rising & JoyMask::A) {
        if (now - m_lastJoyConfirmMs < 350) {
            return;
        }
        m_lastJoyConfirmMs = now;
        switch (m_launcherGameIndex) {
        case 0:
            enterThunderGame();
            break;
        case 1:
            enterTetrisGame();
            break;
        case 2:
            enterGame2048();
            break;
        case 3:
            enterXiangqiGame();
            break;
        case 4:
            enterChessGame();
            break;
        default:
            break;
        }
        return;
    }
}

void MainWindow::teardownBeforeJoyStateDestroyed()
{
    if (m_teardownDone) {
        return;
    }
    m_teardownDone = true;

    if (QApplication *app = qApp) {
        QObject::disconnect(app, &QApplication::aboutToQuit, this, &MainWindow::shutdownGameTimers);
    }

    if (m_joyPollTimer) {
        m_joyPollTimer->stop();
        QObject::disconnect(m_joyPollTimer, nullptr, this, nullptr);
    }

    /* 先停串口/BLE 解析，避免退出时仍写 SharedJoyState */
    if (m_setup) {
        m_setup->setEspUartGameStream(false);
        m_setup->prepareForApplicationQuit();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (m_gameStack) {
        m_gameStack->setCurrentIndex(0);
    }

    if (m_game) {
        m_game->prepareForShutdown();
    }
    if (m_tetris) {
        m_tetris->prepareForShutdown();
    }
    if (m_game2048) {
        m_game2048->prepareForShutdown();
    }
    if (m_xiangqi) {
        m_xiangqi->prepareForShutdown();
    }
    if (m_chess) {
        m_chess->prepareForShutdown();
    }

    blockSignals(true);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    /* 先断开游戏→主窗口槽，避免子控件析构链里再触发 MainWindow 槽 */
    if (m_game) {
        QObject::disconnect(m_game, nullptr, this, nullptr);
    }
    if (m_tetris) {
        QObject::disconnect(m_tetris, nullptr, this, nullptr);
    }
    if (m_game2048) {
        QObject::disconnect(m_game2048, nullptr, this, nullptr);
    }
    if (m_xiangqi) {
        QObject::disconnect(m_xiangqi, nullptr, this, nullptr);
    }
    if (m_chess) {
        QObject::disconnect(m_chess, nullptr, this, nullptr);
    }
    if (m_btnThunder) {
        QObject::disconnect(m_btnThunder, nullptr, this, nullptr);
    }
    if (m_btnTetris) {
        QObject::disconnect(m_btnTetris, nullptr, this, nullptr);
    }
    if (m_btn2048) {
        QObject::disconnect(m_btn2048, nullptr, this, nullptr);
    }
    if (m_btnXiangqi) {
        QObject::disconnect(m_btnXiangqi, nullptr, this, nullptr);
    }
    if (m_btnChess) {
        QObject::disconnect(m_btnChess, nullptr, this, nullptr);
    }

    /* 勿将子控件裸指针置空：对象仍由 Qt 子对象树持有，置空后 processEvents 可能 UAF/堆栈破坏 */
}

void MainWindow::shutdownGameTimers()
{
    teardownBeforeJoyStateDestroyed();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    teardownBeforeJoyStateDestroyed();
    QMainWindow::closeEvent(event);
}
