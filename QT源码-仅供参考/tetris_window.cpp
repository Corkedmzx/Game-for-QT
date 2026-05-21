#include "tetris_window.h"
#include "shared_joy_state.h"

#include <QMouseEvent>
#include <QCloseEvent>
#include <QDateTime>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRandomGenerator>
#include <QTimer>
#include <QtMath>

namespace {

static QColor colorForType(int t)
{
    static const QColor cols[8] = {
        QColor(), QColor(0, 240, 240), QColor(240, 240, 0), QColor(160, 0, 240),
        QColor(0, 240, 0), QColor(240, 0, 0), QColor(0, 0, 240), QColor(240, 160, 0)};
    if (t < 1 || t > 7) {
        return QColor(200, 200, 200);
    }
    return cols[t];
}

/** 七种四连方块：每种 4 旋转，4 个格子相对 4×4 左上角的偏移（与常见 SRS 4×4 表示一致） */
static void tetrominoCells(int type, int rot, int ox[4], int oy[4])
{
    struct Pt {
        int x, y;
    };
    static const Pt T[7][4][4] = {
        // I
        {{{1, 0}, {1, 1}, {1, 2}, {1, 3}},
         {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
         {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
         {{0, 1}, {1, 1}, {2, 1}, {3, 1}}},
        // O
        {{{1, 1}, {1, 2}, {2, 1}, {2, 2}},
         {{1, 1}, {1, 2}, {2, 1}, {2, 2}},
         {{1, 1}, {1, 2}, {2, 1}, {2, 2}},
         {{1, 1}, {1, 2}, {2, 1}, {2, 2}}},
        // T
        {
            {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
            {{1, 0}, {0, 1}, {1, 1}, {1, 2}}
        },
        // S
        {
            {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
            {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
            {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
            {{0, 0}, {0, 1}, {1, 1}, {1, 2}}
        },
        // Z
        {
            {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
            {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
            {{1, 0}, {0, 1}, {1, 1}, {0, 2}}
        },
        // J
        {
            {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
            {{1, 0}, {1, 1}, {0, 2}, {1, 2}}
        },
        // L
        {
            {{2, 0}, {0, 1}, {1, 1}, {2, 1}},
            {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
            {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
            {{0, 0}, {1, 0}, {1, 1}, {1, 2}}
        }
    };
    const int ti = qBound(0, type - 1, 6);
    const int ri = rot & 3;
    for (int i = 0; i < 4; ++i) {
        ox[i] = T[ti][ri][i].x;
        oy[i] = T[ti][ri][i].y;
    }
}

} // namespace

TetrisWindow::TetrisWindow(SharedJoyState *joyInput, QWidget *parent)
    : QWidget(parent)
    , m_joyInput(joyInput)
{
    m_grid.resize(kRows * kCols);
    m_grid.fill(0);

    setFixedSize(kCols * kCell + 160, kRows * kCell + 40);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    m_gameTimer = new QTimer(this);
    m_clearAnimTimer = new QTimer(this);
    m_inputTimer = new QTimer(this);
    m_axisTimer = new QTimer(this);

    connect(m_gameTimer, &QTimer::timeout, this, &TetrisWindow::tickGame);
    connect(m_clearAnimTimer, &QTimer::timeout, this, &TetrisWindow::tickLineClearAnim);
    m_clearAnimTimer->setInterval(45);
    connect(m_inputTimer, &QTimer::timeout, this, &TetrisWindow::pollJoyButtons);
    m_inputTimer->start(8);
    connect(m_axisTimer, &QTimer::timeout, this, &TetrisWindow::tickJoyAxes);
    m_axisTimer->start(28);

    initGame();
}

int TetrisWindow::gridAt(int row, int col) const
{
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) {
        return 0;
    }
    const int idx = row * kCols + col;
    if (idx < 0 || idx >= m_grid.size()) {
        return 0;
    }
    return m_grid.at(idx);
}

void TetrisWindow::setGridAt(int row, int col, int value)
{
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) {
        return;
    }
    const int idx = row * kCols + col;
    if (idx < 0 || idx >= m_grid.size()) {
        return;
    }
    m_grid[idx] = value;
}

TetrisWindow::~TetrisWindow()
{
    prepareForShutdown();
}

void TetrisWindow::prepareForShutdown()
{
    if (m_destroying) {
        return;
    }
    m_destroying = true;
    m_lineClearAnim = false;
    m_clearLineCount = 0;
    m_clearAnimStep = 0;
    if (m_gameTimer) {
        QObject::disconnect(m_gameTimer, nullptr, this, nullptr);
        m_gameTimer->stop();
    }
    if (m_clearAnimTimer) {
        QObject::disconnect(m_clearAnimTimer, nullptr, this, nullptr);
        m_clearAnimTimer->stop();
    }
    if (m_inputTimer) {
        QObject::disconnect(m_inputTimer, nullptr, this, nullptr);
        m_inputTimer->stop();
    }
    if (m_axisTimer) {
        QObject::disconnect(m_axisTimer, nullptr, this, nullptr);
        m_axisTimer->stop();
    }
    m_joyInput = nullptr;
}

void TetrisWindow::scheduleJoyEdgeResync()
{
    m_prevJoyButtons = m_joyInput ? m_joyInput->buttonMask.load(std::memory_order_relaxed) : 0;
    resetJoyStickDiscreteState();
    QTimer::singleShot(30, this, [this]() {
        if (m_destroying) {
            return;
        }
        m_prevJoyButtons = m_joyInput ? m_joyInput->buttonMask.load(std::memory_order_relaxed) : 0;
    });
}

void TetrisWindow::returnToLauncher()
{
    if (m_destroying) {
        return;
    }
    initGame();
    update();
}

void TetrisWindow::initGame()
{
    if (m_gameTimer) {
        m_gameTimer->stop();
    }
    if (m_clearAnimTimer) {
        m_clearAnimTimer->stop();
    }
    m_lineClearAnim = false;
    m_clearLineCount = 0;
    m_clearAnimStep = 0;
    if (m_grid.size() != kRows * kCols) {
        m_grid.resize(kRows * kCols);
    }
    m_grid.fill(0);
    m_gameState = MENU;
    m_pauseOverlay = PauseOverlayNone;
    m_pauseFocusIndex = 0;
    m_score = 0;
    m_lines = 0;
    m_dropIntervalMs = 800;
    if (m_grid.size() != kRows * kCols) {
        m_grid.resize(kRows * kCols);
    }
    m_grid.fill(0);
    m_curType = 0;
    m_curRot = 0;
    m_curX = 3;
    m_curY = 0;
    m_nextType = 1 + QRandomGenerator::global()->bounded(7);
    resetJoyStickDiscreteState();
    m_lastKbdVertMs = 0;
}

void TetrisWindow::resetJoyStickDiscreteState()
{
    m_joyLatchL = m_joyLatchR = m_joyLatchU = m_joyLatchD = false;
    m_joyDasL = m_joyDasR = 0;
    m_lastJoyVertMs = 0;
}

void TetrisWindow::resetGame()
{
    initGame();
    m_gameState = PLAYING;
    m_dropIntervalMs = 800;
    spawnPiece();
    if (m_gameTimer) {
        m_gameTimer->start(m_dropIntervalMs);
    }
}

void TetrisWindow::spawnPiece()
{
    m_curType = m_nextType;
    m_nextType = 1 + QRandomGenerator::global()->bounded(7);
    m_curRot = 0;
    m_curX = 3;
    bool placed = false;
    /* 先试 y=0，再略抬高（等价于隐藏行），避免堆到倒数第二行时顶行仍空却已判死 */
    for (int tryY : {0, -1, -2}) {
        if (pieceFits(m_curType, m_curRot, m_curX, tryY)) {
            m_curY = tryY;
            placed = true;
            break;
        }
    }
    if (!placed) {
        m_gameState = GAME_OVER;
        if (m_gameTimer) {
            m_gameTimer->stop();
        }
    }
}

bool TetrisWindow::pieceFits(int type, int rot, int px, int py) const
{
    int ox[4], oy[4];
    tetrominoCells(type, rot, ox, oy);
    for (int i = 0; i < 4; ++i) {
        const int x = px + ox[i];
        const int y = py + oy[i];
        if (x < 0 || x >= kCols || y >= kRows) {
            return false;
        }
        if (y >= 0 && gridAt(y, x) != 0) {
            return false;
        }
    }
    return true;
}

void TetrisWindow::lockPiece()
{
    int ox[4], oy[4];
    tetrominoCells(m_curType, m_curRot, ox, oy);
    for (int i = 0; i < 4; ++i) {
        const int x = m_curX + ox[i];
        const int y = m_curY + oy[i];
        if (y >= 0 && y < kRows && x >= 0 && x < kCols) {
            setGridAt(y, x, m_curType);
        }
    }
    if (findFullLines() > 0) {
        beginLineClearAnim();
    } else {
        spawnPiece();
    }
}

int TetrisWindow::findFullLines()
{
    m_clearLineCount = 0;
    for (int r = kRows - 1; r >= 0; --r) {
        bool full = true;
        for (int c = 0; c < kCols; ++c) {
            if (gridAt(r, c) == 0) {
                full = false;
                break;
            }
        }
        if (full && m_clearLineCount < kRows) {
            m_clearRows[m_clearLineCount++] = r;
        }
    }
    return m_clearLineCount;
}

void TetrisWindow::applyLineClear()
{
    if (m_clearLineCount <= 0) {
        return;
    }

    bool remove[kRows] = {};
    for (int i = 0; i < m_clearLineCount; ++i) {
        const int r = m_clearRows[i];
        if (r >= 0 && r < kRows) {
            remove[r] = true;
        }
    }

    int dst = kRows - 1;
    for (int src = kRows - 1; src >= 0; --src) {
        if (!remove[src]) {
            if (dst != src) {
                for (int c = 0; c < kCols; ++c) {
                    setGridAt(dst, c, gridAt(src, c));
                }
            }
            --dst;
        }
    }
    for (int r = dst; r >= 0; --r) {
        for (int c = 0; c < kCols; ++c) {
            setGridAt(r, c, 0);
        }
    }
}

void TetrisWindow::beginLineClearAnim()
{
    m_lineClearAnim = true;
    m_clearAnimStep = 0;
    if (m_gameTimer) {
        m_gameTimer->stop();
    }
    if (m_clearAnimTimer) {
        m_clearAnimTimer->start();
    }
    update();
}

void TetrisWindow::finishLineClearAnim()
{
    const int cleared = m_clearLineCount;
    applyLineClear();
    m_lineClearAnim = false;
    m_clearLineCount = 0;
    m_clearAnimStep = 0;
    if (m_clearAnimTimer) {
        m_clearAnimTimer->stop();
    }

    if (cleared > 0) {
        m_lines += cleared;
        m_score += cleared * cleared * 100;
        m_dropIntervalMs = qMax(120, 800 - (m_lines / 5) * 40);
    }
    spawnPiece();
    if (m_gameTimer && m_gameState == PLAYING) {
        m_gameTimer->start(m_dropIntervalMs);
    }
    update();
}

void TetrisWindow::tickLineClearAnim()
{
    if (m_destroying || !m_lineClearAnim || m_clearLineCount <= 0) {
        return;
    }
    ++m_clearAnimStep;
    const int totalSteps = kClearFlashSteps + kClearCollapseSteps;
    if (m_clearAnimStep >= totalSteps) {
        finishLineClearAnim();
        return;
    }
    update();
}

void TetrisWindow::tryShift(int dx, int dy)
{
    if (m_gameState != PLAYING || m_lineClearAnim) {
        return;
    }
    if (pieceFits(m_curType, m_curRot, m_curX + dx, m_curY + dy)) {
        m_curX += dx;
        m_curY += dy;
    }
}

void TetrisWindow::tryRotate()
{
    if (m_gameState != PLAYING || m_lineClearAnim) {
        return;
    }
    const int nr = (m_curRot + 1) & 3;
    if (pieceFits(m_curType, nr, m_curX, m_curY)) {
        m_curRot = nr;
        return;
    }
    for (int kick : {-1, 1, -2, 2}) {
        if (pieceFits(m_curType, nr, m_curX + kick, m_curY)) {
            m_curX += kick;
            m_curRot = nr;
            return;
        }
    }
}

void TetrisWindow::hardDrop()
{
    if (m_gameState != PLAYING || m_lineClearAnim) {
        return;
    }
    while (pieceFits(m_curType, m_curRot, m_curX, m_curY + 1)) {
        ++m_curY;
    }
    lockPiece();
}

void TetrisWindow::tickGame()
{
    if (m_destroying || m_gameState != PLAYING || m_lineClearAnim) {
        return;
    }
    if (pieceFits(m_curType, m_curRot, m_curX, m_curY + 1)) {
        ++m_curY;
    } else {
        lockPiece();
    }
    update();
}

void TetrisWindow::openPauseMenu()
{
    if (m_gameState != PLAYING || m_lineClearAnim) {
        return;
    }
    m_gameState = PAUSED;
    m_pauseOverlay = PauseOverlayMain;
    m_pauseFocusIndex = 0;
    if (m_gameTimer) {
        m_gameTimer->stop();
    }
    resetJoyStickDiscreteState();
}

void TetrisWindow::resumeFromPause()
{
    if (m_gameState != PAUSED) {
        return;
    }
    resetJoyStickDiscreteState();
    m_gameState = PLAYING;
    m_pauseOverlay = PauseOverlayNone;
    unsetCursor();
    if (m_gameTimer) {
        m_gameTimer->start(m_dropIntervalMs);
    }
}

void TetrisWindow::handleEscapeKey()
{
    if (m_gameState == MENU || m_gameState == GAME_OVER) {
        emit requestReturnToSetup();
        return;
    }
    if (m_gameState == PLAYING) {
        openPauseMenu();
    } else if (m_gameState == PAUSED) {
        if (m_pauseOverlay == PauseOverlayExitConfirm) {
            m_pauseOverlay = PauseOverlayMain;
            m_pauseFocusIndex = 1;
        } else if (m_pauseOverlay == PauseOverlayMain) {
            resumeFromPause();
        }
    }
}

void TetrisWindow::cyclePauseFocus()
{
    if (m_pauseOverlay == PauseOverlayNone) {
        return;
    }
    m_pauseFocusIndex = (m_pauseFocusIndex + 1) % 2;
}

void TetrisWindow::activatePauseSelection()
{
    if (m_gameState != PAUSED || m_pauseOverlay == PauseOverlayNone) {
        return;
    }
    if (m_pauseOverlay == PauseOverlayMain) {
        if (m_pauseFocusIndex == 0) {
            resumeFromPause();
        } else {
            m_pauseOverlay = PauseOverlayExitConfirm;
            m_pauseFocusIndex = 0;
        }
    } else if (m_pauseOverlay == PauseOverlayExitConfirm) {
        if (m_pauseFocusIndex == 0) {
            emit requestReturnToSetup();
            m_pauseOverlay = PauseOverlayNone;
        } else {
            m_pauseOverlay = PauseOverlayMain;
            m_pauseFocusIndex = 1;
        }
    }
}

QRect TetrisWindow::pauseOverlayPanelRect() const
{
    const int panelW = 400;
    const int panelH = (m_pauseOverlay == PauseOverlayExitConfirm) ? 260 : 268;
    return QRect((width() - panelW) / 2, (height() - panelH) / 2, panelW, panelH);
}

QRect TetrisWindow::pauseOverlayButtonRect(int row) const
{
    const QRect panel = pauseOverlayPanelRect();
    const int btnLeft = panel.left() + 36;
    const int btnW = panel.width() - 72;
    const int btnH = 42;
    const int gap = 12;
    const int startRowY =
        (m_pauseOverlay == PauseOverlayExitConfirm) ? panel.top() + 138 : panel.top() + 88;
    return QRect(btnLeft, startRowY + row * (btnH + gap), btnW, btnH);
}

void TetrisWindow::drawPauseOverlay(QPainter &painter)
{
    auto uiFont = [](bool bold, int pointSize) {
        QFont f(QStringLiteral("Microsoft YaHei UI"), pointSize);
        f.setBold(bold);
        return f;
    };

    painter.fillRect(rect(), QColor(0, 0, 0, 175));

    const QRect panel = pauseOverlayPanelRect();

    painter.setPen(QPen(QColor(0, 180, 255), 2));
    painter.setBrush(QColor(16, 20, 44));
    painter.drawRoundedRect(panel, 14, 14);

    painter.setPen(QColor(255, 215, 90));
    painter.setFont(uiFont(true, 18));
    painter.drawText(panel.adjusted(0, 16, 0, 0), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("暂停"));

    painter.setPen(QColor(160, 190, 220));
    painter.setFont(uiFont(false, 10));
    const QString hint =
        (m_pauseOverlay == PauseOverlayExitConfirm)
            ? QStringLiteral("鼠标点击按钮　↑↓ / 摇杆 B 切换　Enter / 摇杆 A 确认　ESC / C / D 返回上一级")
            : QStringLiteral("鼠标点击按钮　↑↓ / 摇杆 B 切换　Enter / 摇杆 A 确认　ESC / C / D 继续游戏");
    painter.drawText(panel.adjusted(14, 48, -14, 0), Qt::AlignHCenter | Qt::AlignTop, hint);

    auto drawBtnRow = [&](int row, const QString &text, bool highlight) {
        const QRect br = pauseOverlayButtonRect(row);
        painter.setBrush(highlight ? QColor(36, 52, 92) : QColor(26, 30, 56));
        painter.setPen(QPen(highlight ? QColor(255, 210, 70) : QColor(70, 110, 150), highlight ? 3 : 1));
        painter.drawRoundedRect(br, 10, 10);
        painter.setPen(highlight ? QColor(255, 245, 220) : QColor(210, 225, 245));
        painter.setFont(uiFont(true, 11));
        painter.drawText(br, Qt::AlignCenter, text);
    };

    if (m_pauseOverlay == PauseOverlayMain) {
        drawBtnRow(0, QStringLiteral("继续游戏"), m_pauseFocusIndex == 0);
        drawBtnRow(1, QStringLiteral("返回游戏首页"), m_pauseFocusIndex == 1);
    } else {
        painter.setPen(QColor(230, 235, 250));
        painter.setFont(uiFont(false, 11));
        {
            QRect tr = panel.adjusted(24, 78, -24, -100).normalized();
            if (tr.isValid() && tr.height() > 4) {
                painter.drawText(tr, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                                 QStringLiteral("将返回主窗口游戏首页并清空当前进度，是否确定？"));
            }
        }
        drawBtnRow(0, QStringLiteral("确定"), m_pauseFocusIndex == 0);
        drawBtnRow(1, QStringLiteral("取消"), m_pauseFocusIndex == 1);
    }
}

void TetrisWindow::pollJoyButtons()
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

    if ((m_gameState == MENU || m_gameState == GAME_OVER) && (rising & JoyMask::A)) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastJoyMenuActionMs >= 400) {
            m_lastJoyMenuActionMs = now;
            resetGame();
        }
        update();
        return;
    }

    if (m_gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
        if (rising & JoyMask::B) {
            cyclePauseFocus();
            update();
            return;
        }
        if (rising & JoyMask::A) {
            activatePauseSelection();
            update();
            return;
        }
        if ((rising & JoyMask::C) || (rising & JoyMask::D)) {
            if (m_pauseOverlay == PauseOverlayMain) {
                resumeFromPause();
            } else if (m_pauseOverlay == PauseOverlayExitConfirm) {
                m_pauseOverlay = PauseOverlayMain;
                m_pauseFocusIndex = 1;
            }
            update();
            return;
        }
    }

    if ((rising & (JoyMask::C | JoyMask::D)) && m_gameState == PLAYING) {
        openPauseMenu();
        update();
        return;
    }

    if ((rising & JoyMask::D) && (m_gameState == MENU || m_gameState == GAME_OVER)) {
        emit requestReturnToSetup();
        update();
        return;
    }
}

void TetrisWindow::tickJoyAxes()
{
    if (m_destroying || !isVisible() || m_gameState != PLAYING || m_lineClearAnim) {
        return;
    }
    if (!m_joyInput || !m_joyInput->serialConnected.load(std::memory_order_relaxed)) {
        return;
    }

    const float nx = m_joyInput->nx.load(std::memory_order_relaxed);
    const float ny = m_joyInput->ny.load(std::memory_order_relaxed);

    constexpr float kOn = 0.50f;
    constexpr float kOff = 0.24f;
    constexpr float kCross = 0.34f;
    constexpr float kTiny = 0.10f;
    constexpr int kDasInit = 16;
    constexpr int kDasRep = 7;

    if (qMax(qAbs(nx), qAbs(ny)) < kTiny) {
        resetJoyStickDiscreteState();
        return;
    }

    /* 主轴需明显占优，避免在斜向附近每帧在「横/竖」间抖动导致上下误触 */
    const float nx2 = nx * nx;
    const float ny2 = ny * ny;
    constexpr float kAxisSep = 1.58f;
    const bool horizDominant = nx2 >= ny2 * kAxisSep;
    const bool vertDominant = ny2 >= nx2 * kAxisSep;
    auto crossH = [&]() { return qAbs(ny) < kCross; };
    auto crossV = [&]() { return qAbs(nx) < kCross; };

    auto stepDasLR = [&](bool want, bool &latch, int &das, int dx) {
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
            tryShift(dx, 0);
            return true;
        }
        ++das;
        if (das > kDasInit && ((das - kDasInit - 1) % kDasRep == 0)) {
            tryShift(dx, 0);
            return true;
        }
        return false;
    };

    bool moved = false;
    if (horizDominant) {
        m_joyLatchU = m_joyLatchD = false;
        const bool wL = (nx <= (m_joyLatchL ? -kOff : -kOn)) && crossH();
        const bool wR = (nx >= (m_joyLatchR ? kOff : kOn)) && crossH();
        if (stepDasLR(wL, m_joyLatchL, m_joyDasL, -1)) {
            moved = true;
        }
        if (stepDasLR(wR, m_joyLatchR, m_joyDasR, 1)) {
            moved = true;
        }
    } else if (vertDominant) {
        m_joyLatchL = m_joyLatchR = false;
        m_joyDasL = m_joyDasR = 0;
        /* ny：上为正；按推杆符号只处理一侧，避免斜向噪声同一帧内既旋转又硬降 */
        const bool wU = (ny >= (m_joyLatchU ? kOff : kOn)) && crossV();
        const bool wD = (ny <= (m_joyLatchD ? -kOff : -kOn)) && crossV();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        constexpr qint64 kVertMinGapMs = 260;
        if (ny >= 0.f) {
            m_joyLatchD = false;
            if (!wU) {
                m_joyLatchU = false;
            } else if (!m_joyLatchU) {
                if (now - m_lastJoyVertMs >= kVertMinGapMs) {
                    m_joyLatchU = true;
                    tryRotate();
                    m_lastJoyVertMs = now;
                    moved = true;
                }
            }
        } else {
            m_joyLatchU = false;
            if (!wD) {
                m_joyLatchD = false;
            } else if (!m_joyLatchD) {
                if (now - m_lastJoyVertMs >= kVertMinGapMs) {
                    m_joyLatchD = true;
                    hardDrop();
                    m_lastJoyVertMs = now;
                    moved = true;
                }
            }
        }
    } else {
        resetJoyStickDiscreteState();
        return;
    }

    if (moved) {
        update();
    }
}

void TetrisWindow::drawBoardCells(QPainter &p, int ox, int oy) const
{
    auto rowIsClearing = [&](int r) {
        if (!m_lineClearAnim) {
            return false;
        }
        for (int i = 0; i < m_clearLineCount; ++i) {
            if (m_clearRows[i] == r) {
                return true;
            }
        }
        return false;
    };

    const bool inFlash = m_lineClearAnim && m_clearAnimStep < kClearFlashSteps;
    const bool inCollapse =
        m_lineClearAnim && m_clearAnimStep >= kClearFlashSteps
        && m_clearAnimStep < kClearFlashSteps + kClearCollapseSteps;
    const bool flashHighlight = inFlash && (m_clearAnimStep % 2 == 0);

    int collapseOffset = 0;
    if (inCollapse) {
        const int cs = m_clearAnimStep - kClearFlashSteps;
        collapseOffset = (cs * kCell) / kClearCollapseSteps;
    }

    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const int t = gridAt(r, c);
            if (t == 0) {
                continue;
            }
            int cellY = oy + r * kCell + 1;
            if (rowIsClearing(r) && inCollapse) {
                cellY += collapseOffset;
            }
            QColor fill = colorForType(t);
            if (rowIsClearing(r) && inFlash) {
                fill = flashHighlight ? QColor(255, 255, 255) : fill.lighter(155);
            } else if (rowIsClearing(r) && inCollapse) {
                fill = fill.lighter(125);
            }
            const int x = ox + c * kCell + 1;
            p.fillRect(x, cellY, kCell - 2, kCell - 2, fill);
            if (rowIsClearing(r) && flashHighlight) {
                p.setPen(QPen(QColor(255, 248, 180), 1));
                p.drawRect(x, cellY, kCell - 2, kCell - 2);
            }
        }
    }

    if (inFlash && flashHighlight) {
        p.setPen(QPen(QColor(255, 255, 255, 90), 2));
        for (int i = 0; i < m_clearLineCount; ++i) {
            const int r = m_clearRows[i];
            p.drawLine(ox, oy + r * kCell + kCell / 2, ox + kCols * kCell,
                       oy + r * kCell + kCell / 2);
        }
    }
}

void TetrisWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(18, 22, 38));
    p.setPen(QColor(200, 210, 230));
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 11));

    const int ox = 20;
    const int oy = 20;
    const int bw = kCols * kCell;
    const int bh = kRows * kCell;

    p.setPen(QPen(QColor(80, 100, 140), 1));
    for (int c = 0; c <= kCols; ++c) {
        p.drawLine(ox + c * kCell, oy, ox + c * kCell, oy + bh);
    }
    for (int r = 0; r <= kRows; ++r) {
        p.drawLine(ox, oy + r * kCell, ox + bw, oy + r * kCell);
    }

    drawBoardCells(p, ox, oy);

    if ((m_gameState == PLAYING || m_gameState == PAUSED) && !m_lineClearAnim) {
        int cx[4], cy[4];
        tetrominoCells(m_curType, m_curRot, cx, cy);
        for (int i = 0; i < 4; ++i) {
            const int c = m_curX + cx[i];
            const int r = m_curY + cy[i];
            if (r >= 0) {
                p.fillRect(ox + c * kCell + 1, oy + r * kCell + 1, kCell - 2, kCell - 2,
                           colorForType(m_curType));
            }
        }
    }

    const int sx = ox + bw + 16;
    p.setPen(QColor(220, 230, 250));
    p.drawText(sx, oy + 24, QStringLiteral("分数: %1").arg(m_score));
    p.drawText(sx, oy + 50, QStringLiteral("消行: %1").arg(m_lines));
    p.drawText(sx, oy + 100, QStringLiteral("下一个:"));
    {
        int nx[4], ny[4];
        tetrominoCells(m_nextType, 0, nx, ny);
        for (int i = 0; i < 4; ++i) {
            p.fillRect(sx + nx[i] * 18, oy + 110 + ny[i] * 18, 16, 16, colorForType(m_nextType));
        }
    }

    if (m_gameState == MENU) {
        p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 20, QFont::Bold));
        p.setPen(QColor(255, 220, 120));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("俄罗斯方块\n\n空格 / 摇杆 A 开始\nESC / 摇杆 D 返回首页"));
    } else if (m_gameState == GAME_OVER) {
        p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 18, QFont::Bold));
        p.setPen(QColor(255, 120, 120));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("游戏结束　分数 %1\n\nR / 摇杆 A 重来　ESC / 摇杆 D 返回首页").arg(m_score));
    }

    if (m_gameState == PAUSED) {
        drawPauseOverlay(p);
    }
}

