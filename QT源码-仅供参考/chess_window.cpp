#include "chess_window.h"
#include "shared_joy_state.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QFontInfo>
#include <QPaintEvent>
#include <QTimer>
#include <QtMath>

namespace {

/** Unicode 国际象棋符号（U+2654–U+265F，参见 Wikimedia / Noto Sans Symbols） */
QString pieceGlyph(ChessEngine::Piece p)
{
    switch (p) {
    case ChessEngine::Piece::WhiteKing:
        return QStringLiteral("♔");
    case ChessEngine::Piece::WhiteQueen:
        return QStringLiteral("♕");
    case ChessEngine::Piece::WhiteRook:
        return QStringLiteral("♖");
    case ChessEngine::Piece::WhiteBishop:
        return QStringLiteral("♗");
    case ChessEngine::Piece::WhiteKnight:
        return QStringLiteral("♘");
    case ChessEngine::Piece::WhitePawn:
        return QStringLiteral("♙");
    case ChessEngine::Piece::BlackKing:
        return QStringLiteral("♚");
    case ChessEngine::Piece::BlackQueen:
        return QStringLiteral("♛");
    case ChessEngine::Piece::BlackRook:
        return QStringLiteral("♜");
    case ChessEngine::Piece::BlackBishop:
        return QStringLiteral("♝");
    case ChessEngine::Piece::BlackKnight:
        return QStringLiteral("♞");
    case ChessEngine::Piece::BlackPawn:
        return QStringLiteral("♟");
    default:
        return QString();
    }
}

QFont chessPieceFont(int pixelSize)
{
    QFont f(QStringLiteral("Segoe UI Symbol"));
    if (!QFontInfo(f).family().contains(QStringLiteral("Segoe"))) {
        f.setFamily(QStringLiteral("DejaVu Sans"));
    }
    f.setPixelSize(pixelSize);
    return f;
}

} // namespace

ChessWindow::ChessWindow(SharedJoyState *joyInput, QWidget *parent)
    : QWidget(parent)
    , m_joyInput(joyInput)
    , m_inputTimer(new QTimer(this))
    , m_axisTimer(new QTimer(this))
    , m_thinkTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("ChessWindow"));
    const int gridW = ChessEngine::kCols * kCell;
    const int gridH = ChessEngine::kRows * kCell;
    setFixedSize(gridW + kSidePanelW * 2 + 48, gridH + 100);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    connect(m_inputTimer, &QTimer::timeout, this, &ChessWindow::pollJoyButtons);
    m_inputTimer->start(8);
    connect(m_axisTimer, &QTimer::timeout, this, &ChessWindow::tickJoyCursor);
    m_axisTimer->start(38);
    connect(m_thinkTimer, &QTimer::timeout, this, &ChessWindow::tickThinkTimer);
    m_thinkTimer->start(1000);

    initGame();
}

ChessWindow::~ChessWindow()
{
    prepareForShutdown();
}

void ChessWindow::prepareForShutdown()
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

void ChessWindow::scheduleJoyEdgeResync()
{
    if (m_destroying) {
        return;
    }
    m_prevJoyButtons = m_joyInput ? m_joyInput->buttonMask.load(std::memory_order_relaxed) : 0;
    m_joyLatchU = m_joyLatchD = m_joyLatchL = m_joyLatchR = false;
    m_joyDasU = m_joyDasD = m_joyDasL = m_joyDasR = 0;
}

void ChessWindow::returnToLauncher()
{
    if (m_destroying) {
        return;
    }
    closeAllMenus();
    initGame();
    update();
}

void ChessWindow::initGame()
{
    m_engine.resetToStart();
    m_gameState = MENU;
    closeAllMenus();
    resetPick();
    m_thinkRemain = kThinkSeconds;
    m_matchPaused = false;
    resetDualConfirm();
}

void ChessWindow::closeAllMenus()
{
    m_whiteMenu = {};
    m_blackMenu = {};
    m_dualPending = false;
}

void ChessWindow::startMatch()
{
    m_engine.resetToStart();
    m_gameState = PLAYING;
    m_matchPaused = false;
    closeAllMenus();
    resetPick();
    restartThinkClock();
}

void ChessWindow::resetPick()
{
    m_pickPhase = PickPiece;
    m_selR = m_selC = -1;
    m_highlights.clear();
    const bool white = m_engine.sideToMove() == ChessEngine::Side::White;
    m_cursorR = white ? 6 : 1;
    m_cursorC = 4;
}

