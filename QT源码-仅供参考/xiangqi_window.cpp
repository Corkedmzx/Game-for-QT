#include "xiangqi_window.h"
#include "shared_joy_state.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>
#include <QtMath>

namespace {

QString pieceLabel(XiangqiEngine::Piece p)
{
    switch (p) {
    case XiangqiEngine::Piece::RedGeneral:
        return QStringLiteral("帅");
    case XiangqiEngine::Piece::RedAdvisor:
        return QStringLiteral("仕");
    case XiangqiEngine::Piece::RedElephant:
        return QStringLiteral("相");
    case XiangqiEngine::Piece::RedHorse:
        return QStringLiteral("马");
    case XiangqiEngine::Piece::RedChariot:
        return QStringLiteral("车");
    case XiangqiEngine::Piece::RedCannon:
        return QStringLiteral("炮");
    case XiangqiEngine::Piece::RedSoldier:
        return QStringLiteral("兵");
    case XiangqiEngine::Piece::BlackGeneral:
        return QStringLiteral("将");
    case XiangqiEngine::Piece::BlackAdvisor:
        return QStringLiteral("士");
    case XiangqiEngine::Piece::BlackElephant:
        return QStringLiteral("象");
    case XiangqiEngine::Piece::BlackHorse:
        return QStringLiteral("马");
    case XiangqiEngine::Piece::BlackChariot:
        return QStringLiteral("车");
    case XiangqiEngine::Piece::BlackCannon:
        return QStringLiteral("炮");
    case XiangqiEngine::Piece::BlackSoldier:
        return QStringLiteral("卒");
    default:
        return QString();
    }
}

} // namespace

XiangqiWindow::XiangqiWindow(SharedJoyState *joyInput, QWidget *parent)
    : QWidget(parent)
    , m_joyInput(joyInput)
    , m_inputTimer(new QTimer(this))
    , m_axisTimer(new QTimer(this))
    , m_thinkTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("XiangqiWindow"));
    const int gridW = (XiangqiEngine::kCols - 1) * kCell;
    const int gridH = (XiangqiEngine::kRows - 1) * kCell;
    setFixedSize(gridW + kSidePanelW * 2 + 48, gridH + 100);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    connect(m_inputTimer, &QTimer::timeout, this, &XiangqiWindow::pollJoyButtons);
    m_inputTimer->start(8);
    connect(m_axisTimer, &QTimer::timeout, this, &XiangqiWindow::tickJoyCursor);
    m_axisTimer->start(38);
    connect(m_thinkTimer, &QTimer::timeout, this, &XiangqiWindow::tickThinkTimer);
    m_thinkTimer->start(1000);

    initGame();
}

XiangqiWindow::~XiangqiWindow()
{
    prepareForShutdown();
}

void XiangqiWindow::prepareForShutdown()
{
    if (m_destroying) {
        return;
    }
    m_destroying = true;
    closeAllMenus();
    if (m_inputTimer) {
        m_inputTimer->stop();
        QObject::disconnect(m_inputTimer, nullptr, this, nullptr);
    }
    if (m_axisTimer) {
        m_axisTimer->stop();
        QObject::disconnect(m_axisTimer, nullptr, this, nullptr);
    }
    if (m_thinkTimer) {
        m_thinkTimer->stop();
        QObject::disconnect(m_thinkTimer, nullptr, this, nullptr);
    }
    m_joyInput = nullptr;
}

void XiangqiWindow::scheduleJoyEdgeResync()
{
    if (m_destroying) {
        return;
    }
    m_prevJoyButtons = m_joyInput ? m_joyInput->buttonMask.load(std::memory_order_relaxed) : 0;
    m_joyLatchU = m_joyLatchD = m_joyLatchL = m_joyLatchR = false;
    m_joyDasU = m_joyDasD = m_joyDasL = m_joyDasR = 0;
}

void XiangqiWindow::returnToLauncher()
{
    if (m_destroying) {
        return;
    }
    closeAllMenus();
    initGame();
    update();
}

void XiangqiWindow::initGame()
{
    m_engine.resetToStart();
    m_gameState = MENU;
    closeAllMenus();
    resetPick();
    m_thinkRemain = kThinkSeconds;
    m_matchPaused = false;
    resetDualConfirm();
}

void XiangqiWindow::closeAllMenus()
{
    m_redMenu = {};
    m_blackMenu = {};
    m_dualPending = false;
}

void XiangqiWindow::startMatch()
{
    m_engine.resetToStart();
    m_gameState = PLAYING;
    m_matchPaused = false;
    closeAllMenus();
    resetPick();
    restartThinkClock();
}

void XiangqiWindow::resetPick()
{
    m_pickPhase = PickPiece;
    m_selR = m_selC = -1;
    m_highlights.clear();
    const bool red = m_engine.sideToMove() == XiangqiEngine::Side::Red;
    m_cursorR = red ? 7 : 2;
    m_cursorC = 4;
}

void XiangqiWindow::refreshHighlights()
{
    m_highlights.clear();
    if (m_selR >= 0 && m_selC >= 0) {
        m_highlights = m_engine.legalMovesFrom(m_selR, m_selC);
    }
}

void XiangqiWindow::restartThinkClock()
{
    m_thinkRemain = kThinkSeconds;
}