void TetrisWindow::keyPressEvent(QKeyEvent *event)
{
    if (!isVisible()) {
        event->ignore();
        return;
    }
    const bool noKbdRepeat = m_gameState == PLAYING && event->isAutoRepeat();
    switch (event->key()) {
    case Qt::Key_Escape:
        handleEscapeKey();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
            activatePauseSelection();
        }
        break;
    case Qt::Key_Up:
        if (m_gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
            cyclePauseFocus();
        }
        break;
    case Qt::Key_Down:
        if (m_gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
            cyclePauseFocus();
        }
        break;
    case Qt::Key_A:
        m_keyA = true;
        if (m_gameState == PLAYING && !noKbdRepeat) {
            tryShift(-1, 0);
        }
        break;
    case Qt::Key_D:
        m_keyD = true;
        if (m_gameState == PLAYING && !noKbdRepeat) {
            tryShift(1, 0);
        }
        break;
    case Qt::Key_W:
        m_keyW = true;
        if (m_gameState == PLAYING && !noKbdRepeat) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastKbdVertMs >= 100) {
                m_lastKbdVertMs = now;
                tryRotate();
            }
        }
        break;
    case Qt::Key_S:
        m_keyS = true;
        if (m_gameState == PLAYING && !noKbdRepeat) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastKbdVertMs >= 100) {
                m_lastKbdVertMs = now;
                hardDrop();
            }
        }
        break;
    case Qt::Key_Space:
        if (m_gameState == MENU || m_gameState == GAME_OVER) {
            resetGame();
        }
        break;
    case Qt::Key_P:
        handleEscapeKey();
        break;
    case Qt::Key_R:
        if (m_gameState == GAME_OVER) {
            resetGame();
        }
        break;
    default:
        break;
    }
    update();
}

void TetrisWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (!isVisible()) {
        event->ignore();
        return;
    }
    switch (event->key()) {
    case Qt::Key_A:
        m_keyA = false;
        break;
    case Qt::Key_D:
        m_keyD = false;
        break;
    case Qt::Key_W:
        m_keyW = false;
        break;
    case Qt::Key_S:
        m_keyS = false;
        break;
    default:
        break;
    }
}

void TetrisWindow::mousePressEvent(QMouseEvent *event)
{
    if (!event) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton && m_gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
        const QPoint pt = event->position().toPoint();
        for (int row = 0; row < 2; ++row) {
            if (pauseOverlayButtonRect(row).contains(pt)) {
                m_pauseFocusIndex = row;
                activatePauseSelection();
                event->accept();
                update();
                return;
            }
        }
        const QRect panel = pauseOverlayPanelRect();
        if (!panel.contains(pt)) {
            if (m_pauseOverlay == PauseOverlayExitConfirm) {
                m_pauseOverlay = PauseOverlayMain;
                m_pauseFocusIndex = 1;
            } else {
                resumeFromPause();
            }
            event->accept();
            update();
            return;
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (m_gameState == MENU || m_gameState == GAME_OVER) {
            resetGame();
            event->accept();
            update();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void TetrisWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!event) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (m_gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
        const QPoint pt = event->position().toPoint();
        bool onBtn = false;
        for (int r = 0; r < 2; ++r) {
            if (pauseOverlayButtonRect(r).contains(pt)) {
                onBtn = true;
                break;
            }
        }
        setCursor(onBtn ? Qt::PointingHandCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    unsetCursor();
    QWidget::mouseMoveEvent(event);
}

void TetrisWindow::closeEvent(QCloseEvent *event)
{
    prepareForShutdown();
    QWidget::closeEvent(event);
}
