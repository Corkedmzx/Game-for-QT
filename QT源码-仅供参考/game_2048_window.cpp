#include "game_2048_window.h"
#include "shared_joy_state.h"

#include <QList>
#include <QCloseEvent>
#include <QDateTime>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRandomGenerator>
#include <QTimer>
#include <QtMath>

namespace {

static QColor tileColor(int v)
{
    if (v <= 0) {
        return QColor(58, 62, 82);
    }
    if (v <= 4) {
        return QColor(120, 140, 180);
    }
    if (v <= 16) {
        return QColor(90, 160, 220);
    }
    if (v <= 64) {
        return QColor(70, 200, 160);
    }
    if (v <= 256) {
        return QColor(240, 190, 90);
    }
    if (v <= 1024) {
        return QColor(240, 130, 70);
    }
    return QColor(240, 70, 90);
}

} // namespace

Game2048Window::Game2048Window(SharedJoyState *joyInput, QWidget *parent)
    : QWidget(parent)
    , m_joyInput(joyInput)
    , m_inputTimer(new QTimer(this))
    , m_axisTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("Game2048Window"));
    const int side = kSize * kTilePx + 48;
    setFixedSize(side + 180, side + 40);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    connect(m_inputTimer, &QTimer::timeout, this, &Game2048Window::pollJoyButtons);
    m_inputTimer->start(8);
    connect(m_axisTimer, &QTimer::timeout, this, &Game2048Window::tickJoyAxes);
    m_axisTimer->start(38);

    initGame();
}

Game2048Window::~Game2048Window()
{
    prepareForShutdown();
}

void Game2048Window::prepareForShutdown()
{
    if (m_destroying) {
        return;
    }
    m_destroying = true;
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

void Game2048Window::scheduleJoyEdgeResync()
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

void Game2048Window::resetJoyStickDiscreteState()
{
    m_joyLatchU = m_joyLatchD = m_joyLatchL = m_joyLatchR = false;
    m_joyDasU = m_joyDasD = m_joyDasL = m_joyDasR = 0;
}

void Game2048Window::returnToLauncher()
{
    if (m_destroying) {
        return;
    }
    initGame();
    update();
}

void Game2048Window::initGame()
{
    m_gameState = MENU;
    m_pauseOverlay = PauseOverlayNone;
    m_pauseFocusIndex = 0;
    m_score = 0;
    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            m_board[r][c] = 0;
        }
    }
    resetJoyStickDiscreteState();
    m_lastKbdVertMs = 0;
}

void Game2048Window::resetGame()
{
    initGame();
    m_gameState = PLAYING;
    addRandomTile();
    addRandomTile();
}

void Game2048Window::addRandomTile()
{
    QList<QPair<int, int>> empty;
    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            if (m_board[r][c] == 0) {
                empty.append({r, c});
            }
        }
    }
    if (empty.isEmpty()) {
        return;
    }
    const int i = QRandomGenerator::global()->bounded(empty.size());
    const int v = (QRandomGenerator::global()->bounded(10) == 0) ? 4 : 2;
    m_board[empty.at(i).first][empty.at(i).second] = v;
}

bool Game2048Window::canMoveAny() const
{
    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            if (m_board[r][c] == 0) {
                return true;
            }
            const int v = m_board[r][c];
            if (r + 1 < kSize && m_board[r + 1][c] == v) {
                return true;
            }
            if (c + 1 < kSize && m_board[r][c + 1] == v) {
                return true;
            }
            if (r > 0 && m_board[r - 1][c] == v) {
                return true;
            }
            if (c > 0 && m_board[r][c - 1] == v) {
                return true;
            }
        }
    }
    return false;
}

int Game2048Window::maxTileValue() const
{
    int m = 0;
    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            m = qMax(m, m_board[r][c]);
        }
    }
    return m;
}