void XiangqiWindow::tickThinkTimer()
{
    if (m_destroying || m_gameState != PLAYING) {
        return;
    }
    if (m_matchPaused || m_dualPending) {
        return;
    }
    if (m_engine.result() != XiangqiEngine::Result::Ongoing) {
        m_gameState = GAME_OVER;
        update();
        return;
    }
    --m_thinkRemain;
    if (m_thinkRemain <= 0) {
        m_engine.flagTimeout(m_engine.sideToMove());
        m_gameState = GAME_OVER;
    }
    update();
}

QRect XiangqiWindow::boardRect() const
{
    const int gridW = (XiangqiEngine::kCols - 1) * kCell;
    const int gridH = (XiangqiEngine::kRows - 1) * kCell;
    return QRect(kSidePanelW + 24, 48, gridW, gridH);
}

QPoint XiangqiWindow::intersection(int row, int col) const
{
    const QRect br = boardRect();
    return QPoint(br.left() + col * kCell, br.top() + row * kCell);
}

bool XiangqiWindow::pointToCell(const QPoint &pt, int *row, int *col) const
{
    const QRect br = boardRect().adjusted(-kCell / 2, -kCell / 2, kCell / 2, kCell / 2);
    if (!br.contains(pt)) {
        return false;
    }
    const int c = qRound(static_cast<double>(pt.x() - boardRect().left()) / kCell);
    const int r = qRound(static_cast<double>(pt.y() - boardRect().top()) / kCell);
    if (r < 0 || r >= XiangqiEngine::kRows || c < 0 || c >= XiangqiEngine::kCols) {
        return false;
    }
    if (row) {
        *row = r;
    }
    if (col) {
        *col = c;
    }
    return true;
}

int XiangqiWindow::riverCenterY() const
{
    static constexpr int kRiverTopRow = 4;
    static constexpr int kRiverBottomRow = 5;
    const QRect br = boardRect();
    return br.top() + (kRiverTopRow + kRiverBottomRow) * kCell / 2;
}

QRect XiangqiWindow::menuButtonRectRed() const
{
    const QRect br = boardRect();
    const int y = riverCenterY() - kMenuBtnH / 2;
    return QRect(br.right() + 10, y, kMenuBtnW, kMenuBtnH);
}

QRect XiangqiWindow::menuButtonRectBlack() const
{
    const QRect br = boardRect();
    const int y = riverCenterY() - kMenuBtnH / 2;
    return QRect(br.left() - 10 - kMenuBtnW, y, kMenuBtnW, kMenuBtnH);
}

int XiangqiWindow::pauseMenuItemCount() const
{
    int n = 4;
    if (m_engine.isDrawByRule()) {
        ++n;
    }
    return n;
}

QRect XiangqiWindow::sideMenuPanelRect(bool redSide) const
{
    const QRect br = boardRect();
    const int panelH = m_dualPending ? 260 : (72 + pauseMenuItemCount() * 46);
    int panelTop = riverCenterY() - panelH / 2;
    panelTop = qBound(8, panelTop, qMax(8, height() - panelH - 8));
    if (redSide) {
        return QRect(br.right() + 10, panelTop, kSidePanelW, panelH);
    }
    return QRect(br.left() - 10 - kSidePanelW, panelTop, kSidePanelW, panelH);
}

QRect XiangqiWindow::sideMenuButtonRect(bool redSide, int row) const
{
    const QRect panel = sideMenuPanelRect(redSide);
    const int btnH = 40;
    const int gap = 8;
    const int topOff = m_dualPending ? 122 : 58;
    return QRect(panel.left() + 10, panel.top() + topOff + row * (btnH + gap), panel.width() - 20,
                 btnH);
}

void XiangqiWindow::openMenuRed()
{
    if (m_gameState != PLAYING && m_gameState != GAME_OVER) {
        return;
    }
    m_redMenu.open = true;
    if (!m_dualPending) {
        m_redMenu.overlay = PauseOverlayMain;
        m_redMenu.focusIndex = 0;
    }
}

void XiangqiWindow::openMenuBlack()
{
    if (m_gameState != PLAYING && m_gameState != GAME_OVER) {
        return;
    }
    m_blackMenu.open = true;
    if (!m_dualPending) {
        m_blackMenu.overlay = PauseOverlayMain;
        m_blackMenu.focusIndex = 0;
    }
}

void XiangqiWindow::closeMenuRed()
{
    if (m_dualPending) {
        return;
    }
    m_redMenu = {};
}

void XiangqiWindow::closeMenuBlack()
{
    if (m_dualPending) {
        return;
    }
    m_blackMenu = {};
}

void XiangqiWindow::toggleMenuRed()
{
    if (m_redMenu.open && !m_dualPending) {
        closeMenuRed();
    } else if (!m_dualPending) {
        openMenuRed();
    }
}

void XiangqiWindow::toggleMenuBlack()
{
    if (m_blackMenu.open && !m_dualPending) {
        closeMenuBlack();
    } else if (!m_dualPending) {
        openMenuBlack();
    }
}