void ChessWindow::refreshHighlights()
{
    m_highlights.clear();
    if (m_selR >= 0 && m_selC >= 0) {
        m_highlights = m_engine.legalMovesFrom(m_selR, m_selC);
    }
}

void ChessWindow::restartThinkClock()
{
    m_thinkRemain = kThinkSeconds;
}

void ChessWindow::tickThinkTimer()
{
    if (m_destroying || m_gameState != PLAYING) {
        return;
    }
    if (m_matchPaused || m_dualPending) {
        return;
    }
    if (m_engine.result() != ChessEngine::Result::Ongoing) {
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

QRect ChessWindow::boardRect() const
{
    const int gridW = ChessEngine::kCols * kCell;
    const int gridH = ChessEngine::kRows * kCell;
    return QRect(kSidePanelW + 24, 48, gridW, gridH);
}

int ChessWindow::boardCenterY() const
{
    const QRect br = boardRect();
    return br.top() + br.height() / 2;
}

QPoint ChessWindow::squareCenter(int row, int col) const
{
    const QRect br = boardRect();
    return QPoint(br.left() + col * kCell + kCell / 2, br.top() + row * kCell + kCell / 2);
}

bool ChessWindow::pointToCell(const QPoint &pt, int *row, int *col) const
{
    const QRect br = boardRect();
    if (!br.contains(pt)) {
        return false;
    }
    const int c = (pt.x() - br.left()) / kCell;
    const int r = (pt.y() - br.top()) / kCell;
    if (r < 0 || r >= ChessEngine::kRows || c < 0 || c >= ChessEngine::kCols) {
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

QRect ChessWindow::menuButtonRectWhite() const
{
    const QRect br = boardRect();
    const int y = boardCenterY() - kMenuBtnH / 2;
    return QRect(br.right() + 10, y, kMenuBtnW, kMenuBtnH);
}

QRect ChessWindow::menuButtonRectBlack() const
{
    const QRect br = boardRect();
    const int y = boardCenterY() - kMenuBtnH / 2;
    return QRect(br.left() - 10 - kMenuBtnW, y, kMenuBtnW, kMenuBtnH);
}

int ChessWindow::pauseMenuItemCount() const
{
    int n = 4;
    if (m_engine.isDrawByRule()) {
        ++n;
    }
    return n;
}

QRect ChessWindow::sideMenuPanelRect(bool whiteSide) const
{
    const QRect br = boardRect();
    const int panelH = m_dualPending ? 260 : (72 + pauseMenuItemCount() * 46);
    int panelTop = boardCenterY() - panelH / 2;
    panelTop = qBound(8, panelTop, qMax(8, height() - panelH - 8));
    if (whiteSide) {
        return QRect(br.right() + 10, panelTop, kSidePanelW, panelH);
    }
    return QRect(br.left() - 10 - kSidePanelW, panelTop, kSidePanelW, panelH);
}

QRect ChessWindow::sideMenuButtonRect(bool whiteSide, int row) const
{
    const QRect panel = sideMenuPanelRect(whiteSide);
    const int btnH = 40;
    const int gap = 8;
    const int topOff = m_dualPending ? 122 : 58;
    return QRect(panel.left() + 10, panel.top() + topOff + row * (btnH + gap), panel.width() - 20,
                 btnH);
}

void ChessWindow::openMenuWhite()
{
    if (m_gameState != PLAYING && m_gameState != GAME_OVER) {
        return;
    }
    m_whiteMenu.open = true;
    if (!m_dualPending) {
        m_whiteMenu.overlay = PauseOverlayMain;
        m_whiteMenu.focusIndex = 0;
    }
}

void ChessWindow::openMenuBlack()
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

void ChessWindow::closeMenuWhite()
{
    if (m_dualPending) {
        return;
    }
    m_whiteMenu = {};
}

void ChessWindow::closeMenuBlack()
{
    if (m_dualPending) {
        return;
    }
    m_blackMenu = {};
}

void ChessWindow::toggleMenuWhite()
{
    if (m_whiteMenu.open && !m_dualPending) {
        closeMenuWhite();
    } else if (!m_dualPending) {
        openMenuWhite();
    }
}

void ChessWindow::toggleMenuBlack()
{
    if (m_blackMenu.open && !m_dualPending) {
        closeMenuBlack();
    } else if (!m_dualPending) {
        openMenuBlack();
    }
}

void ChessWindow::cycleMenuFocus(SideMenu *menu, int delta)
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

void ChessWindow::resetDualConfirm()
{
    m_whiteDualOk = false;
    m_blackDualOk = false;
}

bool ChessWindow::dualConfirmSatisfied() const
{
    if (m_gameState == GAME_OVER && m_dualAction == DualExit) {
        return m_whiteDualOk || m_blackDualOk;
    }
    return m_whiteDualOk && m_blackDualOk;
}

void ChessWindow::beginDualConfirm(DualAction action)
{
    m_dualPending = true;
    m_dualAction = action;
    resetDualConfirm();
    m_whiteMenu.open = true;
    m_blackMenu.open = true;
    m_whiteMenu.overlay = PauseOverlayDualConfirm;
    m_blackMenu.overlay = PauseOverlayDualConfirm;
    m_whiteMenu.focusIndex = 0;
    m_blackMenu.focusIndex = 0;
}

void ChessWindow::cancelDualConfirm()
{
    m_dualPending = false;
    resetDualConfirm();
    m_whiteMenu.overlay = PauseOverlayMain;
    m_blackMenu.overlay = PauseOverlayMain;
}

void ChessWindow::applyDualAction()
{
    if (m_destroying) {
        return;
    }
    switch (m_dualAction) {
    case DualPause:
        m_dualPending = false;
        resetDualConfirm();
        m_matchPaused = true;
        closeMenuWhite();
        closeMenuBlack();
        break;
    case DualResume:
        m_dualPending = false;
        resetDualConfirm();
        m_matchPaused = false;
        closeMenuWhite();
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

void ChessWindow::registerDualOkWhite()
{
    if (!m_dualPending) {
        return;
    }
    m_whiteDualOk = true;
    if (dualConfirmSatisfied()) {
        applyDualAction();
    }
}

void ChessWindow::registerDualOkBlack()
{
    if (!m_dualPending) {
        return;
    }
    m_blackDualOk = true;
    if (dualConfirmSatisfied()) {
        applyDualAction();
    }
}

void ChessWindow::trySurrender(ChessEngine::Side side)
{
    m_engine.resign(side);
    m_gameState = GAME_OVER;
    closeAllMenus();
}

void ChessWindow::activateMenuSelection(bool whiteSide)
{
    SideMenu *menu = whiteSide ? &m_whiteMenu : &m_blackMenu;
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
        } else if (whiteSide) {
            closeMenuWhite();
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
            trySurrender(whiteSide ? ChessEngine::Side::White : ChessEngine::Side::Black);
        }
        break;
    }
}

void ChessWindow::menuSurrenderWhite()
{
    if (!m_whiteMenu.open || m_whiteMenu.overlay != PauseOverlayMain) {
        return;
    }
    const int idxSurrender = m_engine.isDrawByRule() ? 4 : 3;
    if (m_whiteMenu.focusIndex == idxSurrender) {
        trySurrender(ChessEngine::Side::White);
    } else {
        activateMenuSelection(true);
    }
}

void ChessWindow::menuSurrenderBlack()
{
    if (!m_blackMenu.open || m_blackMenu.overlay != PauseOverlayMain) {
        return;
    }
    const int idxSurrender = m_engine.isDrawByRule() ? 4 : 3;
    if (m_blackMenu.focusIndex == idxSurrender) {
        trySurrender(ChessEngine::Side::Black);
    } else {
        activateMenuSelection(false);
    }
}

void ChessWindow::p1MoveCursor(int dr, int dc)
{
    if (m_matchPaused || m_gameState != PLAYING
        || m_engine.sideToMove() != ChessEngine::Side::White) {
        return;
    }
    if (m_whiteMenu.open && !m_dualPending) {
        return;
    }
    m_cursorR = qBound(0, m_cursorR + dr, ChessEngine::kRows - 1);
    m_cursorC = qBound(0, m_cursorC + dc, ChessEngine::kCols - 1);
}

void ChessWindow::p2MoveCursor(int dr, int dc)
{
    if (m_matchPaused || m_gameState != PLAYING
        || m_engine.sideToMove() != ChessEngine::Side::Black) {
        return;
    }
    if (m_blackMenu.open && !m_dualPending) {
        return;
    }
    m_cursorR = qBound(0, m_cursorR + dr, ChessEngine::kRows - 1);
    m_cursorC = qBound(0, m_cursorC + dc, ChessEngine::kCols - 1);
}

void ChessWindow::p1Confirm()
{
    if (m_destroying) {
        return;
    }
    if (m_dualPending) {
        if (m_whiteMenu.focusIndex == 1) {
            cancelDualConfirm();
        } else {
            registerDualOkWhite();
        }
        return;
    }
    if (m_whiteMenu.open) {
        menuSurrenderWhite();
        return;
    }
    if (m_gameState == MENU) {
        startMatch();
        return;
    }
    if (m_matchPaused || m_gameState != PLAYING
        || m_engine.sideToMove() != ChessEngine::Side::White) {
        return;
    }

    if (m_pickPhase == PickPiece) {
        const ChessEngine::Piece p = m_engine.at(m_cursorR, m_cursorC);
        if (p == ChessEngine::Piece::Empty
            || ChessEngine::sideOf(p) != ChessEngine::Side::White) {
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

    for (const ChessEngine::Move &mv : m_highlights) {
        if (mv.tr == m_cursorR && mv.tc == m_cursorC) {
            if (m_engine.applyMove(mv)) {
                resetPick();
                restartThinkClock();
                if (m_engine.result() != ChessEngine::Result::Ongoing) {
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

void ChessWindow::p2Confirm()
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
        || m_engine.sideToMove() != ChessEngine::Side::Black) {
        return;
    }

    if (m_pickPhase == PickPiece) {
        const ChessEngine::Piece p = m_engine.at(m_cursorR, m_cursorC);
        if (p == ChessEngine::Piece::Empty
            || ChessEngine::sideOf(p) != ChessEngine::Side::Black) {
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

    for (const ChessEngine::Move &mv : m_highlights) {
        if (mv.tr == m_cursorR && mv.tc == m_cursorC) {
            if (m_engine.applyMove(mv)) {
                resetPick();
                restartThinkClock();
                if (m_engine.result() != ChessEngine::Result::Ongoing) {
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

void ChessWindow::pollJoyButtons()
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

void ChessWindow::tickJoyCursor()
{
    if (m_destroying || !isVisible() || m_gameState != PLAYING || m_matchPaused) {
        return;
    }
    if (!m_joyInput || !m_joyInput->serialConnected.load(std::memory_order_relaxed)) {
        return;
    }
    if (m_engine.sideToMove() != ChessEngine::Side::Black) {
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

void ChessWindow::drawBoard(QPainter &painter) const
{
    const QRect br = boardRect();
    painter.fillRect(br.adjusted(-10, -10, 10, 10), QColor(48, 44, 40));

    const QColor lightSq(240, 217, 181);
    const QColor darkSq(181, 136, 99);
    for (int r = 0; r < ChessEngine::kRows; ++r) {
        for (int c = 0; c < ChessEngine::kCols; ++c) {
            const QRect sq(br.left() + c * kCell, br.top() + r * kCell, kCell, kCell);
            painter.fillRect(sq, ((r + c) % 2 == 0) ? lightSq : darkSq);
        }
    }

    painter.setPen(QColor(90, 70, 50));
    QFont coordFont(QStringLiteral("Segoe UI"), 9);
    painter.setFont(coordFont);
    for (int c = 0; c < ChessEngine::kCols; ++c) {
        const QChar file = QChar('a' + c);
        const QRect bottom(br.left() + c * kCell, br.bottom() - 16, kCell, 14);
        const QRect top(br.left() + c * kCell, br.top() + 2, kCell, 14);
        painter.drawText(bottom, Qt::AlignHCenter | Qt::AlignTop, QString(file));
        painter.drawText(top, Qt::AlignHCenter | Qt::AlignTop, QString(file));
    }
    for (int r = 0; r < ChessEngine::kRows; ++r) {
        const QString rank = QString::number(ChessEngine::kRows - r);
        const QRect left(br.left() + 4, br.top() + r * kCell, 14, kCell);
        const QRect right(br.right() - 18, br.top() + r * kCell, 14, kCell);
        painter.drawText(left, Qt::AlignVCenter | Qt::AlignLeft, rank);
        painter.drawText(right, Qt::AlignVCenter | Qt::AlignRight, rank);
    }
}

void ChessWindow::drawHighlights(QPainter &painter) const
{
    const ChessEngine::Side mover = m_engine.sideToMove();

    for (const ChessEngine::Move &mv : m_highlights) {
        const ChessEngine::Piece tgt = m_engine.at(mv.tr, mv.tc);
        const bool isCapture =
            tgt != ChessEngine::Piece::Empty && ChessEngine::sideOf(tgt) != mover;
        if (!isCapture) {
            painter.setBrush(QColor(80, 220, 120, 160));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(squareCenter(mv.tr, mv.tc), 10, 10);
        }
    }

    if (m_selR >= 0) {
        const QPoint selPt = squareCenter(m_selR, m_selC);
        painter.setPen(QPen(QColor(60, 220, 90), 4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(selPt, 28, 28);
    }

    for (const ChessEngine::Move &mv : m_highlights) {
        const ChessEngine::Piece tgt = m_engine.at(mv.tr, mv.tc);
        if (tgt == ChessEngine::Piece::Empty
            || ChessEngine::sideOf(tgt) == mover) {
            continue;
        }
        const QPoint capPt = squareCenter(mv.tr, mv.tc);
        painter.setPen(QPen(QColor(255, 50, 50), 4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(capPt, 28, 28);
    }

    const bool cursorOnSelection = m_selR >= 0 && m_cursorR == m_selR && m_cursorC == m_selC;
    if (!cursorOnSelection) {
        const QPoint cur = squareCenter(m_cursorR, m_cursorC);
        painter.setPen(QPen(QColor(80, 180, 255), 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(cur, 26, 26);
    }
}

void ChessWindow::drawPieces(QPainter &painter) const
{
    const QFont pf = chessPieceFont(static_cast<int>(kCell * 0.72));
    painter.setFont(pf);
    for (int r = 0; r < ChessEngine::kRows; ++r) {
        for (int c = 0; c < ChessEngine::kCols; ++c) {
            const ChessEngine::Piece p = m_engine.at(r, c);
            if (p == ChessEngine::Piece::Empty) {
                continue;
            }
            const QPoint pt = squareCenter(r, c);
            const QRect box(pt.x() - kCell / 2, pt.y() - kCell / 2, kCell, kCell);
            const bool white = ChessEngine::isWhite(p);
            painter.setPen(QPen(white ? QColor(25, 25, 30) : QColor(10, 10, 12), white ? 1 : 2));
            painter.drawText(box, Qt::AlignCenter, pieceGlyph(p));
        }
    }
}

void ChessWindow::drawHud(QPainter &painter) const
{
    painter.setPen(QColor(230, 235, 250));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 11));
    const QString side =
        (m_engine.sideToMove() == ChessEngine::Side::White) ? QStringLiteral("白方(P1 键盘)")
                                                            : QStringLiteral("黑方(P2 摇杆)");
    painter.drawText(12, 24, QStringLiteral("行棋: %1").arg(side));
    if (m_matchPaused) {
        painter.setPen(QColor(255, 200, 100));
        painter.drawText(12, 42, QStringLiteral("对局已暂停（双方确认后继续）"));
    } else if (m_gameState == PLAYING) {
        painter.drawText(12, 42, QStringLiteral("思考: %1 秒").arg(m_thinkRemain));
    }

    switch (m_engine.result()) {
    case ChessEngine::Result::WhiteWin:
        painter.setPen(QColor(255, 200, 100));
        painter.drawText(12, height() - 12, QStringLiteral("白方胜"));
        break;
    case ChessEngine::Result::BlackWin:
        painter.setPen(QColor(255, 200, 100));
        painter.drawText(12, height() - 12, QStringLiteral("黑方胜"));
        break;
    case ChessEngine::Result::Draw:
        painter.setPen(QColor(255, 200, 100));
        painter.drawText(12, height() - 12, QStringLiteral("和棋"));
        break;
    default:
        break;
    }
}

void ChessWindow::drawMenuButtons(QPainter &painter) const
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
    drawBtn(menuButtonRectWhite(), m_whiteMenu.open, QStringLiteral("白方 P1"));
}

void ChessWindow::drawCheckAlert(QPainter &painter, bool whiteSide) const
{
    if (m_gameState != PLAYING && m_gameState != GAME_OVER) {
        return;
    }
    const ChessEngine::Side side =
        whiteSide ? ChessEngine::Side::White : ChessEngine::Side::Black;
    if (!m_engine.isInCheck(side)) {
        return;
    }

    const QRect br = boardRect();
    QRect alertRect;
    if (whiteSide) {
        alertRect = QRect(br.right() + 12, br.bottom() - 58, kSidePanelW - 16, 52);
    } else {
        alertRect = QRect(br.left() - kSidePanelW + 4, br.top() + 4, kSidePanelW - 16, 52);
    }

    QFont f(QStringLiteral("KaiTi"), 48, QFont::Bold);
    painter.setFont(f);
    painter.setPen(QColor(255, 45, 45));
    painter.drawText(alertRect, Qt::AlignCenter, QStringLiteral("Check"));
}

void ChessWindow::drawSideMenu(QPainter &painter, bool whiteSide) const
{
    const SideMenu &menu = whiteSide ? m_whiteMenu : m_blackMenu;
    if (!menu.open) {
        return;
    }

    const QRect panel = sideMenuPanelRect(whiteSide);
    painter.setBrush(QColor(32, 38, 58, 240));
    painter.setPen(QPen(QColor(90, 120, 180), 2));
    painter.drawRoundedRect(panel, 8, 8);

    painter.setPen(QColor(240, 245, 255));
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 12, QFont::Bold));
    painter.drawText(panel.adjusted(0, 10, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                     whiteSide ? QStringLiteral("白方菜单") : QStringLiteral("黑方菜单"));

    auto drawBtn = [&](int row, const QString &text, bool focused) {
        const QRect br = sideMenuButtonRect(whiteSide, row);
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
                .arg(m_whiteDualOk ? QStringLiteral("已确认") : QStringLiteral("待确认"))
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

void ChessWindow::paintEvent(QPaintEvent *)
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
            QStringLiteral("国际象棋\n\n白方 P1：WASD + Enter（右侧中线「菜单」 ESC/P）\n"
                             "黑方 P2：摇杆 + A（左侧中线「菜单」 摇杆C/D）\n"
                             "选子绿圈 · 可吃子红圈 · 被将军一侧大字 Check\n"
                             "棋子为 Unicode 标准符号（♔♕♖等）\n思考 60 秒\n\n"
                             "空格/Enter 或 摇杆 A 开始"));
    }

    if (m_gameState == GAME_OVER && !m_whiteMenu.open && !m_blackMenu.open) {
        p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 14, QFont::Bold));
        p.setPen(QColor(255, 180, 120));
        p.drawText(boardRect(), Qt::AlignCenter, QStringLiteral("对局结束"));
    }
}

void ChessWindow::keyPressEvent(QKeyEvent *event)
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
            toggleMenuWhite();
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
        if (!repeat && m_whiteMenu.open) {
            cycleMenuFocus(&m_whiteMenu, -1);
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
        if (!repeat && m_whiteMenu.open) {
            cycleMenuFocus(&m_whiteMenu, 1);
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
        if (!repeat && m_gameState == PLAYING && !m_whiteMenu.open) {
            p1MoveCursor(0, -1);
        }
        break;
    case Qt::Key_D:
        m_keyD = true;
        if (!repeat && m_gameState == PLAYING && !m_whiteMenu.open) {
            p1MoveCursor(0, 1);
        }
        break;
    default:
        break;
    }
    update();
}

void ChessWindow::keyReleaseEvent(QKeyEvent *event)
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

void ChessWindow::mousePressEvent(QMouseEvent *event)
{
    if (!event || m_destroying) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint pt = event->position().toPoint();

    if (event->button() == Qt::LeftButton && menuButtonRectWhite().contains(pt)) {
        toggleMenuWhite();
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

    if (event->button() == Qt::LeftButton && m_whiteMenu.open) {
        const int nBtn = m_dualPending ? 2 : pauseMenuItemCount();
        for (int row = 0; row < nBtn; ++row) {
            if (sideMenuButtonRect(true, row).contains(pt)) {
                m_whiteMenu.focusIndex = row;
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

void ChessWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!event) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint pt = event->position().toPoint();
    if (menuButtonRectWhite().contains(pt) || menuButtonRectBlack().contains(pt)) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    unsetCursor();
    QWidget::mouseMoveEvent(event);
}

void ChessWindow::closeEvent(QCloseEvent *event)
{
    prepareForShutdown();
    QWidget::closeEvent(event);
}