bool Game2048Window::tryMove(int dir)
{
    if (m_gameState != PLAYING) {
        return false;
    }
    int nb[kSize][kSize];
    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            nb[r][c] = m_board[r][c];
        }
    }

    auto mergeLine = [&](int *line, int len) {
        int tmp[8];
        int n = 0;
        for (int i = 0; i < len; ++i) {
            if (line[i] != 0) {
                tmp[n++] = line[i];
            }
        }
        int w = 0;
        for (int i = 0; i < n; ++i) {
            if (i + 1 < n && tmp[i] == tmp[i + 1]) {
                line[w++] = tmp[i] * 2;
                m_score += line[w - 1];
                ++i;
            } else {
                line[w++] = tmp[i];
            }
        }
        while (w < len) {
            line[w++] = 0;
        }
    };

    bool changed = false;

    if (dir == 0) { // up
        for (int c = 0; c < kSize; ++c) {
            int col[kSize];
            for (int r = 0; r < kSize; ++r) {
                col[r] = nb[r][c];
            }
            int before[kSize];
            for (int r = 0; r < kSize; ++r) {
                before[r] = col[r];
            }
            mergeLine(col, kSize);
            for (int r = 0; r < kSize; ++r) {
                if (col[r] != before[r]) {
                    changed = true;
                }
                nb[r][c] = col[r];
            }
        }
    } else if (dir == 2) { // down
        for (int c = 0; c < kSize; ++c) {
            int col[kSize];
            for (int r = 0; r < kSize; ++r) {
                col[kSize - 1 - r] = nb[r][c];
            }
            mergeLine(col, kSize);
            for (int r = 0; r < kSize; ++r) {
                const int nv = col[kSize - 1 - r];
                if (nv != nb[r][c]) {
                    changed = true;
                }
                nb[r][c] = nv;
            }
        }
    } else if (dir == 3) { // left
        for (int r = 0; r < kSize; ++r) {
            int row[kSize];
            for (int c = 0; c < kSize; ++c) {
                row[c] = nb[r][c];
            }
            int before[kSize];
            for (int c = 0; c < kSize; ++c) {
                before[c] = row[c];
            }
            mergeLine(row, kSize);
            for (int c = 0; c < kSize; ++c) {
                if (row[c] != before[c]) {
                    changed = true;
                }
                nb[r][c] = row[c];
            }
        }
    } else if (dir == 1) { // right
        for (int r = 0; r < kSize; ++r) {
            int row[kSize];
            for (int c = 0; c < kSize; ++c) {
                row[kSize - 1 - c] = nb[r][c];
            }
            mergeLine(row, kSize);
            for (int c = 0; c < kSize; ++c) {
                const int nv = row[kSize - 1 - c];
                if (nv != nb[r][c]) {
                    changed = true;
                }
                nb[r][c] = nv;
            }
        }
    }

    if (!changed) {
        return false;
    }
    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            m_board[r][c] = nb[r][c];
        }
    }
    addRandomTile();
    if (!canMoveAny()) {
        m_gameState = GAME_OVER;
    }
    return true;
}

void Game2048Window::openPauseMenu()
{
    if (m_gameState != PLAYING) {
        return;
    }
    m_gameState = PAUSED;
    m_pauseOverlay = PauseOverlayMain;
    m_pauseFocusIndex = 0;
    resetJoyStickDiscreteState();
}

void Game2048Window::resumeFromPause()
{
    if (m_gameState != PAUSED) {
        return;
    }
    resetJoyStickDiscreteState();
    m_gameState = PLAYING;
    m_pauseOverlay = PauseOverlayNone;
    unsetCursor();
}

void Game2048Window::handleEscapeKey()
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

void Game2048Window::cyclePauseFocus()
{
    if (m_pauseOverlay == PauseOverlayNone) {
        return;
    }
    m_pauseFocusIndex = (m_pauseFocusIndex + 1) % 2;
}

void Game2048Window::activatePauseSelection()
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

QRect Game2048Window::pauseOverlayPanelRect() const
{
    const int panelW = 400;
    const int panelH = (m_pauseOverlay == PauseOverlayExitConfirm) ? 260 : 268;
    return QRect((width() - panelW) / 2, (height() - panelH) / 2, panelW, panelH);
}

QRect Game2048Window::pauseOverlayButtonRect(int row) const
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

void Game2048Window::drawPauseOverlay(QPainter &painter)
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

void Game2048Window::pollJoyButtons()
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