void XiangqiWindow::cycleMenuFocus(SideMenu *menu, int delta)
{
    if (!menu || !menu->open || menu->overlay == PauseOverlayNone) {
        return;
    }
    if (menu->overlay == PauseOverlayMain) {
        const int n = pauseMenuItemCount();
        menu->focusIndex = (menu->focusIndex + delta + n) % n;
    } else if (menu->overlay == PauseOverlayDualConfirm) {
        menu->focusIndex = (menu->focusIndex + delta + 2) % 2;
    }
}

void XiangqiWindow::resetDualConfirm()
{
    m_redDualOk = false;
    m_blackDualOk = false;
}

bool XiangqiWindow::dualConfirmSatisfied() const
{
    if (m_gameState == GAME_OVER && m_dualAction == DualExit) {
        return m_redDualOk || m_blackDualOk;
    }
    return m_redDualOk && m_blackDualOk;
}

void XiangqiWindow::beginDualConfirm(DualAction action)
{
    m_dualPending = true;
    m_dualAction = action;
    resetDualConfirm();
    m_redMenu.open = true;
    m_blackMenu.open = true;
    m_redMenu.overlay = PauseOverlayDualConfirm;
    m_blackMenu.overlay = PauseOverlayDualConfirm;
    m_redMenu.focusIndex = 0;
    m_blackMenu.focusIndex = 0;
}

void XiangqiWindow::cancelDualConfirm()
{
    m_dualPending = false;
    resetDualConfirm();
    m_redMenu.overlay = PauseOverlayMain;
    m_blackMenu.overlay = PauseOverlayMain;
}

void XiangqiWindow::applyDualAction()
{
    if (m_destroying) {
        return;
    }
    switch (m_dualAction) {
    case DualPause:
        m_dualPending = false;
        resetDualConfirm();
        m_matchPaused = true;
        closeMenuRed();
        closeMenuBlack();
        break;
    case DualResume:
        m_dualPending = false;
        resetDualConfirm();
        m_matchPaused = false;
        closeMenuRed();
        closeMenuBlack();
        break;
    case DualExit:
        closeAllMenus();
        if (!m_destroying) {
            emit requestReturnToSetup();
        }
        return;
    case DualDraw:
        m_engine.declareDraw();
        m_gameState = GAME_OVER;
        closeAllMenus();
        break;
    default:
        break;
    }
    m_dualPending = false;
    resetDualConfirm();
}

void XiangqiWindow::registerDualOkRed()
{
    if (!m_dualPending) {
        return;
    }
    m_redDualOk = true;
    if (dualConfirmSatisfied()) {
        applyDualAction();
    }
}

void XiangqiWindow::registerDualOkBlack()
{
    if (!m_dualPending) {
        return;
    }
    m_blackDualOk = true;
    if (dualConfirmSatisfied()) {
        applyDualAction();
    }
}

void XiangqiWindow::trySurrender(XiangqiEngine::Side side)
{
    m_engine.resign(side);
    m_gameState = GAME_OVER;
    closeAllMenus();
}

void XiangqiWindow::activateMenuSelection(bool redSide)
{
    SideMenu *menu = redSide ? &m_redMenu : &m_blackMenu;
    if (!menu->open || menu->overlay == PauseOverlayNone) {
        return;
    }

    if (menu->overlay == PauseOverlayDualConfirm) {
        if (menu->focusIndex == 1) {
            cancelDualConfirm();
        }
        return;
    }

    const bool drawItem = m_engine.isDrawByRule();
    const int idxSurrender = drawItem ? 4 : 3;
    const int idxDraw = drawItem ? 3 : -1;

    switch (menu->focusIndex) {
    case 0:
        if (m_matchPaused) {
            beginDualConfirm(DualResume);
        } else if (redSide) {
            closeMenuRed();
        } else {
            closeMenuBlack();
        }
        break;
    case 1:
        if (!m_matchPaused) {
            beginDualConfirm(DualPause);
        }
        break;
    case 2:
        beginDualConfirm(DualExit);
        break;
    default:
        if (menu->focusIndex == idxDraw) {
            beginDualConfirm(DualDraw);
        } else if (menu->focusIndex == idxSurrender) {
            trySurrender(redSide ? XiangqiEngine::Side::Red : XiangqiEngine::Side::Black);
        }
        break;
    }
}

void XiangqiWindow::menuSurrenderRed()
{
    if (!m_redMenu.open || m_redMenu.overlay != PauseOverlayMain) {
        return;
    }
    const int idxSurrender = m_engine.isDrawByRule() ? 4 : 3;
    if (m_redMenu.focusIndex == idxSurrender) {
        trySurrender(XiangqiEngine::Side::Red);
    } else {
        activateMenuSelection(true);
    }
}

void XiangqiWindow::menuSurrenderBlack()
{
    if (!m_blackMenu.open || m_blackMenu.overlay != PauseOverlayMain) {
        return;
    }
    const int idxSurrender = m_engine.isDrawByRule() ? 4 : 3;
    if (m_blackMenu.focusIndex == idxSurrender) {
        trySurrender(XiangqiEngine::Side::Black);
    } else {
        activateMenuSelection(false);
    }
}

void XiangqiWindow::p1MoveCursor(int dr, int dc)
{
    if (m_matchPaused || m_gameState != PLAYING
        || m_engine.sideToMove() != XiangqiEngine::Side::Red) {
        return;
    }
    if (m_redMenu.open && !m_dualPending) {
        return;
    }
    m_cursorR = qBound(0, m_cursorR + dr, XiangqiEngine::kRows - 1);
    m_cursorC = qBound(0, m_cursorC + dc, XiangqiEngine::kCols - 1);
}

void XiangqiWindow::p2MoveCursor(int dr, int dc)
{
    if (m_matchPaused || m_gameState != PLAYING
        || m_engine.sideToMove() != XiangqiEngine::Side::Black) {
        return;
    }
    if (m_blackMenu.open && !m_dualPending) {
        return;
    }
    m_cursorR = qBound(0, m_cursorR + dr, XiangqiEngine::kRows - 1);
    m_cursorC = qBound(0, m_cursorC + dc, XiangqiEngine::kCols - 1);
}

void XiangqiWindow::p1Confirm()
{
    if (m_destroying) {
        return;
    }
    if (m_dualPending) {
        if (m_redMenu.focusIndex == 1) {
            cancelDualConfirm();
        } else {
            registerDualOkRed();
        }
        return;
    }
    if (m_redMenu.open) {
        menuSurrenderRed();
        return;
    }
    if (m_gameState == MENU) {
        startMatch();
        return;
    }
    if (m_matchPaused || m_gameState != PLAYING
        || m_engine.sideToMove() != XiangqiEngine::Side::Red) {
        return;
    }

    if (m_pickPhase == PickPiece) {
        const XiangqiEngine::Piece p = m_engine.at(m_cursorR, m_cursorC);
        if (p == XiangqiEngine::Piece::Empty
            || XiangqiEngine::sideOf(p) != XiangqiEngine::Side::Red) {
            return;
        }
        if (m_engine.legalMovesFrom(m_cursorR, m_cursorC).isEmpty()) {
            return;
        }
        m_selR = m_cursorR;
        m_selC = m_cursorC;
        m_pickPhase = PickDestination;
        refreshHighlights();
        return;
    }

    for (const XiangqiEngine::Move &mv : m_highlights) {
        if (mv.tr == m_cursorR && mv.tc == m_cursorC) {
            if (m_engine.applyMove(mv)) {
                resetPick();
                restartThinkClock();
                if (m_engine.result() != XiangqiEngine::Result::Ongoing) {
                    m_gameState = GAME_OVER;
                }
            }
            return;
        }
    }
    m_pickPhase = PickPiece;
    m_selR = m_selC = -1;
    m_highlights.clear();
    p1Confirm();
}

void XiangqiWindow::p2Confirm()
{
    if (m_destroying) {
        return;
    }
    if (m_dualPending) {
        if (m_blackMenu.focusIndex == 1) {
            cancelDualConfirm();
        } else {
            registerDualOkBlack();
        }
        return;
    }
    if (m_blackMenu.open) {
        menuSurrenderBlack();
        return;
    }
    if (m_gameState == MENU) {
        startMatch();
        return;
    }
    if (m_matchPaused || m_gameState != PLAYING
        || m_engine.sideToMove() != XiangqiEngine::Side::Black) {
        return;
    }

    if (m_pickPhase == PickPiece) {
        const XiangqiEngine::Piece p = m_engine.at(m_cursorR, m_cursorC);
        if (p == XiangqiEngine::Piece::Empty
            || XiangqiEngine::sideOf(p) != XiangqiEngine::Side::Black) {
            return;
        }
        if (m_engine.legalMovesFrom(m_cursorR, m_cursorC).isEmpty()) {
            return;
        }
        m_selR = m_cursorR;
        m_selC = m_cursorC;
        m_pickPhase = PickDestination;
        refreshHighlights();
        return;
    }

    for (const XiangqiEngine::Move &mv : m_highlights) {
        if (mv.tr == m_cursorR && mv.tc == m_cursorC) {
            if (m_engine.applyMove(mv)) {
                resetPick();
                restartThinkClock();
                if (m_engine.result() != XiangqiEngine::Result::Ongoing) {
                    m_gameState = GAME_OVER;
                }
            }
            return;
        }
    }
    m_pickPhase = PickPiece;
    m_selR = m_selC = -1;
    m_highlights.clear();
    p2Confirm();
}

void XiangqiWindow::pollJoyButtons()
{
    if (m_destroying) {
        return;
    }
    if (!m_joyInput || !m_joyInput->serialConnected.load(std::memory_order_relaxed)) {
        m_prevJoyButtons = 0;
        return;
    }
    if (!isVisible()) {
        m_prevJoyButtons = m_joyInput->buttonMask.load(std::memory_order_relaxed);
        return;
    }

    const quint32 cur = m_joyInput->buttonMask.load(std::memory_order_relaxed);
    const quint32 rising = cur & ~m_prevJoyButtons;
    m_prevJoyButtons = cur;
    if (rising == 0) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (m_dualPending && (rising & JoyMask::A)) {
        if (m_blackMenu.focusIndex == 1) {
            cancelDualConfirm();
        } else {
            registerDualOkBlack();
        }
        update();
        return;
    }

    if (m_blackMenu.open) {
        if (rising & JoyMask::B) {
            cycleMenuFocus(&m_blackMenu, 1);
            update();
            return;
        }
        if (rising & JoyMask::A && !m_dualPending) {
            p2Confirm();
            update();
            return;
        }
        if ((rising & JoyMask::C) || (rising & JoyMask::D)) {
            if (!m_dualPending) {
                closeMenuBlack();
            }
            update();
            return;
        }
    }

    if ((m_gameState == MENU || m_gameState == GAME_OVER) && (rising & JoyMask::A)) {
        if (now - m_lastJoyMenuActionMs >= 350) {
            m_lastJoyMenuActionMs = now;
            if (m_gameState == MENU) {
                startMatch();
            } else {
                openMenuBlack();
            }
        }
        update();
        return;
    }

    if ((rising & (JoyMask::C | JoyMask::D)) && (m_gameState == PLAYING || m_gameState == GAME_OVER)) {
        toggleMenuBlack();
        update();
        return;
    }

    if ((rising & JoyMask::D) && m_gameState == MENU) {
        if (!m_destroying) {
            emit requestReturnToSetup();
        }
        update();
        return;
    }

    if (m_destroying) {
        return;
    }

    if (rising & JoyMask::A) {
        p2Confirm();
        update();
    }
}