void Game2048Window::tickJoyAxes()
{
    if (m_destroying || !isVisible() || m_gameState != PLAYING) {
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
    constexpr int kDasInitH = 16;
    constexpr int kDasRepH = 7;
    constexpr int kDasInitV = 38;
    constexpr int kDasRepV = 15;

    if (qMax(qAbs(nx), qAbs(ny)) < kTiny) {
        resetJoyStickDiscreteState();
        return;
    }

    const float nx2 = nx * nx;
    const float ny2 = ny * ny;
    constexpr float kAxisSep = 1.58f;
    const bool horizDominant = nx2 >= ny2 * kAxisSep;
    const bool vertDominant = ny2 >= nx2 * kAxisSep;
    auto crossH = [&]() { return qAbs(ny) < kCross; };
    auto crossV = [&]() { return qAbs(nx) < kCross; };

    auto stepDas = [&](bool want, bool &latch, int &das, int dir, int di, int dr) {
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
            tryMove(dir);
            return true;
        }
        ++das;
        if (das > di && ((das - di - 1) % dr == 0)) {
            tryMove(dir);
            return true;
        }
        return false;
    };

    bool moved = false;
    if (horizDominant) {
        m_joyLatchU = m_joyLatchD = false;
        m_joyDasU = m_joyDasD = 0;
        const bool wL = (nx <= (m_joyLatchL ? -kOff : -kOn)) && crossH();
        const bool wR = (nx >= (m_joyLatchR ? kOff : kOn)) && crossH();
        if (stepDas(wL, m_joyLatchL, m_joyDasL, 3, kDasInitH, kDasRepH)) {
            moved = true;
        }
        if (stepDas(wR, m_joyLatchR, m_joyDasR, 1, kDasInitH, kDasRepH)) {
            moved = true;
        }
    } else if (vertDominant) {
        m_joyLatchL = m_joyLatchR = false;
        m_joyDasL = m_joyDasR = 0;
        /* ny：上为正；每帧只处理上或下之一，避免斜向抖动同一 tick 内连发两步 */
        const bool wU = (ny >= (m_joyLatchU ? kOff : kOn)) && crossV();
        const bool wD = (ny <= (m_joyLatchD ? -kOff : -kOn)) && crossV();
        if (ny >= 0.f) {
            m_joyLatchD = false;
            if (stepDas(wU, m_joyLatchU, m_joyDasU, 0, kDasInitV, kDasRepV)) {
                moved = true;
            }
        } else {
            m_joyLatchU = false;
            if (stepDas(wD, m_joyLatchD, m_joyDasD, 2, kDasInitV, kDasRepV)) {
                moved = true;
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

void Game2048Window::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 28, 42));

    const int ox = 24;
    const int oy = 24;
    const int side = kSize * kTilePx;

    p.setPen(QPen(QColor(70, 80, 110), 2));
    p.setBrush(QColor(40, 44, 60));
    p.drawRoundedRect(ox - 6, oy - 6, side + 12, side + 12, 8, 8);

    for (int r = 0; r < kSize; ++r) {
        for (int c = 0; c < kSize; ++c) {
            const int v = m_board[r][c];
            const int x = ox + c * kTilePx;
            const int y = oy + r * kTilePx;
            p.setBrush(tileColor(v));
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(x + 3, y + 3, kTilePx - 6, kTilePx - 6, 6, 6);
            if (v > 0) {
                p.setPen(QColor(255, 255, 255));
                QFont f(QStringLiteral("Microsoft YaHei UI"), v >= 100 ? 18 : 22, QFont::Bold);
                p.setFont(f);
                p.drawText(QRect(x, y, kTilePx, kTilePx), Qt::AlignCenter, QString::number(v));
            }
        }
    }

    const int tx = ox + side + 20;
    p.setPen(QColor(230, 235, 250));
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 12));
    p.drawText(tx, oy + 28, QStringLiteral("分数: %1").arg(m_score));
    p.drawText(tx, oy + 56, QStringLiteral("最大块: %1").arg(maxTileValue()));

    if (m_gameState == MENU) {
        p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 18, QFont::Bold));
        p.setPen(QColor(255, 220, 140));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("2048\n\n空格 / 摇杆 A 开始\nWASD / 摇杆方向移动\nESC / 摇杆 D 返回首页"));
    } else if (m_gameState == GAME_OVER) {
        p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 16, QFont::Bold));
        p.setPen(QColor(255, 140, 140));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("无法继续移动\n分数 %1\n\nR / 摇杆 A 重来　ESC / 摇杆 D 返回首页").arg(m_score));
    }

    if (m_gameState == PAUSED) {
        drawPauseOverlay(p);
    }
}

void Game2048Window::keyPressEvent(QKeyEvent *event)
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
        } else if (m_gameState == PLAYING && !noKbdRepeat) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastKbdVertMs >= 90) {
                m_lastKbdVertMs = now;
                tryMove(0);
            }
        }
        break;
    case Qt::Key_Down:
        if (m_gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
            cyclePauseFocus();
        } else if (m_gameState == PLAYING && !noKbdRepeat) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastKbdVertMs >= 90) {
                m_lastKbdVertMs = now;
                tryMove(2);
            }
        }
        break;
    case Qt::Key_Left:
        if (m_gameState == PLAYING && !noKbdRepeat) {
            tryMove(3);
        }
        break;
    case Qt::Key_Right:
        if (m_gameState == PLAYING && !noKbdRepeat) {
            tryMove(1);
        }
        break;
    case Qt::Key_W:
        m_keyW = true;
        if (m_gameState == PLAYING && !noKbdRepeat) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastKbdVertMs >= 90) {
                m_lastKbdVertMs = now;
                tryMove(0);
            }
        }
        break;
    case Qt::Key_S:
        m_keyS = true;
        if (m_gameState == PLAYING && !noKbdRepeat) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastKbdVertMs >= 90) {
                m_lastKbdVertMs = now;
                tryMove(2);
            }
        }
        break;
    case Qt::Key_A:
        m_keyA = true;
        if (m_gameState == PLAYING && !noKbdRepeat) {
            tryMove(3);
        }
        break;
    case Qt::Key_D:
        m_keyD = true;
        if (m_gameState == PLAYING && !noKbdRepeat) {
            tryMove(1);
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

void Game2048Window::keyReleaseEvent(QKeyEvent *event)
{
    if (!isVisible()) {
        event->ignore();
        return;
    }
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

void Game2048Window::mousePressEvent(QMouseEvent *event)
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

void Game2048Window::mouseMoveEvent(QMouseEvent *event)
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

void Game2048Window::closeEvent(QCloseEvent *event)
{
    prepareForShutdown();
    QWidget::closeEvent(event);
}