void XiangqiWindow::tickJoyCursor()
{
    if (m_destroying || !isVisible() || m_gameState != PLAYING || m_matchPaused) {
        return;
    }
    if (!m_joyInput || !m_joyInput->serialConnected.load(std::memory_order_relaxed)) {
        return;
    }
    if (m_engine.sideToMove() != XiangqiEngine::Side::Black) {
        return;
    }
    if (m_blackMenu.open && !m_dualPending) {
        return;
    }

    const float nx = m_joyInput->nx.load(std::memory_order_relaxed);
    const float ny = m_joyInput->ny.load(std::memory_order_relaxed);

    constexpr float kOn = 0.50f;
    constexpr float kOff = 0.24f;
    constexpr float kCross = 0.34f;
    constexpr float kTiny = 0.10f;
    constexpr int kDasInit = 18;
    constexpr int kDasRep = 8;

    if (qMax(qAbs(nx), qAbs(ny)) < kTiny) {
        m_joyLatchU = m_joyLatchD = m_joyLatchL = m_joyLatchR = false;
        m_joyDasU = m_joyDasD = m_joyDasL = m_joyDasR = 0;
        return;
    }

    const float nx2 = nx * nx;
    const float ny2 = ny * ny;
    constexpr float kAxisSep = 1.58f;
    const bool horizDominant = nx2 >= ny2 * kAxisSep;
    const bool vertDominant = ny2 >= nx2 * kAxisSep;
    /* ny 向上为正，与屏幕 row 减小（视觉上移）一致，同 P1 的 W/S */
    const int vUp = -1;
    const int vDown = 1;

    auto stepDas = [&](bool want, bool &latch, int &das, int dr, int dc) {
        if (!want) {
            if (latch) {
                latch = false;
                das = 0;
            }
            return false;
        }
        if (!latch) {
            latch = true;
            das = 1;
            p2MoveCursor(dr, dc);
            return true;
        }
        ++das;
        if (das > kDasInit && ((das - kDasInit - 1) % kDasRep == 0)) {
            p2MoveCursor(dr, dc);
            return true;
        }
        return false;
    };

    bool moved = false;
    if (horizDominant) {
        m_joyLatchU = m_joyLatchD = false;
        m_joyDasU = m_joyDasD = 0;
        const bool wL = (nx <= (m_joyLatchL ? -kOff : -kOn)) && qAbs(ny) < kCross;
        const bool wR = (nx >= (m_joyLatchR ? kOff : kOn)) && qAbs(ny) < kCross;
        if (stepDas(wL, m_joyLatchL, m_joyDasL, 0, -1)) {
            moved = true;
        }
        if (stepDas(wR, m_joyLatchR, m_joyDasR, 0, 1)) {
            moved = true;
        }
    } else if (vertDominant) {
        m_joyLatchL = m_joyLatchR = false;
        m_joyDasL = m_joyDasR = 0;
        const bool wU = (ny >= (m_joyLatchU ? kOff : kOn)) && qAbs(nx) < kCross;
        const bool wD = (ny <= (m_joyLatchD ? -kOff : -kOn)) && qAbs(nx) < kCross;
        if (ny >= 0.f) {
            m_joyLatchD = false;
            if (stepDas(wU, m_joyLatchU, m_joyDasU, vUp, 0)) {
                moved = true;
            }
        } else {
            m_joyLatchU = false;
            if (stepDas(wD, m_joyLatchD, m_joyDasD, vDown, 0)) {
                moved = true;
            }
        }
    }

    if (moved) {
        update();
    }
}

void XiangqiWindow::drawBoard(QPainter &painter) const
{
    const QRect br = boardRect();
    painter.fillRect(br.adjusted(-12, -12, 12, 12), QColor(88, 62, 38));
    painter.fillRect(br.adjusted(-4, -4, 4, 4), QColor(210, 170, 110));

    /* 河界：第 5、6 横线之间为空白河界，竖线不断开贯穿（标准棋盘） */
    static constexpr int kRiverTopRow = 4;
    static constexpr int kRiverBottomRow = 5;
    const int riverTopY = br.top() + kRiverTopRow * kCell;
    const int riverBottomY = br.top() + kRiverBottomRow * kCell;
    const QRect riverBand(br.left(), riverTopY, br.width(), riverBottomY - riverTopY);
    painter.fillRect(riverBand, QColor(228, 198, 140));

    const QPen linePen(QColor(40, 28, 16), 2);
    painter.setPen(linePen);

    for (int r = 0; r < XiangqiEngine::kRows; ++r) {
        const int y = br.top() + r * kCell;
        painter.drawLine(br.left(), y, br.right(), y);
    }
    for (int c = 0; c < XiangqiEngine::kCols; ++c) {
        const int x = br.left() + c * kCell;
        painter.drawLine(x, br.top(), x, riverTopY);
        painter.drawLine(x, riverBottomY, x, br.bottom());
    }

    painter.setPen(QColor(100, 60, 30));
    QFont f(QStringLiteral("KaiTi"), 15, QFont::Bold);
    painter.setFont(f);
    painter.drawText(riverBand, Qt::AlignCenter, QStringLiteral("楚 河          汉 界"));

    painter.setPen(QPen(QColor(40, 28, 16), 1));
    painter.drawLine(intersection(0, 3), intersection(2, 5));
    painter.drawLine(intersection(0, 5), intersection(2, 3));
    painter.drawLine(intersection(7, 3), intersection(9, 5));
    painter.drawLine(intersection(7, 5), intersection(9, 3));
}

void XiangqiWindow::drawHighlights(QPainter &painter) const
{
    const XiangqiEngine::Side mover = m_engine.sideToMove();

    for (const XiangqiEngine::Move &mv : m_highlights) {
        const XiangqiEngine::Piece tgt = m_engine.at(mv.tr, mv.tc);
        const bool isCapture =
            tgt != XiangqiEngine::Piece::Empty && XiangqiEngine::sideOf(tgt) != mover;
        if (!isCapture) {
            painter.setBrush(QColor(80, 220, 120, 160));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(intersection(mv.tr, mv.tc), 10, 10);
        }
    }

    if (m_selR >= 0) {
        const QPoint selPt = intersection(m_selR, m_selC);
        painter.setPen(QPen(QColor(60, 220, 90), 4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(selPt, 28, 28);
    }

    for (const XiangqiEngine::Move &mv : m_highlights) {
        const XiangqiEngine::Piece tgt = m_engine.at(mv.tr, mv.tc);
        if (tgt == XiangqiEngine::Piece::Empty
            || XiangqiEngine::sideOf(tgt) == mover) {
            continue;
        }
        const QPoint capPt = intersection(mv.tr, mv.tc);
        painter.setPen(QPen(QColor(255, 50, 50), 4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(capPt, 28, 28);
    }

    const bool cursorOnSelection = m_selR >= 0 && m_cursorR == m_selR && m_cursorC == m_selC;
    if (!cursorOnSelection) {
        const QPoint cur = intersection(m_cursorR, m_cursorC);
        painter.setPen(QPen(QColor(80, 180, 255), 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(cur, 26, 26);
    }
}

void XiangqiWindow::drawPieces(QPainter &painter) const
{
    for (int r = 0; r < XiangqiEngine::kRows; ++r) {
        for (int c = 0; c < XiangqiEngine::kCols; ++c) {
            const XiangqiEngine::Piece p = m_engine.at(r, c);
            if (p == XiangqiEngine::Piece::Empty) {
                continue;
            }
            const QPoint pt = intersection(r, c);
            const bool red = XiangqiEngine::isRed(p);
            painter.setBrush(red ? QColor(220, 60, 50) : QColor(40, 40, 48));
            painter.setPen(QPen(red ? QColor(120, 20, 20) : QColor(200, 200, 210), 2));
            painter.drawEllipse(pt, 24, 24);
            painter.setPen(QColor(255, 255, 255));
            QFont f(QStringLiteral("KaiTi"), 16, QFont::Bold);
            painter.setFont(f);
            painter.drawText(QRect(pt.x() - 24, pt.y() - 24, 48, 48), Qt::AlignCenter, pieceLabel(p));
        }
    }
}

void XiangqiWindow::drawHud(QPainter &painter) const
{
    painter.setPen(QColor(230, 235, 250));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 11));
    const QString side =
        (m_engine.sideToMove() == XiangqiEngine::Side::Red) ? QStringLiteral("红方(P1 键盘)")
                                                            : QStringLiteral("黑方(P2 摇杆)");
    painter.drawText(12, 24, QStringLiteral("行棋: %1").arg(side));
    if (m_matchPaused) {
        painter.setPen(QColor(255, 200, 100));
        painter.drawText(12, 42, QStringLiteral("对局已暂停（双方确认后继续）"));
    } else if (m_gameState == PLAYING) {
        painter.drawText(12, 42, QStringLiteral("思考: %1 秒").arg(m_thinkRemain));
    }

    switch (m_engine.result()) {
    case XiangqiEngine::Result::RedWin:
        painter.setPen(QColor(255, 200, 100));
        painter.drawText(12, height() - 12, QStringLiteral("红方胜"));
        break;
    case XiangqiEngine::Result::BlackWin:
        painter.setPen(QColor(255, 200, 100));
        painter.drawText(12, height() - 12, QStringLiteral("黑方胜"));
        break;
    case XiangqiEngine::Result::Draw:
        painter.setPen(QColor(255, 200, 100));
        painter.drawText(12, height() - 12, QStringLiteral("和棋"));
        break;
    default:
        break;
    }
}

void XiangqiWindow::drawMenuButtons(QPainter &painter) const
{
    if (m_gameState != PLAYING && m_gameState != GAME_OVER) {
        return;
    }
    auto drawBtn = [&](const QRect &r, bool open, const QString &sideLabel) {
        painter.setBrush(open ? QColor(70, 100, 150) : QColor(55, 62, 82));
        painter.setPen(QPen(QColor(120, 160, 220), 2));
        painter.drawRoundedRect(r, 6, 6);
        painter.setPen(QColor(240, 245, 255));
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
        painter.drawText(r.adjusted(6, 4, -6, -r.height() / 2 + 2), Qt::AlignHCenter | Qt::AlignVCenter,
                         sideLabel);
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 12, QFont::Bold));
        painter.drawText(r.adjusted(6, r.height() / 2 - 2, -6, -4), Qt::AlignHCenter | Qt::AlignVCenter,
                         QStringLiteral("菜单"));
    };
    drawBtn(menuButtonRectBlack(), m_blackMenu.open, QStringLiteral("黑方 P2"));
    drawBtn(menuButtonRectRed(), m_redMenu.open, QStringLiteral("红方 P1"));
}

void XiangqiWindow::drawCheckAlert(QPainter &painter, bool redSide) const
{
    if (m_gameState != PLAYING && m_gameState != GAME_OVER) {
        return;
    }
    const XiangqiEngine::Side side =
        redSide ? XiangqiEngine::Side::Red : XiangqiEngine::Side::Black;
    if (!m_engine.isInCheck(side)) {
        return;
    }

    const QRect br = boardRect();
    QRect alertRect;
    if (redSide) {
        alertRect = QRect(br.right() + 12, br.bottom() - 58, kSidePanelW - 16, 52);
    } else {
        alertRect = QRect(br.left() - kSidePanelW + 4, br.top() + 4, kSidePanelW - 16, 52);
    }

    QFont f(QStringLiteral("KaiTi"), 48, QFont::Bold);
    painter.setFont(f);
    painter.setPen(QColor(255, 45, 45));
    painter.drawText(alertRect, Qt::AlignCenter, QStringLiteral("将军"));
}

void XiangqiWindow::drawSideMenu(QPainter &painter, bool redSide) const
{
    const SideMenu &menu = redSide ? m_redMenu : m_blackMenu;
    if (!menu.open) {
        return;
    }

    const QRect panel = sideMenuPanelRect(redSide);
    painter.setBrush(QColor(32, 38, 58, 240));
    painter.setPen(QPen(QColor(90, 120, 180), 2));
    painter.drawRoundedRect(panel, 8, 8);

    painter.setPen(QColor(240, 245, 255));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 12, QFont::Bold));
    painter.drawText(panel.adjusted(0, 10, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                     redSide ? QStringLiteral("红方菜单") : QStringLiteral("黑方菜单"));

    auto drawBtn = [&](int row, const QString &text, bool focused) {
        const QRect br = sideMenuButtonRect(redSide, row);
        painter.setBrush(focused ? QColor(70, 110, 180) : QColor(48, 56, 78));
        painter.setPen(focused ? QPen(QColor(140, 190, 255), 2) : QPen(QColor(70, 80, 110), 1));
        painter.drawRoundedRect(br, 5, 5);
        painter.setPen(QColor(255, 255, 255));
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
        painter.drawText(br, Qt::AlignCenter, text);
    };

    if (menu.overlay == PauseOverlayDualConfirm) {
        QString title;
        switch (m_dualAction) {
        case DualPause:
            title = QStringLiteral("双方确认后暂停对局");
            break;
        case DualResume:
            title = QStringLiteral("双方确认后继续对局");
            break;
        case DualExit:
            title = QStringLiteral("双方确认退出");
            break;
        case DualDraw:
            title = QStringLiteral("双方确认求和");
            break;
        }
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
        painter.drawText(panel.adjusted(8, 32, -8, 0), Qt::AlignHCenter | Qt::AlignTop, title);
        painter.drawText(
            panel.adjusted(8, 56, -8, 0), Qt::TextWordWrap,
            QStringLiteral("P1 Enter: %1\nP2 摇杆A: %2")
                .arg(m_redDualOk ? QStringLiteral("已确认") : QStringLiteral("待确认"))
                .arg(m_blackDualOk ? QStringLiteral("已确认") : QStringLiteral("待确认")));
        if (m_gameState == GAME_OVER && m_dualAction == DualExit) {
            painter.drawText(panel.adjusted(8, 100, -8, 0), Qt::AlignHCenter | Qt::AlignTop,
                             QStringLiteral("已结束：任一方确认可退出"));
        }
        drawBtn(0, QStringLiteral("确认"), menu.focusIndex == 0);
        drawBtn(1, QStringLiteral("返回"), menu.focusIndex == 1);
        return;
    }

    drawBtn(0, m_matchPaused ? QStringLiteral("继续对局(双方确认)") : QStringLiteral("继续"),
             menu.focusIndex == 0);
    drawBtn(1, m_matchPaused ? QStringLiteral("已暂停") : QStringLiteral("暂停"), menu.focusIndex == 1);
    drawBtn(2, QStringLiteral("退出"), menu.focusIndex == 2);
    if (m_engine.isDrawByRule()) {
        drawBtn(3, QStringLiteral("求和"), menu.focusIndex == 3);
        drawBtn(4, QStringLiteral("投降"), menu.focusIndex == 4);
    } else {
        drawBtn(3, QStringLiteral("投降"), menu.focusIndex == 3);
    }
}

void XiangqiWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 32, 48));

    drawBoard(p);
    drawPieces(p);
    drawHighlights(p);
    drawHud(p);
    drawCheckAlert(p, false);
    drawCheckAlert(p, true);
    drawMenuButtons(p);
    drawSideMenu(p, true);
    drawSideMenu(p, false);

    if (m_gameState == MENU) {
        p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 15, QFont::Bold));
        p.setPen(QColor(255, 220, 140));
        p.drawText(
            rect(), Qt::AlignCenter,
            QStringLiteral("中国象棋\n\n红方 P1：WASD + Enter（右侧河界「菜单」 ESC/P）\n"
                             "黑方 P2：摇杆 + A（左侧河界「菜单」 摇杆C/D）\n"
                             "选子绿圈 · 可吃子红圈 · 被将军一侧大字提示\n"
                             "棋子落在格子线交点上\n思考 60 秒\n\n"
                             "空格/Enter 或 摇杆 A 开始"));
    }

    if (m_gameState == GAME_OVER && !m_redMenu.open && !m_blackMenu.open) {
        p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 14, QFont::Bold));
        p.setPen(QColor(255, 180, 120));
        p.drawText(boardRect(), Qt::AlignCenter, QStringLiteral("对局结束"));
    }
}

void XiangqiWindow::keyPressEvent(QKeyEvent *event)
{
    if (!isVisible() || m_destroying) {
        event->ignore();
        return;
    }
    const bool repeat = event->isAutoRepeat();

    switch (event->key()) {
    case Qt::Key_Escape:
    case Qt::Key_P:
        if (m_gameState == MENU) {
            if (!m_destroying) {
                emit requestReturnToSetup();
            }
        } else if (m_dualPending) {
            cancelDualConfirm();
        } else {
            toggleMenuRed();
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        p1Confirm();
        break;
    case Qt::Key_Space:
        if (m_gameState == MENU) {
            startMatch();
        }
        break;
    case Qt::Key_W:
        m_keyW = true;
        if (!repeat && m_redMenu.open) {
            cycleMenuFocus(&m_redMenu, -1);
        } else if (!repeat && m_gameState == PLAYING) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastKbdCursorMs >= 120) {
                m_lastKbdCursorMs = now;
                p1MoveCursor(-1, 0);
            }
        }
        break;
    case Qt::Key_S:
        m_keyS = true;
        if (!repeat && m_redMenu.open) {
            cycleMenuFocus(&m_redMenu, 1);
        } else if (!repeat && m_gameState == PLAYING) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastKbdCursorMs >= 120) {
                m_lastKbdCursorMs = now;
                p1MoveCursor(1, 0);
            }
        }
        break;
    case Qt::Key_A:
        m_keyA = true;
        if (!repeat && m_gameState == PLAYING && !m_redMenu.open) {
            p1MoveCursor(0, -1);
        }
        break;
    case Qt::Key_D:
        m_keyD = true;
        if (!repeat && m_gameState == PLAYING && !m_redMenu.open) {
            p1MoveCursor(0, 1);
        }
        break;
    default:
        break;
    }
    update();
}

void XiangqiWindow::keyReleaseEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_W:
        m_keyW = false;
        break;
    case Qt::Key_S:
        m_keyS = false;
        break;
    case Qt::Key_A:
        m_keyA = false;
        break;
    case Qt::Key_D:
        m_keyD = false;
        break;
    default:
        break;
    }
}

void XiangqiWindow::mousePressEvent(QMouseEvent *event)
{
    if (!event || m_destroying) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint pt = event->position().toPoint();

    if (event->button() == Qt::LeftButton && menuButtonRectRed().contains(pt)) {
        toggleMenuRed();
        event->accept();
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && menuButtonRectBlack().contains(pt)) {
        toggleMenuBlack();
        event->accept();
        update();
        return;
    }

    if (event->button() == Qt::LeftButton && m_redMenu.open) {
        const int nBtn = m_dualPending ? 2 : pauseMenuItemCount();
        for (int row = 0; row < nBtn; ++row) {
            if (sideMenuButtonRect(true, row).contains(pt)) {
                m_redMenu.focusIndex = row;
                p1Confirm();
                event->accept();
                update();
                return;
            }
        }
    }

    if (event->button() == Qt::LeftButton && m_gameState == MENU) {
        startMatch();
        event->accept();
        update();
        return;
    }

    QWidget::mousePressEvent(event);
}

void XiangqiWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!event) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint pt = event->position().toPoint();
    if (menuButtonRectRed().contains(pt) || menuButtonRectBlack().contains(pt)) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    unsetCursor();
    QWidget::mouseMoveEvent(event);
}

void XiangqiWindow::closeEvent(QCloseEvent *event)
{
    prepareForShutdown();
    QWidget::closeEvent(event);
}
