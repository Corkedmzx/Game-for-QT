#include "thunder_fighter_window.h"
#include "shared_joy_state.h"
#include <atomic>
#include <cmath>
#include <QColor>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QMessageBox>
#include <QRandomGenerator>
#include <QMouseEvent>
#include <QtMath>
#include <QDebug>
#include <QDateTime>
#include <QGuiApplication>
#include <QMap>
#include <QMetaObject>
#include <QVector>

namespace {

static const QColor kBerserkBulletBrush[3] = {
    QColor(255, 0, 0), QColor(0, 150, 255), QColor(255, 255, 0)};
static const QColor kBerserkBulletPen[3] = {
    QColor(200, 0, 0), QColor(0, 100, 200), QColor(200, 200, 0)};

/** 防御上限：异常情况下避免 QList 无限增长拖垮主线程碰撞检测 */
constexpr int kMaxPlayerBulletsSafety = 2500;
constexpr int kMaxEnemyBulletsSafety = 2500;

/** 敌弹命中玩家时的单次伤害（原逻辑固定 -20） */
constexpr int kEnemyBulletDamageToPlayer = 20;

inline int damageForPlayerBullet(int colorType)
{
    double base = 1.0;
    if (colorType >= 0 && colorType <= 2)
        base *= 1.5;
    return qRound(base);
}

/** 空间桶粗筛：桶边长 96，Chebyshev 桶距 >2 时中心距至少约 96，大于敌机半径+子弹外包 */
constexpr double kBulletEnemyBucket = 96.0;

static inline int enemyBucketIndex(double v)
{
    return static_cast<int>(std::floor(v / kBulletEnemyBucket));
}

static inline bool bulletEnemyBucketTooFar(int bcx, int bcy, int ecx, int ecy)
{
    const int dx = ecx - bcx;
    const int dy = ecy - bcy;
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    const int cheb = adx > ady ? adx : ady;
    return cheb > 2;
}

} // namespace

ThunderFighterWindow::ThunderFighterWindow(SharedJoyState *joyInput, QWidget *parent)
    : QWidget(parent)
    , m_joyInput(joyInput)
    , gameState(MENU)
    , playerPos(400, 500)
    , playerSpeed(5)
    , playerHealth(100) // 初始血量（独立血条系统）
    , maxHealth(100) // 最大血量（固定100）
    , playerLevel(0) // 玩家升级等级
    , invincibleTime(0) // 无敌时间
    , invincibleFlashTimer(0) // 闪烁计时器
    , bulletCount(1) // 初始1发子弹
    , shootCooldownBase(10) // 基础射击冷却
    , playerShootCooldown(0) // 玩家射击冷却
    , hasShield(false) // 初始无护盾
    , shieldTimeLeft(0) // 护盾剩余时间
    , laserCooldown(0) // 穿透激光冷却
    , laserDuration(0) // 激光持续时间
    , berserkTimeLeft(0) // 暴走剩余时间
    , berserkBarrageAngle(0.0) // 暴走弹幕旋转角度
    , score(0)
    , level(1)
    , enemySpawnTimer(0)
    , enemySpawnInterval(2000)
    , enemyGroupSpawnTimer(0)
    , enemyGroupSpawnInterval(300) // 5秒间隔（300帧，60fps）
    , bossSpawnTimer(0)
    , bossSpawnInterval(30000) // 30秒生成一次Boss
    , bossKillCount(0) // Boss击杀计数
    , difficultyLevel(0) // 难度等级
    , enemyWaveCount(0) // 敌机波数计数
    , totalWaveCount(0) // 全局波次计数
    , nextEnemyId(1) // 下一个敌机ID（从1开始）
    , keyLeft(false)
    , keyRight(false)
    , keyUp(false)
    , keyDown(false)
    , keyW(false)
    , keyA(false)
    , keyS(false)
    , keyD(false)
    , keySpace(false)
    , keyPause(false)
    , mouseDragging(false)
    , mouseDragOffset(0, 0)
    , gameTimer(new QTimer(this))
    , enemySpawnTimerObj(new QTimer(this))
    , bossSpawnTimerObj(new QTimer(this))
    , m_inputTimer(new QTimer(this))
    , m_playerMoveTimer(new QTimer(this))
    , m_hudFont(QStringLiteral("Arial"), 12)
{
    /* 不再调用 uic 生成的 setupUi（内部 translate/retranslate 在部分环境下会走入 QFileInfo，
       调试器曾报告 QFileInfo::filePath 空指针访问）。原 .ui 仅含窗口尺寸与标题，此处等价设置即可。 */
    setObjectName(QStringLiteral("ThunderFighterWindow"));
    resize(800, 600);

    setWindowTitle("雷霆战机");
    setFixedSize(800, 600);
    
    // 设置焦点策略以接收键盘事件
    setFocusPolicy(Qt::StrongFocus);
    /** 暂停菜单按钮悬停指针：无鼠标跟踪则仅按住键时才收到 mouseMove */
    setMouseTracking(true);

    connect(gameTimer, &QTimer::timeout, this, &ThunderFighterWindow::updateGame);
    // 注意：敌机生成由 updateGame 中的 enemyGroupSpawnTimer 控制，不再使用 enemySpawnTimerObj
    // connect(enemySpawnTimerObj, &QTimer::timeout, this, &ThunderFighterWindow::spawnEnemy);
    connect(bossSpawnTimerObj, &QTimer::timeout, this, &ThunderFighterWindow::spawnBoss);

    connect(m_inputTimer, &QTimer::timeout, this, &ThunderFighterWindow::pollJoyHardwareButtons);
    m_inputTimer->start(8);

    m_playerMoveTimer->setInterval(8);
    connect(m_playerMoveTimer, &QTimer::timeout, this, &ThunderFighterWindow::tickPlayerMoveInput);
    m_playerMoveTimer->start();
    m_kbdJoyMoveClock.start();

    gameTimer->setInterval(16);
    gameTimer->setTimerType(Qt::PreciseTimer);

    m_arenaWidth = width();
    m_arenaHeight = height();

    // 初始化游戏
    initGame();
}

void ThunderFighterWindow::resetJoyButtonEdges()
{
    if (m_joyInput) {
        m_prevJoyButtons = m_joyInput->buttonMask.load(std::memory_order_relaxed);
    } else {
        m_prevJoyButtons = 0;
    }
}

void ThunderFighterWindow::scheduleJoyEdgeResync()
{
    resetJoyButtonEdges();
    QTimer::singleShot(30, this, [this]() {
        if (m_destroying) {
            return;
        }
        resetJoyButtonEdges();
    });
    QTimer::singleShot(120, this, [this]() {
        if (m_destroying) {
            return;
        }
        resetJoyButtonEdges();
    });
}

void ThunderFighterWindow::openPauseMenu()
{
    if (gameState != PLAYING) {
        return;
    }
    gameState = PAUSED;
    enemySpawnTimerObj->stop();
    bossSpawnTimerObj->stop();
    m_pauseOverlay = PauseOverlayMain;
    m_pauseFocusIndex = 0;
    if (gameTimer) {
        gameTimer->stop();
    }
}

void ThunderFighterWindow::resumeFromPause()
{
    if (gameState != PAUSED) {
        return;
    }
    unsetCursor();
    gameState = PLAYING;
    bossSpawnTimerObj->start(bossSpawnInterval);
    m_pauseOverlay = PauseOverlayNone;
    if (gameTimer) {
        gameTimer->start(16);
        primeGameFrameClock();
    }
}

void ThunderFighterWindow::handleEscapeKey()
{
    if (gameState == MENU || gameState == GAME_OVER) {
        emit requestReturnToSetup();
        return;
    }
    if (gameState == PLAYING) {
        openPauseMenu();
    } else if (gameState == PAUSED) {
        if (m_pauseOverlay == PauseOverlayExitConfirm) {
            m_pauseOverlay = PauseOverlayMain;
            m_pauseFocusIndex = 1;
        } else if (m_pauseOverlay == PauseOverlayMain) {
            resumeFromPause();
        }
    }
}

void ThunderFighterWindow::cyclePauseFocus()
{
    if (m_pauseOverlay == PauseOverlayNone) {
        return;
    }
    m_pauseFocusIndex = (m_pauseFocusIndex + 1) % 2;
}

void ThunderFighterWindow::activatePauseSelection()
{
    if (gameState != PAUSED || m_pauseOverlay == PauseOverlayNone) {
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

QRect ThunderFighterWindow::pauseOverlayPanelRect() const
{
    const int panelW = 400;
    const int panelH = (m_pauseOverlay == PauseOverlayExitConfirm) ? 260 : 268;
    return QRect((width() - panelW) / 2, (height() - panelH) / 2, panelW, panelH);
}

QRect ThunderFighterWindow::pauseOverlayButtonRect(int row) const
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

void ThunderFighterWindow::drawPauseOverlay(QPainter &painter)
{
    /* 勿用 QGuiApplication::font()：部分环境下会得到空 family，引发 DirectWrite 异常与 QString 断言 */
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

void ThunderFighterWindow::pollJoyHardwareButtons()
{
    if (m_destroying) {
        return;
    }
    if (!m_joyInput || !m_joyInput->serialConnected.load(std::memory_order_relaxed)) {
        m_prevJoyButtons = 0;
        return;
    }
    /** 未作为当前页显示时（主窗口项目列表/摇杆标签）：不消费摇杆边沿，避免与 MainWindow 主页逻辑抢 A */
    if (!isVisible()) {
        m_prevJoyButtons = m_joyInput->buttonMask.load(std::memory_order_relaxed);
        return;
    }

    const quint32 cur = m_joyInput->buttonMask.load(std::memory_order_relaxed);
    /* 叠在主窗口启动器/摇杆标签下时不可见：不消费按键，避免与 MainWindow 主页 B/A/C 冲突 */
    if (!isVisible()) {
        m_prevJoyButtons = cur;
        return;
    }

    const quint32 rising = cur & ~m_prevJoyButtons;
    m_prevJoyButtons = cur;
    if (rising == 0) {
        return;
    }

    // 准备页/结束页优先处理 A，避免与 C 同帧时先被下面 C 分支 return 吞掉
    if ((gameState == MENU || gameState == GAME_OVER) && (rising & JoyMask::A)) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastJoyMenuActionMs >= 400) {
            m_lastJoyMenuActionMs = now;
            resetGame();
        }
        update();
        return;
    }

    // 暂停菜单：摇杆 B 切换、A 确认；C/D 同效力：继续游戏或从二次确认返回上一级
    if (gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
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

    // 游戏中：C / D 均只打开暂停菜单；返回主窗口须在菜单内选择「返回游戏首页」并确认
    if ((rising & (JoyMask::C | JoyMask::D)) && gameState == PLAYING) {
        openPauseMenu();
        update();
        return;
    }

    // 游戏内菜单/结束页：D 仍可直接返回主窗口首页（与 ESC 一致）
    if ((rising & JoyMask::D) && (gameState == MENU || gameState == GAME_OVER)) {
        emit requestReturnToSetup();
        update();
        return;
    }
}

ThunderFighterWindow::~ThunderFighterWindow()
{
    prepareForShutdown();
}

void ThunderFighterWindow::prepareForShutdown()
{
    if (m_destroying) {
        return;
    }
    m_destroying = true;
    /* 仅断开本窗口定时器槽，避免 disconnect(this) 全量断开引发退出阶段异常 */
    if (gameTimer) {
        QObject::disconnect(gameTimer, nullptr, this, nullptr);
    }
    if (enemySpawnTimerObj) {
        QObject::disconnect(enemySpawnTimerObj, nullptr, this, nullptr);
    }
    if (bossSpawnTimerObj) {
        QObject::disconnect(bossSpawnTimerObj, nullptr, this, nullptr);
    }
    if (m_inputTimer) {
        QObject::disconnect(m_inputTimer, nullptr, this, nullptr);
    }
    if (m_playerMoveTimer) {
        QObject::disconnect(m_playerMoveTimer, nullptr, this, nullptr);
    }
    stopAllGameTimers();
    m_joyInput = nullptr;
}

void ThunderFighterWindow::returnToLauncher()
{
    if (m_destroying) {
        return;
    }
    unsetCursor();
    initGame();
    update();
}

void ThunderFighterWindow::stopAllGameTimers()
{
    if (gameTimer) {
        gameTimer->stop();
    }
    if (enemySpawnTimerObj) {
        enemySpawnTimerObj->stop();
    }
    if (bossSpawnTimerObj) {
        bossSpawnTimerObj->stop();
    }
    if (m_inputTimer) {
        m_inputTimer->stop();
    }
    if (m_playerMoveTimer) {
        m_playerMoveTimer->stop();
    }
}

void ThunderFighterWindow::invalidateHudTextCache()
{
    m_hudSigScore = INT_MIN;
    m_hudSigLevel = INT_MIN;
    m_hudSigWave = INT_MIN;
    m_hudSigPlane = INT_MIN;
    m_hudSigHealth = INT_MIN;
    m_hudSigShieldSec = INT_MIN;
    m_hudSigBerserkSec = INT_MIN;
    m_hudSigDiff = INT_MIN;
    m_hudSigHasShield = false;
    m_hudSigBerserkLine = false;
}

void ThunderFighterWindow::initGame()
{
    gameState = MENU;
    m_pauseOverlay = PauseOverlayNone;
    m_pauseFocusIndex = 0;
    playerBullets.clear();
    enemyBullets.clear();
    enemies.clear();
    explosions.clear();
    bosses.clear();
    powerUps.clear();
    drones.clear();

    enemySpawnTimer = 0;
    enemyGroupSpawnTimer = 0;
    m_spawnInVacuumPeriod = false;
    m_spawnVacuumTimer = 0;

    playerPos = QPointF(400, 500);
    playerLevel = 0;
    maxHealth = 100; // 固定最大血量100
    playerHealth = maxHealth; // 初始满血
    invincibleTime = 0;
    invincibleFlashTimer = 0;
    bulletCount = 1;
    shootCooldownBase = 10;
    playerShootCooldown = 0;
    hasShield = false;
    shieldTimeLeft = 0;
    wingmanPositions.clear();
    laserCooldown = 0;
    laserDuration = 0;
    berserkTimeLeft = 0;
    berserkBarrageAngle = 0.0;
    score = 0;
    level = 1;
    enemySpawnInterval = 2000;
    bossSpawnTimer = 0;
    bossKillCount = 0;
    difficultyLevel = 0;
    enemyWaveCount = 0;
    totalWaveCount = 0;
    nextEnemyId = 1; // 重置敌机ID计数器
    mouseDragging = false;
    mouseDragOffset = QPointF(0, 0);
    m_joyMotionActive = false;

    invalidateHudTextCache();

    if (gameTimer) {
        gameTimer->stop();
    }
    enemySpawnTimerObj->stop();
    bossSpawnTimerObj->stop();
}

void ThunderFighterWindow::resetGame()
{
    initGame();
    gameState = PLAYING;
    
    // 启动游戏循环（60fps，主线程 QTimer）
    if (gameTimer) {
        gameTimer->start(16);
        primeGameFrameClock();
    }
    // 敌机生成现在由updateGame中的enemyGroupSpawnTimer控制
    // enemySpawnTimerObj->start(enemySpawnInterval);
    bossSpawnTimerObj->start(bossSpawnInterval);
}

void ThunderFighterWindow::primeGameFrameClock()
{
    m_gameFrameClock.start();
    m_kbdJoyMoveClock.start();
}

void ThunderFighterWindow::tickPlayerMoveInput()
{
    if (m_destroying) {
        return;
    }
    qint64 dtMs = m_kbdJoyMoveClock.restart();
    if (gameState != PLAYING || mouseDragging) {
        return;
    }
    if (dtMs < 1 || dtMs > 80) {
        dtMs = 16;
    }
    applyKeyboardJoyMovementStep(static_cast<double>(dtMs) / 16.0);
}

void ThunderFighterWindow::applyKeyboardJoyMovementStep(double moveScale)
{
    if (m_destroying) {
        return;
    }

    const double step = static_cast<double>(playerSpeed) * moveScale;

    bool useJoy = false;
    float jxn = 0.f;
    float jyn = 0.f;
    if (m_joyInput && m_joyInput->serialConnected.load(std::memory_order_relaxed)) {
        constexpr float dzEnter = 0.14f;
        constexpr float dzExit = 0.08f;
        const float jx = m_joyInput->nx.load(std::memory_order_relaxed);
        const float jy = m_joyInput->ny.load(std::memory_order_relaxed);
        const float mag = std::sqrt(jx * jx + jy * jy);
        if (!m_joyMotionActive) {
            if (mag >= dzEnter) {
                m_joyMotionActive = true;
            }
        } else {
            if (mag < dzExit) {
                m_joyMotionActive = false;
            }
        }
        if (m_joyMotionActive) {
            useJoy = true;
            jxn = jx;
            jyn = jy;
        }
    } else {
        m_joyMotionActive = false;
    }
    if (useJoy) {
        const double len = std::hypot(jxn, jyn);
        if (len > 1e-5) {
            const double ix = jxn / len;
            const double iy = jyn / len;
            playerPos.rx() += ix * step;
            playerPos.ry() -= iy * step;
        }
        playerPos.setX(qBound(20.0, playerPos.x(), static_cast<double>(m_arenaWidth) - 20.0));
        playerPos.setY(qBound(50.0, playerPos.y(), static_cast<double>(m_arenaHeight) - 30.0));
        return;
    }

    bool moveLeft = (keyLeft || keyA) && playerPos.x() > 20;
    bool moveRight = (keyRight || keyD) && playerPos.x() < m_arenaWidth - 20;
    bool moveUp = (keyUp || keyW) && playerPos.y() > 50;
    bool moveDown = (keyDown || keyS) && playerPos.y() < m_arenaHeight - 30;

    if (moveLeft) {
        playerPos.rx() -= step;
    }
    if (moveRight) {
        playerPos.rx() += step;
    }
    if (moveUp) {
        playerPos.ry() -= step;
    }
    if (moveDown) {
        playerPos.ry() += step;
    }
}

void ThunderFighterWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // 绘制背景
    drawBackground(painter);
    
    if (gameState == MENU) {
        drawMenu(painter);
    } else if (gameState == GAME_OVER) {
        drawPlayer(painter);
        drawBullets(painter);
        drawEnemies(painter);
        drawExplosions(painter);
        drawGameOver(painter);
    } else {
        // 游戏中或暂停
        drawPlayer(painter);
        drawBullets(painter);
        drawEnemies(painter);
        drawBosses(painter);
        drawPowerUps(painter);
        drawDrones(painter);
        drawShield(painter);
        drawExplosions(painter);
        drawUI(painter);
        
        if (gameState == PAUSED) {
            drawPauseOverlay(painter);
        }
    }
}

void ThunderFighterWindow::drawBackground(QPainter &painter)
{
    // 绘制星空背景
    painter.fillRect(rect(), QColor(10, 10, 30));
    
    // 绘制星星（简单的点）
    painter.setPen(QColor(255, 255, 255));
    for (int i = 0; i < 50; ++i) {
        int x = (i * 37 + score) % width();
        int y = (i * 73 + score * 2) % height();
        painter.drawPoint(x, y);
    }
}

void ThunderFighterWindow::drawPlayer(QPainter &painter)
{
    // 无敌时间闪烁效果：每10帧切换一次显示状态
    bool shouldDraw = true;
    if (invincibleTime > 0) {
        // 闪烁效果：每10帧中前5帧显示，后5帧隐藏
        shouldDraw = (invincibleFlashTimer < 5);
    }
    
    if (shouldDraw) {
        painter.save();
        painter.translate(playerPos);

        // 暴走外观：红色能量外环与更亮的主体
        if (berserkTimeLeft > 0) {
            int pulse = 120 + (berserkTimeLeft % 20) * 6;
            painter.setPen(QPen(QColor(255, 60, 60, qBound(80, pulse, 230)), 3));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QPointF(0, 0), 26, 26);
            painter.drawEllipse(QPointF(0, 0), 20, 20);
        }
        
        // 无敌时间时使用半透明效果（闪烁时显示）
        if (invincibleTime > 0 && shouldDraw) {
            painter.setOpacity(0.7); // 半透明，稍微亮一点以便看到
        }
        
        // 绘制玩家战机（三角形，增大100%）
        QPolygonF fighter;
        fighter << QPointF(0, -30)  // 顶部（-15 * 2）
                << QPointF(-20, 20) // 左下（-10 * 2, 10 * 2）
                << QPointF(0, 10)   // 中间（0, 5 * 2）
                << QPointF(20, 20); // 右下（10 * 2, 10 * 2）
        
        if (berserkTimeLeft > 0) {
            painter.setBrush(QColor(255, 90, 90));
            painter.setPen(QPen(QColor(180, 20, 20), 2));
        } else {
            painter.setBrush(QColor(0, 150, 255));
            painter.setPen(QPen(QColor(0, 100, 200), 2));
        }
        painter.drawPolygon(fighter);
        
        // 绘制引擎光效（增大100%）
        painter.setBrush(QColor(255, 200, 0));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(-6, 16, 12, 8);  // (-3*2, 8*2, 6*2, 4*2)
        painter.drawEllipse(6, 16, 12, 8);   // (3*2, 8*2, 6*2, 4*2)
        
        // 在玩家战机中心绘制白色小圆点（碰撞检测点）
        painter.setBrush(QColor(255, 255, 255));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(0, 0), 3, 3); // 中心点，半径3像素
        
        painter.restore();
    }
    
    // 绘制僚机（4级和5级）- 即使玩家闪烁也绘制僚机
    
    // 绘制僚机（4级和5级）
    if (playerLevel >= 4) {
        for (const QPointF &wingmanPos : wingmanPositions) {
            painter.save();
            painter.translate(wingmanPos);
            
            // 绘制小型僚机（小三角形，增大100%）
            QPolygonF wingman;
            wingman << QPointF(0, -16)   // (0, -8*2)
                    << QPointF(-10, 10)  // (-5*2, 5*2)
                    << QPointF(0, 4)     // (0, 2*2)
                    << QPointF(10, 10);  // (5*2, 5*2)
            
            painter.setBrush(QColor(100, 200, 255)); // 稍浅的蓝色
            painter.setPen(QPen(QColor(50, 150, 200), 1));
            painter.drawPolygon(wingman);
            
            // 僚机引擎光效（增大100%）
            painter.setBrush(QColor(255, 150, 0));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(-4, 8, 8, 6); // (-2*2, 4*2, 4*2, 3*2)
            painter.drawEllipse(2, 4, 4, 3);
            
            painter.restore();
        }
    }
}

void ThunderFighterWindow::drawBullets(QPainter &painter)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);

    // 绘制玩家子弹（关闭 AA，大量椭圆时 GPU/CPU 压力明显降低）
    for (const Bullet &bullet : playerBullets) {
        // 暴走弹幕：红蓝黄三色（子弹大小减小，形成带状效果）
        if (bullet.colorType >= 0 && bullet.colorType <= 2) {
            const int ct = qBound(0, bullet.colorType, 2);
            painter.setBrush(kBerserkBulletBrush[ct]);
            painter.setPen(QPen(kBerserkBulletPen[ct], 1));
            painter.drawEllipse(bullet.pos, 3, 3); // 圆形弹幕（大小减小，从5改为3）
        } else if (bullet.penetration > 0 && playerLevel >= 5) {
            // 5级时的穿透子弹：显示为稍亮的黄色
            painter.setBrush(QColor(255, 255, 100));
            painter.setPen(QPen(QColor(255, 200, 0), 2));
            painter.drawEllipse(bullet.pos, 4, 10);
        } else {
            // 普通子弹
            painter.setBrush(QColor(255, 255, 0));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(bullet.pos, 3, 8);
        }
    }

    // 绘制激光（仅在4/5级且激光激活时）— 双层线替代四重 drawLine
    if (playerLevel >= 4 && laserDuration > 0) {
        QVector<QPointF> wingPts;
        wingPts.reserve(4);
        if (playerLevel == 4) {
            wingPts.append(QPointF(playerPos.x() - 30, playerPos.y() + 5));
            wingPts.append(QPointF(playerPos.x() + 30, playerPos.y() + 5));
        } else if (playerLevel >= 5) {
            wingPts.append(QPointF(playerPos.x() - 30, playerPos.y() - 20));
            wingPts.append(QPointF(playerPos.x() + 30, playerPos.y() - 20));
            wingPts.append(QPointF(playerPos.x() - 45, playerPos.y() + 15));
            wingPts.append(QPointF(playerPos.x() + 45, playerPos.y() + 15));
        }

        painter.setBrush(Qt::NoBrush);
        for (int wi = 0; wi < wingPts.size(); ++wi) {
            const QPointF &wingmanPos = wingPts.at(wi);
            const QPointF start = wingmanPos;
            const QPointF end(wingmanPos.x(), 0);

            painter.setPen(QPen(QColor(0, 120, 255, 140), 10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(start, end);
            painter.setPen(QPen(QColor(200, 240, 255, 230), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(start, end);
        }
    }

    // 绘制敌机子弹
    painter.setBrush(QColor(255, 0, 0));
    painter.setPen(Qt::NoPen);
    for (const Bullet &bullet : enemyBullets) {
        painter.drawEllipse(bullet.pos, 2, 6);
    }

    painter.restore();
}

void ThunderFighterWindow::drawEnemies(QPainter &painter)
{
    for (const Enemy &enemy : enemies) {
        painter.save();
        painter.translate(enemy.pos);
        
        // 根据敌机类型绘制不同大小
        int size = 15 + enemy.type * 5;
        
        // 绘制敌机（倒三角形）
        QPolygonF enemyShape;
        enemyShape << QPointF(0, size)      // 底部
                   << QPointF(-size, -5)   // 左上
                   << QPointF(0, 0)         // 中间
                   << QPointF(size, -5);    // 右上
        
        // 根据生命值设置颜色
        QColor enemyColor;
        if (enemy.type == 0) {
            enemyColor = QColor(255, 100, 100); // 小型敌机 - 红色
        } else if (enemy.type == 1) {
            enemyColor = QColor(255, 150, 0);   // 中型敌机 - 橙色
        } else {
            enemyColor = QColor(200, 0, 200);   // 大型敌机 - 紫色
        }
        
        painter.setBrush(enemyColor);
        painter.setPen(QPen(enemyColor.darker(), 2));
        painter.drawPolygon(enemyShape);
        
        // 绘制生命值条
        if (enemy.health < (enemy.type + 1)) {
            painter.setPen(QPen(QColor(255, 0, 0), 1));
            painter.setBrush(QColor(255, 0, 0));
            int barWidth = size * 2;
            int barHeight = 3;
            painter.drawRect(-barWidth/2, -size - 8, barWidth, barHeight);
            
            painter.setBrush(QColor(0, 255, 0));
            int healthWidth = barWidth * enemy.health / (enemy.type + 1);
            painter.drawRect(-barWidth/2, -size - 8, healthWidth, barHeight);
        }
        
        painter.restore();
    }
}

void ThunderFighterWindow::drawBosses(QPainter &painter)
{
    for (const Boss &boss : bosses) {
        // 绘制警告动画
        if (boss.isWarning) {
            painter.save();
            // 警告文字闪烁效果
            int alpha = 150 + (boss.warningTimer % 20) * 5;
            painter.setPen(QPen(QColor(255, 0, 0, alpha), 4));
            painter.setFont(QFont("Arial", 48, QFont::Bold));
            QRect warningRect = rect();
            warningRect.setTop(200);
            warningRect.setHeight(80);
            painter.drawText(warningRect, Qt::AlignCenter, "WARNING!");
            
            // 绘制警告边框
            painter.setPen(QPen(QColor(255, 0, 0, alpha), 5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect().adjusted(10, 10, -10, -10));
            
            painter.restore();
            continue; // 警告阶段不绘制Boss
        }
        
        // 处理层转换等待阶段：Boss保持在场上，但处于等待状态
        // 注意：等待阶段的Boss仍然会被绘制，只是不移动和攻击
        
        if (!boss.isActive && boss.layerTransitionTimer == 0) {
            // 只有在非激活且不在等待阶段时才跳过绘制
            continue;
        }
        
        painter.save();
        painter.translate(boss.pos);
        
        // 绘制Boss（大型敌机，更复杂的设计，加大尺寸）
        int bossSize = 80; // 从60增加到80，加大boss模型
        
        // 主体（大矩形）
        QRectF bodyRect(-bossSize, -bossSize/2, bossSize * 2, bossSize);
        painter.setBrush(QColor(200, 0, 0)); // 深红色
        painter.setPen(QPen(QColor(255, 0, 0), 4)); // 加粗边框
        painter.drawRoundedRect(bodyRect, 8, 8);
        
        // 绘制装饰（两侧的翼，加大）
        QPolygonF leftWing;
        leftWing << QPointF(-bossSize, 0)
                 << QPointF(-bossSize - 30, -20)
                 << QPointF(-bossSize - 20, 0)
                 << QPointF(-bossSize - 30, 20);
        painter.setBrush(QColor(150, 0, 0));
        painter.drawPolygon(leftWing);
        
        QPolygonF rightWing;
        rightWing << QPointF(bossSize, 0)
                  << QPointF(bossSize + 30, -20)
                  << QPointF(bossSize + 20, 0)
                  << QPointF(bossSize + 30, 20);
        painter.drawPolygon(rightWing);
        
        // 绘制生命值条（多血条Boss在一条血槽上分段显示）
        int barWidth = bossSize * 3;
        int barHeight = 10; // 加粗血条
        int barY = -bossSize - 10; // 血条位置（向下移动，确保完全显示）
        
        // 背景条（整个血槽）
        painter.setPen(QPen(QColor(255, 0, 0), 3));
        painter.setBrush(QColor(100, 0, 0));
        painter.drawRect(-barWidth/2, barY, barWidth, barHeight);
        
        if (boss.maxLayer == 1) {
            // 单血条Boss：直接显示当前生命值
            painter.setBrush(QColor(0, 255, 0)); // 绿色
            int healthWidth = barWidth * boss.health / boss.maxHealth;
            painter.drawRect(-barWidth/2, barY, healthWidth, barHeight);
        } else {
            // 双血条Boss：在一条血槽上分段显示
            // 第一段（第二层，layer == 2）：右侧部分，绿色
            // 第二段（第一层，layer == 1）：左侧部分，蓝色
            int segmentWidth = barWidth / boss.maxLayer;
            
            if (boss.layer == 2) {
                // 显示第一段（第二层）生命值，在右侧
                painter.setBrush(QColor(0, 255, 0)); // 绿色
                int healthWidth = segmentWidth * boss.health / boss.maxHealth;
                painter.drawRect(barWidth/2 - segmentWidth, barY, healthWidth, barHeight);
            } else if (boss.layer == 1) {
                // 第一段已清空，显示为灰色
                painter.setBrush(QColor(100, 100, 100));
                painter.drawRect(barWidth/2 - segmentWidth, barY, segmentWidth, barHeight);
                
                // 显示第二段（第一层）生命值，在左侧，蓝色
                painter.setBrush(QColor(0, 150, 255)); // 蓝色
                int healthWidth = segmentWidth * boss.health / boss.maxHealth;
                painter.drawRect(-barWidth/2, barY, healthWidth, barHeight);
            }
        }
        
        // 绘制Boss标签和层数
        painter.setPen(QColor(255, 255, 0));
        painter.setFont(QFont("Arial", 12, QFont::Bold));
        QString bossText = QString("BOSS L%1/%2").arg(boss.layer).arg(boss.maxLayer);
        painter.drawText(QRectF(-barWidth/2, barY - 25, barWidth, 18), 
                        Qt::AlignCenter, bossText);
        
        painter.restore();
    }
}

void ThunderFighterWindow::drawExplosions(QPainter &painter)
{
    for (const Explosion &explosion : explosions) {
        int radius = explosion.frame * 2;
        int alpha = 255 - (explosion.frame * 255 / explosion.maxFrames);
        
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 200, 0, alpha));
        painter.drawEllipse(explosion.pos, radius, radius);
        
        painter.setBrush(QColor(255, 0, 0, alpha / 2));
        painter.drawEllipse(explosion.pos, radius * 0.7, radius * 0.7);
    }
}

void ThunderFighterWindow::drawUI(QPainter &painter)
{
    painter.setFont(m_hudFont);

    if (m_hudSigScore != score) {
        m_hudSigScore = score;
        m_hudTextScore = QStringLiteral("分数: %1").arg(score);
    }
    if (m_hudSigLevel != level) {
        m_hudSigLevel = level;
        m_hudTextLevel = QStringLiteral("等级: %1").arg(level);
    }
    const int waveShown = totalWaveCount + 1;
    if (m_hudSigWave != waveShown) {
        m_hudSigWave = waveShown;
        m_hudTextWave = QStringLiteral("波次: %1").arg(waveShown);
    }
    if (m_hudSigPlane != playerLevel) {
        m_hudSigPlane = playerLevel;
        m_hudTextPlane = QStringLiteral("战机等级: %1/5").arg(playerLevel);
    }
    if (m_hudSigHealth != playerHealth) {
        m_hudSigHealth = playerHealth;
        m_hudTextHealth = QStringLiteral("生命值: %1/%2").arg(playerHealth).arg(maxHealth);
    }

    const bool shieldLine = hasShield && shieldTimeLeft > 0;
    const int shieldSec = shieldTimeLeft / 60;
    if (shieldLine != m_hudSigHasShield || (shieldLine && m_hudSigShieldSec != shieldSec)) {
        m_hudSigHasShield = shieldLine;
        m_hudSigShieldSec = shieldSec;
        if (shieldLine) {
            m_hudTextShield = QStringLiteral("护盾: %1秒").arg(shieldSec);
        }
    }

    const bool berserkLine = berserkTimeLeft > 0;
    const int berserkSec = berserkTimeLeft / 60;
    if (berserkLine != m_hudSigBerserkLine || (berserkLine && m_hudSigBerserkSec != berserkSec)) {
        m_hudSigBerserkLine = berserkLine;
        m_hudSigBerserkSec = berserkSec;
        if (berserkLine) {
            m_hudTextBerserk = QStringLiteral("暴走: %1秒").arg(berserkSec);
        }
    }

    if (m_hudSigDiff != difficultyLevel) {
        m_hudSigDiff = difficultyLevel;
        m_hudTextDifficulty = QStringLiteral("难度等级: %1").arg(difficultyLevel);
    }

    painter.setPen(QColor(255, 255, 255));
    painter.drawText(10, 25, m_hudTextScore);
    painter.drawText(10, 45, m_hudTextLevel);
    painter.drawText(10, 65, m_hudTextWave);

    painter.setPen(QColor(255, 215, 0)); // 金色
    painter.drawText(10, 85, m_hudTextPlane);

    painter.setPen(QColor(255, 255, 255));
    painter.drawText(10, 105, m_hudTextHealth);
    
    // 绘制血条背景（灰色）
    int barWidth = 200;
    int barHeight = 15;
    int barX = 10;
    int barY = 120;
    painter.setPen(QPen(QColor(100, 100, 100), 2));
    painter.setBrush(QColor(50, 50, 50));
    painter.drawRect(barX, barY, barWidth, barHeight);
    
    // 绘制当前血量（红色）
    int healthWidth = barWidth * playerHealth / maxHealth;
    if (healthWidth > 0) {
        // 根据血量百分比选择颜色：绿色(>60%) -> 黄色(30-60%) -> 红色(<30%)
        QColor healthColor;
        double healthPercent = (double)playerHealth / maxHealth;
        if (healthPercent > 0.6) {
            healthColor = QColor(0, 255, 0); // 绿色
        } else if (healthPercent > 0.3) {
            healthColor = QColor(255, 255, 0); // 黄色
        } else {
            healthColor = QColor(255, 0, 0); // 红色
        }
        painter.setPen(QPen(healthColor.darker(), 1));
        painter.setBrush(healthColor);
        painter.drawRect(barX + 1, barY + 1, healthWidth - 2, barHeight - 2);
    }
    
    // 绘制护盾状态
    if (shieldLine) {
        painter.setPen(QColor(0, 150, 255)); // 蓝色
        painter.drawText(10, 145, m_hudTextShield);
    }

    // 绘制暴走状态
    if (berserkLine) {
        painter.setPen(QColor(255, 80, 80)); // 红色
        painter.drawText(10, 185, m_hudTextBerserk);
    }

    // 绘制难度等级
    painter.setPen(QColor(255, 100, 100)); // 红色
    painter.drawText(10, berserkLine ? 205 : 165, m_hudTextDifficulty);
}

void ThunderFighterWindow::drawMenu(QPainter &painter)
{
    // 绘制标题
    painter.setPen(QColor(255, 255, 0));
    painter.setFont(QFont("Arial", 36, QFont::Bold));
    QRect titleRect = rect();
    titleRect.setTop(60);
    titleRect.setHeight(50);
    painter.drawText(titleRect, Qt::AlignCenter, "雷霆战机");
    
    // 绘制开始提示
    painter.setPen(QColor(0, 255, 255));
    painter.setFont(QFont("Arial", 20, QFont::Bold));
    QRect startRect = rect();
    startRect.setTop(130);
    startRect.setHeight(35);
    painter.drawText(startRect, Qt::AlignCenter, QStringLiteral("空格 / 摇杆 A / 鼠标左键　开始游戏"));
    
    // 绘制操作说明（分行显示，增加间距）
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(QFont("Arial", 13));
    
    int yPos = 190;
    int lineHeight = 28;
    
    // 操作说明标题
    painter.setPen(QColor(255, 200, 0));
    painter.setFont(QFont("Arial", 15, QFont::Bold));
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "操作说明");
    yPos += lineHeight + 5;
    
    // 操作说明内容
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(QFont("Arial", 13));
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "WASD 或 ↑↓←→ 移动战机");
    yPos += lineHeight;
    
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "鼠标左键拖动 移动战机");
    yPos += lineHeight;
    
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "自动发射子弹");
    yPos += lineHeight;
    
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter,
                     QStringLiteral(
                         "ESC：返回游戏首页　游戏中 P/C/D：暂停菜单（返回主页请从菜单操作）"));
    yPos += lineHeight + 10;
    
    // 游戏机制说明标题
    painter.setPen(QColor(255, 200, 0));
    painter.setFont(QFont("Arial", 15, QFont::Bold));
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "游戏机制");
    yPos += lineHeight + 5;
    
    // 游戏机制说明内容
    painter.setPen(QColor(200, 255, 200));
    painter.setFont(QFont("Arial", 12));
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "• 击败敌机获得升级道具，最高5级");
    yPos += lineHeight;
    
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "• 5级后获得护盾/暴走道具");
    yPos += lineHeight;
    
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "• 每5波出现Boss，击败Boss获得奖励");
    yPos += lineHeight;
    
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "• 4/5级战机可发射穿透激光");
    yPos += lineHeight;
    
    painter.drawText(rect().adjusted(0, yPos, 0, 0), Qt::AlignCenter, "• 暴走模式：大幅提升攻击力");
}

void ThunderFighterWindow::drawGameOver(QPainter &painter)
{
    // 半透明背景
    painter.fillRect(rect(), QColor(0, 0, 0, 180));
    
    painter.setPen(QColor(255, 0, 0));
    painter.setFont(QFont("Arial", 32, QFont::Bold));
    painter.drawText(rect().adjusted(0, -100, 0, 0), Qt::AlignCenter, "游戏结束");
    
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(QFont("Arial", 18));
    painter.drawText(rect().adjusted(0, 50, 0, 0), Qt::AlignCenter,
                     QStringLiteral("最终分数: %1\n\nR / 摇杆 A / 鼠标左键 重新开始　ESC 返回首页").arg(score));
}

void ThunderFighterWindow::keyPressEvent(QKeyEvent *event)
{
    if (!isVisible()) {
        event->ignore();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Escape:
        handleEscapeKey();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
            activatePauseSelection();
        }
        break;
    case Qt::Key_Up:
        if (gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
            cyclePauseFocus();
        } else {
            keyUp = true;
        }
        break;
    case Qt::Key_Down:
        if (gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
            cyclePauseFocus();
        } else {
            keyDown = true;
        }
        break;
    case Qt::Key_Left:
        keyLeft = true;
        break;
    case Qt::Key_Right:
        keyRight = true;
        break;
    case Qt::Key_A:
        keyA = true;
        break;
    case Qt::Key_D:
        keyD = true;
        break;
    case Qt::Key_W:
        keyW = true;
        break;
    case Qt::Key_S:
        keyS = true;
        break;
    case Qt::Key_Space:
        if (gameState == MENU) {
            resetGame();
        } else if (gameState == PLAYING || gameState == PAUSED) {
            keySpace = true;
        }
        break;
    case Qt::Key_P:
        handleEscapeKey();
        break;
    case Qt::Key_R:
        if (gameState == GAME_OVER) {
            resetGame();
        }
        break;
    }
    
    update();
}

void ThunderFighterWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (!isVisible()) {
        event->ignore();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left:
        keyLeft = false;
        break;
    case Qt::Key_Right:
        keyRight = false;
        break;
    case Qt::Key_Up:
        keyUp = false;
        break;
    case Qt::Key_Down:
        keyDown = false;
        break;
    case Qt::Key_A:
        keyA = false;
        break;
    case Qt::Key_D:
        keyD = false;
        break;
    case Qt::Key_W:
        keyW = false;
        break;
    case Qt::Key_S:
        keyS = false;
        break;
    case Qt::Key_Space:
        keySpace = false;
        break;
    }
}

void ThunderFighterWindow::updateGame()
{
    if (m_destroying) {
        return;
    }
    if (gameState == PAUSED) {
        update();
        return;
    }
    if (gameState != PLAYING) {
        return;
    }

    (void)m_gameFrameClock.restart();
    updatePlayer();
    updateBullets();
    updateEnemies();
    updateBosses();
    updatePowerUps();
    updateShield();
    updateLaser();
    updateInvincible(); // 更新无敌时间
    updateBerserk(); // 更新暴走时间
    updateDrones(); // 更新浮游炮
    updateExplosions();
    checkCollisions();
    
    // 更新敌机组生成计时器
    // 检查Boss是否存在，如果存在则重置计时器，避免Boss消失后密集刷新
    bool bossExists = false;
    for (const Boss &boss : bosses) {
        if (boss.isActive || boss.isWarning || boss.layerTransitionTimer > 0) {
            bossExists = true;
            break;
        }
    }
    
    if (!bossExists) {
        // 只有在没有Boss时才更新计时器
        // 根据难度等级调整出怪速度：难度越高，出怪越快
        // 基础间隔360帧（6秒，延长1秒），每级难度减少20帧，最低100帧（约1.67秒）
        const int baseSpawnInterval = 360; // 基础出怪间隔（6秒，延长1秒）
        int currentSpawnInterval = qMax(100, baseSpawnInterval - difficultyLevel * 20);
        
        // 真空期状态：两波之间间隔1秒（60帧）
        if (m_spawnInVacuumPeriod) {
            m_spawnVacuumTimer++;
            if (m_spawnVacuumTimer >= 60) { // 1秒真空期结束
                m_spawnInVacuumPeriod = false;
                m_spawnVacuumTimer = 0;
                enemyGroupSpawnTimer = 0; // 重置计时器，开始新一波
            }
        } else {
            // 检查是否所有敌机都被击毁
            bool allEnemiesDestroyed = enemies.isEmpty();
            
            // 检查是否所有敌机都离开了屏幕（y > arenaHeight）
            bool allEnemiesLeftScreen = true;
            for (const Enemy &enemy : enemies) {
                if (enemy.pos.y() <= m_arenaHeight + 20) { // 还在屏幕内或刚离开
                    allEnemiesLeftScreen = false;
                    break;
                }
            }
            
            // 情况1：玩家清空了该波所有敌机，立即进入下一波
            if (allEnemiesDestroyed && enemyGroupSpawnTimer >= 60) {
                // 所有敌机都被击毁，立即进入下一波（跳过剩余时间）
                m_spawnInVacuumPeriod = true;
                m_spawnVacuumTimer = 0;
                enemyGroupSpawnTimer = 0;
                // 全局波次计数
                totalWaveCount++;

                // 每5波出现一次Boss（确保不会连续出现Boss）
                // 再次检查是否已经有Boss存在（包括警告和转换阶段），避免重复生成
                bool hasExistingBoss = false;
                for (const Boss &boss : bosses) {
                    if (boss.isActive || boss.isWarning || boss.layerTransitionTimer > 0) {
                        hasExistingBoss = true;
                        break;
                    }
                }
                
                // Boss生成条件：totalWaveCount % 5 == 4（即第5、10、15...波）
                if ((totalWaveCount % 5 == 4) && !hasExistingBoss) {
                    enemyWaveCount = 0; // 保持旧逻辑兼容（内部循环计数不再用于显示）
                    spawnBoss();
                } else if (!hasExistingBoss) {
                    // 兼容旧字段：用于内部节奏（不用于UI显示）
                    enemyWaveCount = totalWaveCount % 5;
                    spawnEnemy();
                }
                // 如果已有Boss存在，不生成新的，等待Boss被击败
            } else {
                // 正常计时
                enemyGroupSpawnTimer++;
                // 情况2：时间到达且所有敌机都离开场景后进入下一波
                if (enemyGroupSpawnTimer >= currentSpawnInterval) {
                    // 时间到达，检查是否所有敌机都离开了屏幕
                    if (allEnemiesLeftScreen || allEnemiesDestroyed) {
                        // 所有敌机都离开了屏幕或被击毁，进入真空期
                        m_spawnInVacuumPeriod = true;
                        m_spawnVacuumTimer = 0;
                        enemyGroupSpawnTimer = 0;
                        // 全局波次计数
                        totalWaveCount++;

                        // 每5波出现一次Boss（确保不会连续出现Boss）
                        // 再次检查是否已经有Boss存在（包括警告和转换阶段），避免重复生成
                        bool hasExistingBoss = false;
                        for (const Boss &boss : bosses) {
                            if (boss.isActive || boss.isWarning || boss.layerTransitionTimer > 0) {
                                hasExistingBoss = true;
                                break;
                            }
                        }
                        
                        // Boss生成条件：totalWaveCount % 5 == 4（即第5、10、15...波）
                        if ((totalWaveCount % 5 == 4) && !hasExistingBoss) {
                            enemyWaveCount = 0; // 保持旧逻辑兼容（内部循环计数不再用于显示）
                            spawnBoss();
                        } else if (!hasExistingBoss) {
                            // 兼容旧字段：用于内部节奏（不用于UI显示）
                            enemyWaveCount = totalWaveCount % 5;
                            spawnEnemy();
                        }
                        // 如果已有Boss存在，不生成新的，等待Boss被击败
                    }
                    // 如果时间到达但还有敌机在屏幕内，继续等待它们离开
                }
            }
        }
    } else {
        // Boss存在时重置计时器，避免Boss消失后立即生成多组敌机
        enemyGroupSpawnTimer = 0;
        m_spawnInVacuumPeriod = false;
        m_spawnVacuumTimer = 0;
    }
    
    // 更新等级（每1000分升一级）
    int newLevel = score / 1000 + 1;
    if (newLevel > level) {
        level = newLevel;
        // 提高难度
        enemySpawnInterval = qMax(500, 2000 - level * 100);
        if (enemySpawnTimerObj) {
            enemySpawnTimerObj->setInterval(enemySpawnInterval);
        }
    }
    
    // 检查游戏结束
    if (playerHealth <= 0) {
        gameState = GAME_OVER;
        if (gameTimer) {
            gameTimer->stop();
        }
        enemySpawnTimerObj->stop();
        bossSpawnTimerObj->stop();
    }

    update();
}

void ThunderFighterWindow::mousePressEvent(QMouseEvent *event)
{
    if (!event)
        return;
    if (event->button() == Qt::LeftButton && gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
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
        if (gameState == MENU) {
            resetGame();
            event->accept();
            update();
            return;
        }
        if (gameState == GAME_OVER) {
            resetGame();
            event->accept();
            update();
            return;
        }
    }
    if (gameState != PLAYING)
        return;
    if (event->button() != Qt::LeftButton)
        return;

    // 只允许在点击到战机附近时开始拖动（更符合“拖动战机移动”）
    const QPointF mousePos = event->position();
    const QPointF diff = mousePos - playerPos;
    const double r = 22.0; // 点击判定半径
    if (diff.x() * diff.x() + diff.y() * diff.y() <= r * r) {
        mouseDragging = true;
        mouseDragOffset = playerPos - mousePos; // 记录偏移，避免瞬移
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void ThunderFighterWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!event)
        return;
    if (gameState == PAUSED && m_pauseOverlay != PauseOverlayNone) {
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
    if (gameState != PLAYING)
        return;
    if (!mouseDragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPointF mousePos = event->position();
    QPointF target = mousePos + mouseDragOffset;

    // 约束到游戏区域内（与键盘移动边界一致）
    target.setX(qBound(20.0, target.x(), static_cast<double>(m_arenaWidth) - 20.0));
    target.setY(qBound(50.0, target.y(), static_cast<double>(m_arenaHeight) - 30.0));
    playerPos = target;

    event->accept();
}

void ThunderFighterWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!event) return;
    if (event->button() == Qt::LeftButton) {
        mouseDragging = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void ThunderFighterWindow::updatePlayer()
{
    // 键盘/摇杆位移由 tickPlayerMoveInput（8ms）负责；鼠标拖动由 mouseMoveEvent 直接改 playerPos

    // 检查Boss状态：如果Boss正在登场或中场切换，玩家停止发射子弹
    bool bossInTransition = false;
    for (const Boss &boss : bosses) {
        if (boss.isWarning || boss.layerTransitionTimer > 0) {
            bossInTransition = true;
            break;
        }
    }
    
    // 自动发射子弹（支持多弹道和不同轨迹）
    // Boss登场或中场切换时，玩家停止发射
    if (!bossInTransition && playerShootCooldown <= 0) {
        // 暴走：提高子弹速度
        double baseSpeed = 8.0 + playerLevel; // 基础速度
        if (berserkTimeLeft > 0) {
            baseSpeed += 6.0;
        }
        
        if (playerLevel == 0) {
            // 0级：三发直线（加强弹道数量）
            for (int i = -1; i <= 1; ++i) {
                    Bullet bullet;
                    bullet.pos = QPointF(playerPos.x() + i * 4, playerPos.y() - 15);
                    bullet.isPlayerBullet = true;
                    bullet.vx = i * 0.3;
                    bullet.vy = -baseSpeed;
                    bullet.penetration = 0;
                    bullet.colorType = -1; // 普通子弹，无颜色类型
                    bullet.ownerEnemyId = -1; // 玩家子弹，ID为-1
                    bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                playerBullets.append(bullet);
            }
        } else if (playerLevel == 1) {
            // 1级：五发扇形（加强弹道数量）
            for (int i = -2; i <= 2; ++i) {
                Bullet bullet;
                bullet.pos = QPointF(playerPos.x() + i * 6, playerPos.y() - 15);
                bullet.isPlayerBullet = true;
                bullet.vx = i * 1.2;
                bullet.vy = -baseSpeed;
                    bullet.penetration = 0;
                    bullet.colorType = -1; // 普通子弹，无颜色类型
                    bullet.ownerEnemyId = -1; // 玩家子弹，ID为-1
                    bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                playerBullets.append(bullet);
            }
        } else if (playerLevel == 2) {
            // 2级：七发扇形（加强弹道数量）
            for (int i = -3; i <= 3; ++i) {
                Bullet bullet;
                bullet.pos = QPointF(playerPos.x() + i * 7, playerPos.y() - 15);
                bullet.isPlayerBullet = true;
                bullet.vx = i * 0.9; // 减少散射范围
                bullet.vy = -baseSpeed;
                    bullet.penetration = 0;
                    bullet.colorType = -1; // 普通子弹，无颜色类型
                    bullet.ownerEnemyId = -1; // 玩家子弹，ID为-1
                    bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                playerBullets.append(bullet);
            }
        } else if (playerLevel == 3) {
            // 3级：九发扇形（加强弹道数量）
            for (int i = -4; i <= 4; ++i) {
                Bullet bullet;
                bullet.pos = QPointF(playerPos.x() + i * 7, playerPos.y() - 15);
                bullet.isPlayerBullet = true;
                bullet.vx = i * 0.75; // 减少散射范围
                bullet.vy = -baseSpeed;
                    bullet.penetration = 0;
                    bullet.colorType = -1; // 普通子弹，无颜色类型
                    bullet.ownerEnemyId = -1; // 玩家子弹，ID为-1
                    bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                playerBullets.append(bullet);
            }
        } else if (playerLevel == 4) {
            // 4级：增加发射子弹的僚机两个（平滑增长：主战机保持7发，僚机各3发）
            // 主战机：七发扇形（与3级相同，不增加）
            for (int i = -3; i <= 3; ++i) {
                Bullet bullet;
                bullet.pos = QPointF(playerPos.x() + i * 7, playerPos.y() - 15);
                bullet.isPlayerBullet = true;
                bullet.vx = i * 0.65; // 减少散射范围
                bullet.vy = -baseSpeed;
                bullet.penetration = 0;
                bullet.colorType = -1; // 普通子弹，无颜色类型
                bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                playerBullets.append(bullet);
            }
            
            // 两个僚机位置（左右各一个）
            QPointF leftWingman = QPointF(playerPos.x() - 30, playerPos.y() + 5);
            QPointF rightWingman = QPointF(playerPos.x() + 30, playerPos.y() + 5);
            
            // 左僚机发射（三发，平滑增长）
            for (int j = -1; j <= 1; ++j) {
                Bullet bullet;
                bullet.pos = QPointF(leftWingman.x() + j * 4, leftWingman.y() - 10);
                bullet.isPlayerBullet = true;
                bullet.vx = j * 0.18; // 减少散射范围
                bullet.vy = -baseSpeed;
                bullet.penetration = 0;
                bullet.colorType = -1; // 普通子弹，无颜色类型
                bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                playerBullets.append(bullet);
            }
            
            // 右僚机发射（三发，平滑增长）
            for (int j = -1; j <= 1; ++j) {
                Bullet bullet;
                bullet.pos = QPointF(rightWingman.x() + j * 4, rightWingman.y() - 10);
                bullet.isPlayerBullet = true;
                bullet.vx = j * 0.18; // 减少散射范围
                bullet.vy = -baseSpeed;
                bullet.penetration = 0;
                bullet.colorType = -1; // 普通子弹，无颜色类型
                bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                playerBullets.append(bullet);
            }
        } else if (playerLevel >= 5) {
            // 5级：僚机增加到四个，所有子弹都可穿透，并定时发射持续激光
            // 主战机：九发扇形（所有子弹可穿透，平滑增长：从7发增加到9发）
            for (int i = -4; i <= 4; ++i) {
                Bullet bullet;
                bullet.pos = QPointF(playerPos.x() + i * 6, playerPos.y() - 15);
                bullet.isPlayerBullet = true;
                bullet.vx = i * 0.95; // 减少散射范围
                bullet.vy = -baseSpeed;
                bullet.penetration = (berserkTimeLeft > 0) ? 8 : 5; // 暴走时穿透增强，但不过高
                bullet.colorType = -1; // 普通子弹，无颜色类型
                bullet.ownerEnemyId = -1; // 玩家子弹，ID为-1
                bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                playerBullets.append(bullet);
            }

            // 暴走：不再额外增加弹幕，只通过暴走弹幕系统增强火力
            
            // 四个僚机位置（正三角雁阵型：上方左右各一个，下方左右各一个，横向拉开）
            QPointF wingman1 = QPointF(playerPos.x() - 30, playerPos.y() - 20); // 上方左侧
            QPointF wingman2 = QPointF(playerPos.x() + 30, playerPos.y() - 20); // 上方右侧
            QPointF wingman3 = QPointF(playerPos.x() - 45, playerPos.y() + 15); // 下方左侧（横向拉开）
            QPointF wingman4 = QPointF(playerPos.x() + 45, playerPos.y() + 15); // 下方右侧（横向拉开）
            
            // 四个僚机发射普通子弹（每个僚机三发，所有子弹可穿透，平滑增长）
            QList<QPointF> wingmanList = {wingman1, wingman2, wingman3, wingman4};
            for (const QPointF &wingmanPos : wingmanList) {
                // 平滑增长：5级时僚机保持3发，暴走时不增加弹幕数量，只通过暴走弹幕系统增强
                for (int j = -1; j <= 1; ++j) {
                    Bullet bullet;
                    bullet.pos = QPointF(wingmanPos.x() + j * 4, wingmanPos.y() - 10);
                    bullet.isPlayerBullet = true;
                    bullet.vx = j * 0.30; // 减少散射范围
                    bullet.vy = -baseSpeed;
                    bullet.penetration = (berserkTimeLeft > 0) ? 7 : 5; // 暴走时穿透增强，但不过高
                    bullet.colorType = -1; // 普通子弹，无颜色类型
                    bullet.ownerEnemyId = -1; // 玩家子弹，ID为-1
                    bullet.hitDamage = damageForPlayerBullet(bullet.colorType);
                    playerBullets.append(bullet);
                }
            }
            
        }
        
        // 固定射速，暴走时增加30%射速（冷却时间减少30%）
        if (berserkTimeLeft > 0) {
            playerShootCooldown = qMax(1, qRound(shootCooldownBase * 0.7)); // 暴走时射速增加30%
        } else {
            playerShootCooldown = shootCooldownBase; // 固定射速，不随等级变化
        }
    }
    if (playerShootCooldown > 0) {
        playerShootCooldown--;
    }
    
    // 暴走状态：召唤5个浮游炮（Boss转场期间保留浮游炮，仅停止攻击）
    if (berserkTimeLeft > 0) {
        // 如果浮游炮数量不足5个，创建新的浮游炮
        while (drones.size() < 5) {
            Drone drone;
            drone.pos = playerPos;
            drone.targetPos = playerPos;
            drone.targetEnemyId = -1;
            drone.targetBossIndex = -1;
            drone.state = 0; // 待机状态
            drone.orbitTimer = 0;
            /* 错开各炮的首攻时刻，避免五门炮同一帧发射激光导致碰撞循环叠乘 */
            drone.laserAttackTimer = 22 + static_cast<int>(drones.size()) * 8;
            drone.laserVisualTimer = 0;
            drone.laserStartPos = playerPos;
            drone.laserEndPos = playerPos;
            drone.orbitAngle = 0.0;
            drone.orbitRadius = 0.0;
            drone.orbitSpeed = 0.0;
            drone.angle = -M_PI / 2.0; // 初始朝向向上
            drones.append(drone);
        }
    } else {
        // 暴走结束，清除所有浮游炮
        drones.clear();
    }
    
    // 更新僚机位置（跟随玩家）
    if (playerLevel >= 4) {
        wingmanPositions.clear();
        if (playerLevel == 4) {
            // 两个僚机
            wingmanPositions.append(QPointF(playerPos.x() - 30, playerPos.y() + 5));
            wingmanPositions.append(QPointF(playerPos.x() + 30, playerPos.y() + 5));
        } else if (playerLevel >= 5) {
            // 四个僚机（正三角雁阵型：上方左右各一个，下方左右各一个，横向拉开）
            wingmanPositions.append(QPointF(playerPos.x() - 30, playerPos.y() - 20)); // 上方左侧
            wingmanPositions.append(QPointF(playerPos.x() + 30, playerPos.y() - 20)); // 上方右侧
            wingmanPositions.append(QPointF(playerPos.x() - 45, playerPos.y() + 15)); // 下方左侧（横向拉开）
            wingmanPositions.append(QPointF(playerPos.x() + 45, playerPos.y() + 15)); // 下方右侧（横向拉开）
        }
    }
}

void ThunderFighterWindow::updateBullets()
{
    // 更新玩家子弹（使用向量移动）
    for (int i = playerBullets.size() - 1; i >= 0; --i) {
        // 5级时的穿透子弹正常移动（不再是激光）
        playerBullets[i].pos.rx() += playerBullets[i].vx;
        playerBullets[i].pos.ry() += playerBullets[i].vy;
        
        // 移除超出屏幕的子弹
        if (playerBullets[i].pos.y() < -10 || 
            playerBullets[i].pos.y() > m_arenaHeight + 10 ||
            playerBullets[i].pos.x() < -10 ||
            playerBullets[i].pos.x() > m_arenaWidth + 10) {
            playerBullets.removeAt(i);
        }
    }
    
    // 更新敌机子弹（使用向量移动）
    for (int i = enemyBullets.size() - 1; i >= 0; --i) {
        enemyBullets[i].pos.rx() += enemyBullets[i].vx;
        enemyBullets[i].pos.ry() += enemyBullets[i].vy;
        
        // 移除超出屏幕的子弹
        if (enemyBullets[i].pos.y() > m_arenaHeight + 10 ||
            enemyBullets[i].pos.y() < -10 ||
            enemyBullets[i].pos.x() < -10 ||
            enemyBullets[i].pos.x() > m_arenaWidth + 10) {
            enemyBullets.removeAt(i);
        }
    }

    while (playerBullets.size() > kMaxPlayerBulletsSafety) {
        playerBullets.removeFirst();
    }
    while (enemyBullets.size() > kMaxEnemyBulletsSafety) {
        enemyBullets.removeFirst();
    }
}

void ThunderFighterWindow::updateEnemies()
{
    for (int i = enemies.size() - 1; i >= 0; --i) {
        // 先检查是否需要撤退（在位置更新之前）
        bool bossExistsForRetreat = false;
        for (const Boss &boss : bosses) {
            if (boss.isActive || boss.isWarning || boss.layerTransitionTimer > 0) {
                bossExistsForRetreat = true;
                break;
            }
        }
        bool shouldRetreat = false;
        if (!bossExistsForRetreat) {
            const int baseSpawnInterval = 360; // 基础出怪间隔（6秒）
            int currentSpawnInterval = qMax(100, baseSpawnInterval - difficultyLevel * 20);
            shouldRetreat = (enemyGroupSpawnTimer >= currentSpawnInterval - 60);
        }
        
        // 如果敌机需要撤退，设置撤退标志并开始向下移动
        if (shouldRetreat && enemies[i].hasReachedTarget) {
            // 敌机开始撤退，不再固定位置到targetPos
            enemies[i].hasReachedTarget = false; // 允许位置更新
            enemies[i].speed = 5.0; // 向下移动速度
            enemies[i].vx = 0.0; // 停止横向移动
        }
        
        // 敌机快速移动到屏幕中央并按队形排列（或撤退）
        if (!enemies[i].hasReachedTarget) {
            if (shouldRetreat) {
                // 撤退状态：直接向下移动
                enemies[i].pos.ry() += enemies[i].speed;
            } else {
                // 正常状态：移动到目标位置
                QPointF diff = enemies[i].targetPos - enemies[i].pos;
                double distance = qSqrt(diff.x() * diff.x() + diff.y() * diff.y());
                
                // 大幅增加移动速度（快速移动到目标位置）
                double moveSpeed = 8.0;
                
                // 如果距离小于移动速度，直接设置到目标位置，避免超调导致抖动
                if (distance <= moveSpeed) {
                    // 到达目标位置
                    enemies[i].pos = enemies[i].targetPos;
                    enemies[i].hasReachedTarget = true;
                    enemies[i].speed = 0.0; // 到达后停止移动
                    enemies[i].vx = 0.0;
                } else {
                    // 正常移动
                    double moveX = diff.x() / distance * moveSpeed;
                    double moveY = diff.y() / distance * moveSpeed;
                    enemies[i].pos.rx() += moveX;
                    enemies[i].pos.ry() += moveY;
                }
            }
        } else {
            // 已到达目标位置，完全停止移动，保持位置固定在目标位置
            // 每帧都确保位置精确等于目标位置，避免浮点数误差累积
            enemies[i].pos = enemies[i].targetPos;
            enemies[i].speed = 0.0;
            enemies[i].vx = 0.0;
        }
        
        // 检测敌机是否进入屏幕（屏幕范围：x在[0, arenaW]，y在[0, arenaH]）
        int enemySize = 15 + enemies[i].type * 5;
        bool isInScreen = (enemies[i].pos.x() + enemySize >= 0 && 
                          enemies[i].pos.x() - enemySize <= m_arenaWidth &&
                          enemies[i].pos.y() + enemySize >= 0 && 
                          enemies[i].pos.y() - enemySize <= m_arenaHeight);
        
        if (isInScreen) {
            // 敌机在屏幕内
            if (enemies[i].enterScreenTimer < 0) {
                // 刚进入屏幕，开始计时
                enemies[i].enterScreenTimer = 0;
            } else {
                // 已在屏幕内，增加计时器
                enemies[i].enterScreenTimer++;
            }
        }
        
        // 侧面平移的敌机：到达屏幕边界后反弹（仅在未到达目标时）
        if (!enemies[i].hasReachedTarget && enemies[i].spawnMode == 1 && enemies[i].vx != 0.0) {
            if (enemies[i].pos.x() < 0 && enemies[i].vx < 0) {
                enemies[i].vx = -enemies[i].vx; // 反弹
            } else if (enemies[i].pos.x() > m_arenaWidth && enemies[i].vx > 0) {
                enemies[i].vx = -enemies[i].vx; // 反弹
            }
        }
        
        // 敌机射击模式切换（每2秒切换一次，120帧）
        enemies[i].shootPatternTimer++;
        if (enemies[i].shootPatternTimer >= 120) { // 2秒 = 120帧
            enemies[i].shootPatternTimer = 0;
            enemies[i].shootPattern = (enemies[i].shootPattern + 1) % 4; // 循环切换4种模式
        }
        
        // 检查Boss状态：如果Boss正在登场或中场切换，敌机停止发射子弹
        bool bossInTransition = false;
        for (const Boss &boss : bosses) {
            if (boss.isWarning || boss.layerTransitionTimer > 0) {
                bossInTransition = true;
                break;
            }
        }
        
        // 如果敌机正在撤退（shouldRetreat已在位置更新逻辑中计算），停止攻击
        if (shouldRetreat && enemies[i].hasReachedTarget) {
            // 到达下一波出场时间前1秒，或一波已结束，往屏幕下方移动离开屏幕
            enemies[i].speed = 5.0; // 向下移动速度
            enemies[i].vx = 0.0; // 停止横向移动
            enemies[i].pos.ry() += enemies[i].speed; // 向下移动
            // 停止攻击（不执行射击逻辑）
            continue; // 跳过射击逻辑
        }
        
        // 敌机射击（每隔一段时间发射一定量花样的子弹弹幕）
        // Boss登场或中场切换时，敌机停止发射
        // 撤退期间也停止发射
        if (!bossInTransition && !shouldRetreat) {
            enemies[i].shootCooldown--;
            int shootInterval = qMax(80, 120 - difficultyLevel * 5); // 基础2秒，难度越高间隔越短（降低增长曲线）
            if (enemies[i].shootCooldown <= 0) {
                double baseSpeed = 1.0 + enemies[i].type * 0.5 + difficultyLevel * 0.15; // 速度随难度增加（降低增长曲线）
            
            // 根据当前射击模式发射弹幕
            switch (enemies[i].shootPattern) {
            case 0: // 直线发射（密集直线）
                {
                    int bulletCount = 3 + difficultyLevel / 2; // 难度越高子弹越多
                    for (int j = -(bulletCount/2); j <= (bulletCount/2); ++j) {
                        Bullet bullet;
                        bullet.pos = QPointF(enemies[i].pos.x() + j * 5, enemies[i].pos.y() + 10);
                        bullet.isPlayerBullet = false;
                        bullet.vx = j * 0.1;
                        bullet.vy = baseSpeed;
                        bullet.colorType = -1; // 敌机子弹，无颜色类型
                        bullet.ownerEnemyId = enemies[i].enemyId; // 记录发射者ID
                        bullet.hitDamage = kEnemyBulletDamageToPlayer;
                        enemyBullets.append(bullet);
                    }
                }
                break;
            case 1: // 扇形发射
                {
                    int bulletCount = 5 + difficultyLevel; // 难度越高子弹越多
                    for (int j = -(bulletCount/2); j <= (bulletCount/2); ++j) {
                        Bullet bullet;
                        bullet.pos = QPointF(enemies[i].pos.x(), enemies[i].pos.y() + 10);
                        bullet.isPlayerBullet = false;
                        bullet.vx = j * 0.6;
                        bullet.vy = baseSpeed;
                        bullet.colorType = -1; // 敌机子弹，无颜色类型
                        bullet.ownerEnemyId = enemies[i].enemyId; // 记录发射者ID
                        bullet.hitDamage = kEnemyBulletDamageToPlayer;
                        enemyBullets.append(bullet);
                    }
                }
                break;
            case 2: // 圆形发射
                {
                    int count = 8 + difficultyLevel * 2; // 难度越高子弹越多
                    for (int j = 0; j < count; ++j) {
                        double angle = j * 2 * M_PI / count + M_PI / 2; // 向下为主方向
                        Bullet bullet;
                        bullet.pos = QPointF(enemies[i].pos.x(), enemies[i].pos.y() + 10);
                        bullet.isPlayerBullet = false;
                        bullet.vx = qCos(angle) * baseSpeed * 0.5;
                        bullet.vy = qSin(angle) * baseSpeed;
                        bullet.colorType = -1; // 敌机子弹，无颜色类型
                        bullet.ownerEnemyId = enemies[i].enemyId; // 记录发射者ID
                        bullet.hitDamage = kEnemyBulletDamageToPlayer;
                        enemyBullets.append(bullet);
                    }
                }
                break;
            case 3: // 密集扇形（多排）
                {
                    int rowCount = 2 + difficultyLevel / 2; // 难度越高排数越多
                    for (int row = 0; row < rowCount; ++row) {
                        int bulletCount = 3 + difficultyLevel / 2;
                        for (int j = -(bulletCount/2); j <= (bulletCount/2); ++j) {
                            Bullet bullet;
                            bullet.pos = QPointF(enemies[i].pos.x() + j * 8, enemies[i].pos.y() + 10 + row * 5);
                            bullet.isPlayerBullet = false;
                            bullet.vx = j * 0.8;
                            bullet.vy = baseSpeed;
                            bullet.colorType = -1; // 敌机子弹，无颜色类型
                            bullet.ownerEnemyId = enemies[i].enemyId; // 记录发射者ID
                            bullet.hitDamage = kEnemyBulletDamageToPlayer;
                            enemyBullets.append(bullet);
                        }
                    }
                }
                break;
            }
            
            enemies[i].shootCooldown = shootInterval;
            }
        }
        
        // 移除超出屏幕的敌机
        if (enemies[i].pos.y() > m_arenaHeight + 20) {
            enemies.removeAt(i);
        }
    }
}

void ThunderFighterWindow::updateBosses()
{
    for (int i = bosses.size() - 1; i >= 0; --i) {
        // 处理警告阶段
        if (bosses[i].isWarning) {
            // Boss出场时清空一次屏幕内所有子弹（仅在警告阶段开始时）
            static QMap<int, bool> bossWarningCleared; // 为每个Boss索引记录是否已清空
            if (!bossWarningCleared.contains(i) || bosses[i].warningTimer == 120) {
                if (bosses[i].warningTimer == 120) { // 警告阶段开始时（第一帧）
                    enemyBullets.clear();
                    playerBullets.clear();
                    bossWarningCleared[i] = true;
                }
            }
            
            bosses[i].warningTimer--;
            
            // Boss登场移动动画：从屏幕外平滑移动到战斗位置
            QPointF targetPos = QPointF(m_arenaWidth / 2, 100); // 战斗位置
            QPointF diff = targetPos - bosses[i].pos;
            double distance = qSqrt(diff.x() * diff.x() + diff.y() * diff.y());
            
            if (distance > 5.0) {
                // 使用缓动函数实现平滑移动动画（ease-out效果）
                double moveSpeed = 8.0; // 移动速度
                double moveX = diff.x() / distance * moveSpeed;
                double moveY = diff.y() / distance * moveSpeed;
                bosses[i].pos.rx() += moveX;
                bosses[i].pos.ry() += moveY;
            } else {
                // 到达目标位置
                bosses[i].pos = targetPos;
            }
            
            if (bosses[i].warningTimer <= 0) {
                bosses[i].isWarning = false;
                bosses[i].isActive = true;
                bosses[i].pos = targetPos; // 确保精确到达战斗位置
                static QMap<int, bool> bossWarningCleared;
                bossWarningCleared.remove(i); // 清除记录
            }
            continue; // 警告阶段不执行其他逻辑
        }
        
        // 处理层转换等待阶段（双血条Boss第一层清空后）- 必须在isActive检查之前
        if (bosses[i].layerTransitionTimer > 0) {
            // Boss中场切换时清空一次屏幕内所有子弹（仅在层转换开始时）
            static QMap<int, bool> bossTransitionCleared; // 为每个Boss索引记录是否已清空
            if (!bossTransitionCleared.contains(i) || bosses[i].layerTransitionTimer == 120) {
                if (bosses[i].layerTransitionTimer == 120) { // 层转换开始时（第一帧）
                    enemyBullets.clear();
                    playerBullets.clear();
                    bossTransitionCleared[i] = true;
                }
            }
            
            bosses[i].layerTransitionTimer--;
            if (bosses[i].layerTransitionTimer <= 0) {
                // 等待结束，进入第二层
                bosses[i].layer--;
                bosses[i].health = bosses[i].maxHealth;
                bosses[i].isActive = true; // 重新激活
                // 清除所有敌机子弹
                enemyBullets.clear();
                static QMap<int, bool> bossTransitionCleared;
                bossTransitionCleared.remove(i); // 清除记录
            }
            // 等待期间Boss保持在场上原地不动不攻击，但继续绘制
            continue; // 等待期间不执行移动和攻击逻辑
        }
        
        if (!bosses[i].isActive) {
            continue;
        }
        
        // Boss左右移动（仅在激活状态下移动）
        bosses[i].pos.rx() += bosses[i].speed;
        if (bosses[i].pos.x() < 50 || bosses[i].pos.x() > m_arenaWidth - 50) {
            bosses[i].speed = -bosses[i].speed; // 反向
        }
        
        // Boss射击模式切换（每3秒切换一次，180帧）
        bosses[i].shootPatternTimer++;
        if (bosses[i].shootPatternTimer >= 180) { // 3秒 = 180帧
            bosses[i].shootPatternTimer = 0;
            bosses[i].shootPattern = (bosses[i].shootPattern + 1) % 5; // 循环切换5种模式（增加更多花样）
        }
        
        // Boss射击（根据当前射击模式和层数）- 断续射击机制
        // 断续射击：连续发射3-5发，然后停顿一段时间
        int burstSize = 3 + difficultyLevel / 2; // 连发数量，难度越高连发越多
        burstSize = qMin(burstSize, 5); // 最多5发连发
        int burstPause = 90 + difficultyLevel * 10; // 连发间隔，难度越高间隔越短
        
        // 处理连发间隔冷却
        if (bosses[i].burstCooldown > 0) {
            bosses[i].burstCooldown--;
            // 冷却期间不发射
        } else {
            // 连发阶段
            bosses[i].shootCooldown--;
            // 第二层难度增加：射击间隔更短，射速更快
            int baseShootInterval = qMax(8, 15 - difficultyLevel); // 连发内的射击间隔
            int shootInterval = (bosses[i].layer == 1 && bosses[i].maxLayer == 2) ? 
                               qMax(5, baseShootInterval - 3) : baseShootInterval; // 第二层射击间隔更短
            double speedMultiplier = (bosses[i].layer == 1 && bosses[i].maxLayer == 2) ? 1.5 : 1.0; // 第二层速度增加50%
            int patternCount = (bosses[i].layer == 1 && bosses[i].maxLayer == 2) ? 2 : 0; // 第二层弹道数量增加更多
            
            if (bosses[i].shootCooldown <= 0) {
                double baseSpeed = (2.0 + difficultyLevel * 0.3) * speedMultiplier;
                
                // 根据当前射击模式发射
                switch (bosses[i].shootPattern) {
                case 0: // 线性发射（密集直线）
                    {
                        int bulletCount = 5 + patternCount; // 第二层增加弹道
                        for (int j = -(bulletCount/2); j <= (bulletCount/2); ++j) {
                            Bullet bullet;
                            bullet.pos = QPointF(bosses[i].pos.x() + j * 8, bosses[i].pos.y() + 50);
                            bullet.isPlayerBullet = false;
                            bullet.vx = j * 0.2;
                            bullet.vy = baseSpeed;
                            bullet.colorType = -1; // 敌机子弹，无颜色类型
                            bullet.ownerEnemyId = -1; // Boss子弹，ID为-1
                            bullet.hitDamage = kEnemyBulletDamageToPlayer;
                            enemyBullets.append(bullet);
                        }
                    }
                    break;
                case 1: // 扇形发射
                    {
                        int bulletCount = 7 + patternCount * 2; // 第二层增加弹道
                        for (int j = -(bulletCount/2); j <= (bulletCount/2); ++j) {
                            Bullet bullet;
                            bullet.pos = QPointF(bosses[i].pos.x() + j * 10, bosses[i].pos.y() + 50);
                            bullet.isPlayerBullet = false;
                            bullet.vx = j * 0.8;
                            bullet.vy = baseSpeed;
                            bullet.colorType = -1; // 敌机子弹，无颜色类型
                            bullet.ownerEnemyId = -1; // Boss子弹，ID为-1
                            bullet.hitDamage = kEnemyBulletDamageToPlayer;
                            enemyBullets.append(bullet);
                        }
                    }
                    break;
                case 2: // 圆形发射
                    {
                        int count = 16 + patternCount * 4; // 第二层增加4发
                        for (int j = 0; j < count; ++j) {
                            double angle = j * 2 * M_PI / count + M_PI / 2;
                            Bullet bullet;
                            bullet.pos = QPointF(bosses[i].pos.x(), bosses[i].pos.y() + 50);
                            bullet.isPlayerBullet = false;
                            bullet.vx = qCos(angle) * baseSpeed * 0.6;
                            bullet.vy = qSin(angle) * baseSpeed;
                            bullet.colorType = -1; // 敌机子弹，无颜色类型
                            bullet.ownerEnemyId = -1; // Boss子弹，ID为-1
                            bullet.hitDamage = kEnemyBulletDamageToPlayer;
                            enemyBullets.append(bullet);
                        }
                    }
                    break;
                case 3: // 密集扇形（多排）
                    {
                        int rowCount = 2 + patternCount; // 第二层增加排数
                        for (int row = 0; row < rowCount; ++row) {
                            int bulletCount = 5 + patternCount;
                            for (int j = -(bulletCount/2); j <= (bulletCount/2); ++j) {
                                Bullet bullet;
                                bullet.pos = QPointF(bosses[i].pos.x() + j * 12, bosses[i].pos.y() + 50 + row * 5);
                                bullet.isPlayerBullet = false;
                                bullet.vx = j * 1.2;
                                bullet.vy = baseSpeed;
                                bullet.colorType = -1; // 敌机子弹，无颜色类型
                                bullet.ownerEnemyId = -1; // Boss子弹，ID为-1
                                bullet.hitDamage = kEnemyBulletDamageToPlayer;
                                enemyBullets.append(bullet);
                            }
                        }
                    }
                    break;
                case 4: // 螺旋发射（新增模式）
                    {
                        // 使用静态变量存储螺旋角度，每次射击时旋转
                        static QMap<int, double> spiralAngles; // 为每个boss索引保存角度
                        if (!spiralAngles.contains(i)) {
                            spiralAngles[i] = 0.0;
                        }
                        spiralAngles[i] += 0.3; // 旋转角度
                        if (spiralAngles[i] >= 2 * M_PI) {
                            spiralAngles[i] -= 2 * M_PI;
                        }
                        
                        int count = 8 + patternCount * 2;
                        for (int j = 0; j < count; ++j) {
                            double angle = spiralAngles[i] + j * 2 * M_PI / count;
                            Bullet bullet;
                            bullet.pos = QPointF(bosses[i].pos.x(), bosses[i].pos.y() + 50);
                            bullet.isPlayerBullet = false;
                            bullet.vx = qCos(angle) * baseSpeed * 0.7;
                            bullet.vy = qSin(angle) * baseSpeed;
                            bullet.colorType = -1; // 敌机子弹，无颜色类型
                            bullet.ownerEnemyId = -1; // Boss子弹，ID为-1
                            bullet.hitDamage = kEnemyBulletDamageToPlayer;
                            enemyBullets.append(bullet);
                        }
                    }
                    break;
                }
                
                bosses[i].shootCooldown = shootInterval;
                bosses[i].burstCount++;
                
                // 如果达到连发数量，进入停顿阶段
                if (bosses[i].burstCount >= burstSize) {
                    bosses[i].burstCount = 0;
                    bosses[i].burstCooldown = burstPause; // 停顿一段时间
                }
            }
        }
        
        // 检查Boss血条清空
        if (bosses[i].health <= 0) {
            // 清除所有敌机子弹
            enemyBullets.clear();
            
            if (bosses[i].layer > 1) {
                // 还有下一层，进入等待阶段（2秒）
                bosses[i].isActive = false; // 暂时停用
                bosses[i].layerTransitionTimer = 120; // 2秒 = 120帧
                
                // 双血条Boss中场切换：固定掉落一个升级道具 + 一个护盾道具
                spawnPowerUp(bosses[i].pos, 0);
                spawnPowerUp(bosses[i].pos, 1);
            } else {
                // 所有层都清空，Boss被击败
                addExplosion(bosses[i].pos);
                score += 500; // Boss被击败获得高分
                bossKillCount++;
                difficultyLevel++; // 增加难度等级
                
                // Boss不走概率掉落：仅"双血条Boss中场切换"固定掉落（见上方）
                bosses.removeAt(i);
                
                // Boss被击败后，更新波次计数，确保下一波是普通敌机而不是Boss
                // Boss算作一波，所以击败后应该进入下一波（普通敌机）
                // 但需要等待当前波次时间结束，所以不立即更新totalWaveCount
                // 而是在下一波生成时，totalWaveCount会自然增加
            }
        } else if (bosses[i].pos.y() > m_arenaHeight + 50) {
            // Boss移出屏幕，重新启动Boss生成定时器
            bosses.removeAt(i);
            if (bossSpawnTimerObj && !bossSpawnTimerObj->isActive()) {
                bossSpawnTimer = 0;
                bossSpawnTimerObj->start(bossSpawnInterval);
            }
        }
    }
}

void ThunderFighterWindow::updateExplosions()
{
    for (int i = explosions.size() - 1; i >= 0; --i) {
        explosions[i].frame++;
        if (explosions[i].frame >= explosions[i].maxFrames) {
            explosions.removeAt(i);
        }
    }
}

void ThunderFighterWindow::checkCollisions()
{
    // 检查Boss状态：如果Boss正在登场或中场切换，某些功能停止
    bool bossInTransition = false;
    for (const Boss &boss : bosses) {
        if (boss.isWarning || boss.layerTransitionTimer > 0) {
            bossInTransition = true;
            break;
        }
    }
    
    // 激光持续伤害检测（在4/5级且激光激活时）
    if (playerLevel >= 4 && laserDuration > 0) {
        QList<QPointF> currentWingmanPositions;
        if (playerLevel == 4) {
            // 4级：两个僚机
            currentWingmanPositions.append(QPointF(playerPos.x() - 30, playerPos.y() + 5));
            currentWingmanPositions.append(QPointF(playerPos.x() + 30, playerPos.y() + 5));
        } else if (playerLevel >= 5) {
            // 5级：四个僚机（正三角雁阵型：上方左右各一个，下方左右各一个，横向拉开）
            currentWingmanPositions.append(QPointF(playerPos.x() - 30, playerPos.y() - 20)); // 上方左侧
            currentWingmanPositions.append(QPointF(playerPos.x() + 30, playerPos.y() - 20)); // 上方右侧
            currentWingmanPositions.append(QPointF(playerPos.x() - 45, playerPos.y() + 15)); // 下方左侧（横向拉开）
            currentWingmanPositions.append(QPointF(playerPos.x() + 45, playerPos.y() + 15)); // 下方右侧（横向拉开）
        }
        
        // 每帧对激光路径上的敌人造成高额穿透伤害（每3帧造成一次伤害）
        static int laserDamageTimer = 0;
        laserDamageTimer++;
        if (laserDamageTimer >= 3) { // 每3帧造成一次伤害
            laserDamageTimer = 0;

            double laserDamageFloat = 1.0 * 1.15;
            if (berserkTimeLeft > 0)
                laserDamageFloat *= 1.5;
            const int laserTickDamage = qRound(laserDamageFloat);

            const int nEnemiesLaser = enemies.size();
            const int nBossesLaser = bosses.size();
            QVector<int> enemyLaserSum(nEnemiesLaser, 0);
            QVector<int> bossLaserSum(nBossesLaser, 0);

            /* 激光伤害仅在 GUI 线程累计，避免 QtConcurrent 跨线程写 QVector 在 MSVC Debug 下触发堆损坏 */
            for (const QPointF &wingmanPos : currentWingmanPositions) {
                const QPointF laserStart = wingmanPos;
                const QPointF laserEnd = QPointF(wingmanPos.x(), 0);

                for (int j = 0; j < nEnemiesLaser; ++j) {
                    if (j >= enemies.size()) {
                        break;
                    }
                    if (checkLaserEnemyCollision(laserStart, laserEnd, enemies.at(j))) {
                        enemyLaserSum[j] += laserTickDamage;
                    }
                }
                for (int j = 0; j < nBossesLaser; ++j) {
                    if (j >= bosses.size()) {
                        break;
                    }
                    if (bosses.at(j).isActive && checkLaserBossCollision(laserStart, laserEnd, bosses.at(j))) {
                        bossLaserSum[j] += laserTickDamage;
                    }
                }
            }

            for (int j = enemies.size() - 1; j >= 0; --j) {
                if (j < 0 || j >= enemyLaserSum.size())
                    continue;
                const int tick = enemyLaserSum.at(j);
                if (tick <= 0)
                    continue;
                enemies[j].health -= tick;
                if (enemies[j].health <= 0) {
                    QPointF killedEnemyPos = enemies[j].pos;
                    addExplosion(killedEnemyPos);
                    score += (enemies[j].type + 1) * 10;

                    if (berserkTimeLeft > 0) {
                        int killedEnemyId = enemies[j].enemyId;
                        for (int k = enemyBullets.size() - 1; k >= 0; --k) {
                            if (enemyBullets[k].ownerEnemyId == killedEnemyId)
                                enemyBullets.removeAt(k);
                        }
                    }

                    tryDropUpgradeAndShield(killedEnemyPos);
                    enemies.removeAt(j);
                }
            }

            for (int j = bosses.size() - 1; j >= 0; --j) {
                if (j < 0 || j >= bossLaserSum.size())
                    continue;
                const int tick = bossLaserSum.at(j);
                if (tick <= 0)
                    continue;
                bosses[j].health -= tick;
            }
        }
    }
    
    // 浮游炮攻击：锁定目标后每0.5秒发射一次穿透激光（Boss转场期间停火）
    if (berserkTimeLeft > 0 && !bossInTransition) {
        const int nEnemiesDrone = enemies.size();
        const int nBossesDrone = bosses.size();
        QVector<int> droneEnemyDmg(nEnemiesDrone, 0);
        QVector<int> droneBossDmg(nBossesDrone, 0);
        constexpr int kDroneLaserDamage = 1;

        for (int i = 0; i < drones.size(); ++i) {
            Drone &drone = drones[i];
            // 锁定后即可攻击：追击态(state=1)与环绕态(state=2)均可发射
            if (drone.state != 1 && drone.state != 2) {
                continue;
            }

            QPointF targetPos;
            bool targetExists = false;
            if (drone.targetBossIndex >= 0 && drone.targetBossIndex < bosses.size() && bosses[drone.targetBossIndex].isActive) {
                targetPos = bosses[drone.targetBossIndex].pos;
                targetExists = true;
            } else if (drone.targetEnemyId >= 0) {
                for (const Enemy &enemy : enemies) {
                    if (enemy.enemyId == drone.targetEnemyId) {
                        targetPos = enemy.pos;
                        targetExists = true;
                        break;
                    }
                }
            }

            if (!targetExists) {
                continue;
            }

            drone.laserAttackTimer--;
            if (drone.laserAttackTimer > 0) {
                continue;
            }
            drone.laserAttackTimer = 30; // 0.5秒一次

            QPointF laserStart = drone.pos;
            QPointF direction = targetPos - drone.pos;
            double dirLen = qSqrt(direction.x() * direction.x() + direction.y() * direction.y());
            if (dirLen < 0.0001) {
                continue;
            }
            direction = QPointF(direction.x() / dirLen, direction.y() / dirLen);
            QPointF laserEnd = laserStart + direction * 2000.0; // 延长到屏幕外，形成穿透激光
            drone.laserStartPos = laserStart;
            drone.laserEndPos = laserEnd;
            drone.laserVisualTimer = 18; // 0.3秒轨迹

            for (int j = 0; j < nEnemiesDrone; ++j) {
                if (j >= enemies.size())
                    break;
                if (checkLaserEnemyCollision(laserStart, laserEnd, enemies.at(j)))
                    droneEnemyDmg[j] += kDroneLaserDamage;
            }
            for (int j = 0; j < nBossesDrone; ++j) {
                if (j >= bosses.size())
                    break;
                if (bosses.at(j).isActive && checkLaserBossCollision(laserStart, laserEnd, bosses.at(j)))
                    droneBossDmg[j] += kDroneLaserDamage;
            }
        }

        for (int j = enemies.size() - 1; j >= 0; --j) {
            if (j < 0 || j >= droneEnemyDmg.size())
                continue;
            const int tick = droneEnemyDmg.at(j);
            if (tick <= 0)
                continue;
            enemies[j].health -= tick;
            if (enemies[j].health <= 0) {
                int killedEnemyId = enemies[j].enemyId;
                QPointF killedEnemyPos = enemies[j].pos;
                addExplosion(killedEnemyPos);
                score += (enemies[j].type + 1) * 10;
                for (int k = enemyBullets.size() - 1; k >= 0; --k) {
                    if (enemyBullets[k].ownerEnemyId == killedEnemyId)
                        enemyBullets.removeAt(k);
                }
                tryDropUpgradeAndShield(killedEnemyPos);
                enemies.removeAt(j);
            }
        }

        for (int j = bosses.size() - 1; j >= 0; --j) {
            if (j < 0 || j >= droneBossDmg.size())
                continue;
            const int tick = droneBossDmg.at(j);
            if (tick <= 0)
                continue;
            bosses[j].health -= tick;
        }
    }
    
    // 玩家子弹与敌机碰撞（空间桶粗筛 + 轴对齐粗剔除，减轻高等级高弹幕时压力）
    for (int i = playerBullets.size() - 1; i >= 0; --i) {
        bool bulletRemoved = false;
        const double bpx = playerBullets[i].pos.x();
        const double bpy = playerBullets[i].pos.y();
        const int bcx = enemyBucketIndex(bpx);
        const int bcy = enemyBucketIndex(bpy);
        for (int j = enemies.size() - 1; j >= 0; --j) {
            const Enemy &ej = enemies[j];
            if (bulletEnemyBucketTooFar(bcx, bcy, enemyBucketIndex(ej.pos.x()), enemyBucketIndex(ej.pos.y()))) {
                continue;
            }
            const int enemySize = 15 + ej.type * 5;
            if (ej.pos.x() + enemySize < 0 || ej.pos.x() - enemySize > m_arenaWidth ||
                ej.pos.y() + enemySize < 0 || ej.pos.y() - enemySize > m_arenaHeight) {
                continue;
            }
            const double edx = bpx - ej.pos.x();
            const double edy = bpy - ej.pos.y();
            const double outer = enemySize + 14;
            if (edx * edx + edy * edy > outer * outer) {
                continue;
            }
            if (!checkBulletEnemyCollision(playerBullets[i], ej, true)) {
                continue;
            }
            const int finalDamage = (playerBullets[i].hitDamage > 0)
                ? playerBullets[i].hitDamage
                : damageForPlayerBullet(playerBullets[i].colorType);
            enemies[j].health -= finalDamage;
                
                // 检查敌机是否被摧毁（必须在处理子弹之前检查，因为可能移除enemy）
                bool enemyDestroyed = (enemies[j].health <= 0);
                
                // 处理穿透子弹（5级时所有子弹都可穿透）
                if (playerBullets[i].penetration > 0) {
                    // 穿透子弹：减少穿透次数，不立即移除
                    playerBullets[i].penetration--;
                    if (playerBullets[i].penetration <= 0) {
                        playerBullets.removeAt(i);
                        bulletRemoved = true;
                        // 如果敌机被摧毁，处理摧毁逻辑
                        if (enemyDestroyed) {
                            QPointF killedEnemyPos = enemies[j].pos;
                            addExplosion(killedEnemyPos);
                            score += (enemies[j].type + 1) * 10;

                            // 暴走期间：清除该敌机发射的所有子弹
                            if (berserkTimeLeft > 0) {
                                int killedEnemyId = enemies[j].enemyId;
                                for (int k = enemyBullets.size() - 1; k >= 0; --k) {
                                    if (enemyBullets[k].ownerEnemyId == killedEnemyId) {
                                        enemyBullets.removeAt(k);
                                    }
                                }
                            }

                            // 击杀掉落：20%概率，三种道具平均分配
                            tryDropUpgradeAndShield(killedEnemyPos);
                            
                            enemies.removeAt(j);
                        }
                        break; // 子弹已用完，退出内层循环
                    }
                    // 穿透子弹继续穿透，检查当前敌机是否被摧毁
                    if (enemyDestroyed) {
                        // 暴走期间：清除该敌机发射的所有子弹
                        if (berserkTimeLeft > 0) {
                            int killedEnemyId = enemies[j].enemyId;
                            for (int k = enemyBullets.size() - 1; k >= 0; --k) {
                                if (enemyBullets[k].ownerEnemyId == killedEnemyId) {
                                    enemyBullets.removeAt(k);
                                }
                            }
                        }
                        
                        addExplosion(enemies[j].pos);
                        score += (enemies[j].type + 1) * 10;

                        // 击杀掉落：升级/护盾独立20%，治疗维持低概率
                        tryDropUpgradeAndShield(enemies[j].pos);
                        tryDropHealPowerUp(enemies[j].pos);
                        
                        enemies.removeAt(j);
                        // 穿透子弹继续，不break
                    }
                } else {
                    // 普通子弹：击中后立即移除
                    playerBullets.removeAt(i);
                    bulletRemoved = true;
                    
                    // 如果敌机被摧毁，处理摧毁逻辑
                    if (enemyDestroyed) {
                        // 暴走期间：清除该敌机发射的所有子弹
                        if (berserkTimeLeft > 0) {
                            int killedEnemyId = enemies[j].enemyId;
                            for (int k = enemyBullets.size() - 1; k >= 0; --k) {
                                if (enemyBullets[k].ownerEnemyId == killedEnemyId) {
                                    enemyBullets.removeAt(k);
                                }
                            }
                        }
                        
                        addExplosion(enemies[j].pos);
                        score += (enemies[j].type + 1) * 10;

                        // 击杀掉落：升级/护盾独立20%，治疗维持低概率
                        tryDropUpgradeAndShield(enemies[j].pos);
                        tryDropHealPowerUp(enemies[j].pos);
                        
                        enemies.removeAt(j);
                    }
                    break;
                }
        }
        if (bulletRemoved) {
            continue; // 子弹已移除，跳过Boss碰撞检测
        }
        
        // 玩家子弹与Boss碰撞
        for (int j = bosses.size() - 1; j >= 0; --j) {
            if (bosses[j].isActive && checkBulletBossCollision(playerBullets[i], bosses[j])) {
                const int finalDamage = (playerBullets[i].hitDamage > 0)
                    ? playerBullets[i].hitDamage
                    : damageForPlayerBullet(playerBullets[i].colorType);
                bosses[j].health -= finalDamage;
                
                // 处理穿透子弹（5级穿透子弹伤害与普通子弹一致，仅增加穿透功能）
                if (playerBullets[i].penetration > 0) {
                    // 穿透子弹：减少穿透次数，不立即移除（伤害已在上面计算，不再额外伤害）
                    playerBullets[i].penetration--;
                    if (playerBullets[i].penetration <= 0) {
                        playerBullets.removeAt(i);
                        break;
                    }
                } else {
                    // 普通子弹：击中后立即移除
                    playerBullets.removeAt(i);
                    break;
                }
                
                // Boss血条清空的处理在updateBosses中进行
                if (playerBullets[i].penetration <= 0) {
                    break;
                }
            }
        }
    }
    
    // 玩家与升级道具碰撞
    for (int i = powerUps.size() - 1; i >= 0; --i) {
        if (checkPlayerPowerUpCollision(playerPos, powerUps[i])) {
            applyPowerUp(powerUps[i].type);
            powerUps.removeAt(i);
        }
    }
    
    // 检查Boss状态：如果Boss正在登场或中场切换，玩家不受伤害（bossInTransition已在函数开头声明）
    
    // 玩家与敌机碰撞
    for (int i = enemies.size() - 1; i >= 0; --i) {
        if (checkPlayerEnemyCollision(playerPos, enemies[i])) {
            // Boss出场或中场动画时，玩家不受伤害
            if (bossInTransition) {
                continue; // 跳过伤害处理
            }
            
            // 暴走期间：清除该敌机发射的所有子弹
            if (berserkTimeLeft > 0) {
                int killedEnemyId = enemies[i].enemyId;
                for (int k = enemyBullets.size() - 1; k >= 0; --k) {
                    if (enemyBullets[k].ownerEnemyId == killedEnemyId) {
                        enemyBullets.removeAt(k);
                    }
                }
            }
            
            addExplosion(enemies[i].pos);
            if (!hasShield && invincibleTime <= 0) {
                // 只有在没有护盾且不在无敌时间时才受到伤害
                // 无护盾碰撞到普通敌机减少30点生命
                playerHealth -= 30;
                if (playerHealth < 0) {
                    playerHealth = 0;
                }
                
                // 如果受伤不足以结束游戏，进行降级操作
                if (playerHealth > 0) {
                    // 降低等级1
                    if (playerLevel > 0) {
                        playerLevel--;
                    }
                }
                
                // 启动3秒无敌时间（180帧）
                invincibleTime = 180;
                invincibleFlashTimer = 0;
            }
            enemies.removeAt(i);
            if (playerHealth <= 0) {
                gameState = GAME_OVER;
                if (gameTimer) {
                    gameTimer->stop();
                }
                enemySpawnTimerObj->stop();
                bossSpawnTimerObj->stop();
                return;
            }
        }
    }
    
    // 玩家与Boss碰撞
    for (int i = bosses.size() - 1; i >= 0; --i) {
        if (bosses[i].isActive && checkPlayerBossCollision(playerPos, bosses[i])) {
            // Boss出场或中场动画时，玩家不受伤害
            if (bosses[i].isWarning || bosses[i].layerTransitionTimer > 0) {
                continue; // 跳过伤害处理
            }
            
            addExplosion(bosses[i].pos);
            if (!hasShield && invincibleTime <= 0) {
                // 只有在没有护盾且不在无敌时间时才受到伤害
                // 无护盾碰撞到boss死亡结束游戏
                playerHealth = 0;
                gameState = GAME_OVER;
                if (gameTimer) {
                    gameTimer->stop();
                }
                enemySpawnTimerObj->stop();
                bossSpawnTimerObj->stop();
                return;
            }
        }
    }
    
    // 玩家与敌机子弹碰撞
    for (int i = enemyBullets.size() - 1; i >= 0; --i) {
        if (checkPlayerBulletCollision(playerPos, enemyBullets[i])) {
            // Boss出场或中场动画时，玩家不受伤害
            if (bossInTransition) {
                enemyBullets.removeAt(i); // 移除子弹但不造成伤害
                continue;
            }
            
            if (!hasShield && invincibleTime <= 0) {
                // 只有在没有护盾且不在无敌时间时才受到伤害
                // 降低等级1，并扣除血量
                if (playerLevel > 0) {
                    playerLevel--;
                }
                // 扣除血量（生成时已写入 hitDamage，默认与原逻辑 20 一致）
                playerHealth -= enemyBullets[i].hitDamage;
                if (playerHealth < 0) {
                    playerHealth = 0;
                }
                // 启动3秒无敌时间（180帧）
                invincibleTime = 180;
                invincibleFlashTimer = 0;
                enemyBullets.removeAt(i);
            } else if (hasShield) {
                // 有护盾时，子弹被护盾抵消（不造成伤害，但移除子弹）
                enemyBullets.removeAt(i);
            } else {
                // 无敌时间内，移除子弹但不造成伤害
                enemyBullets.removeAt(i);
            }
            if (playerHealth <= 0) {
                return;
            }
        }
    }
}

bool ThunderFighterWindow::checkBulletEnemyCollision(const Bullet &bullet, const Enemy &enemy,
                                                       bool skipBroadPhase)
{
    int enemySize = 15 + enemy.type * 5;
    if (!skipBroadPhase) {
        // 屏幕范围外的敌机不受伤害
        bool isInScreen = (enemy.pos.x() + enemySize >= 0 &&
                          enemy.pos.x() - enemySize <= m_arenaWidth &&
                          enemy.pos.y() + enemySize >= 0 &&
                          enemy.pos.y() - enemySize <= m_arenaHeight);
        if (!isInScreen) {
            return false;
        }

        const double dx = bullet.pos.x() - enemy.pos.x();
        const double dy = bullet.pos.y() - enemy.pos.y();
        const double outer = enemySize + 14;
        if (dx * dx + dy * dy > outer * outer) {
            return false;
        }
    }

    QPointF diff = bullet.pos - enemy.pos;
    return (diff.x() * diff.x() + diff.y() * diff.y()) < (enemySize * enemySize);
}

bool ThunderFighterWindow::checkPlayerEnemyCollision(const QPointF &playerPos, const Enemy &enemy)
{
    int enemySize = 15 + enemy.type * 5;
    int playerCollisionRadius = 3; // 碰撞检测基于中心点，半径3像素
    QPointF diff = playerPos - enemy.pos;
    return (diff.x() * diff.x() + diff.y() * diff.y()) < ((enemySize + playerCollisionRadius) * (enemySize + playerCollisionRadius));
}

bool ThunderFighterWindow::checkPlayerBulletCollision(const QPointF &playerPos, const Bullet &bullet)
{
    int playerCollisionRadius = 3; // 碰撞检测基于中心点，半径3像素
    int bulletRadius = 3; // 子弹半径
    QPointF diff = playerPos - bullet.pos;
    return (diff.x() * diff.x() + diff.y() * diff.y()) < ((playerCollisionRadius + bulletRadius) * (playerCollisionRadius + bulletRadius));
}

bool ThunderFighterWindow::checkBulletBossCollision(const Bullet &bullet, const Boss &boss)
{
    int bossSize = 80; // 与绘制尺寸一致
    
    // 点碰撞检测（5级时的穿透子弹也是点碰撞）
    QPointF diff = bullet.pos - boss.pos;
    return (diff.x() * diff.x() + diff.y() * diff.y()) < (bossSize * bossSize);
}

bool ThunderFighterWindow::checkLaserEnemyCollision(const QPointF &laserStart, const QPointF &laserEnd, const Enemy &enemy)
{
    // 屏幕范围外的敌机不受伤害
    int enemySize = 15 + enemy.type * 5;
    bool isInScreen = (enemy.pos.x() + enemySize >= 0 && 
                      enemy.pos.x() - enemySize <= m_arenaWidth &&
                      enemy.pos.y() + enemySize >= 0 && 
                      enemy.pos.y() - enemySize <= m_arenaHeight);
    if (!isInScreen) {
        return false; // 不在屏幕内，不受伤害
    }
    
    // 计算线段到圆心的最短距离
    QPointF lineVec = laserEnd - laserStart;
    QPointF pointVec = enemy.pos - laserStart;
    
    double lineLenSq = lineVec.x() * lineVec.x() + lineVec.y() * lineVec.y();
    if (lineLenSq < 0.0001) {
        // 线段长度为0，退化为点碰撞
        QPointF diff = laserStart - enemy.pos;
        return (diff.x() * diff.x() + diff.y() * diff.y()) < (enemySize * enemySize);
    }
    
    // 计算投影参数t
    double t = qMax(0.0, qMin(1.0, (pointVec.x() * lineVec.x() + pointVec.y() * lineVec.y()) / lineLenSq));
    
    // 计算线段上最近的点
    QPointF closestPoint = laserStart + QPointF(lineVec.x() * t, lineVec.y() * t);
    
    // 计算最近点到圆心的距离
    QPointF diff = closestPoint - enemy.pos;
    double distSq = diff.x() * diff.x() + diff.y() * diff.y();
    
    return distSq < (enemySize * enemySize);
}

bool ThunderFighterWindow::checkLaserBossCollision(const QPointF &laserStart, const QPointF &laserEnd, const Boss &boss)
{
    int bossSize = 80; // 与绘制尺寸一致
    
    // 计算线段到圆心的最短距离
    QPointF lineVec = laserEnd - laserStart;
    QPointF pointVec = boss.pos - laserStart;
    
    double lineLenSq = lineVec.x() * lineVec.x() + lineVec.y() * lineVec.y();
    if (lineLenSq < 0.0001) {
        // 线段长度为0，退化为点碰撞
        QPointF diff = laserStart - boss.pos;
        return (diff.x() * diff.x() + diff.y() * diff.y()) < (bossSize * bossSize);
    }
    
    // 计算投影参数t
    double t = qMax(0.0, qMin(1.0, (pointVec.x() * lineVec.x() + pointVec.y() * lineVec.y()) / lineLenSq));
    
    // 计算线段上最近的点
    QPointF closestPoint = laserStart + QPointF(lineVec.x() * t, lineVec.y() * t);
    
    // 计算最近点到圆心的距离
    QPointF diff = closestPoint - boss.pos;
    double distSq = diff.x() * diff.x() + diff.y() * diff.y();
    
    return distSq < (bossSize * bossSize);
}

bool ThunderFighterWindow::checkPlayerBossCollision(const QPointF &playerPos, const Boss &boss)
{
    int bossSize = 80; // 与绘制尺寸一致
    int playerCollisionRadius = 3; // 碰撞检测基于中心点，半径3像素
    QPointF diff = playerPos - boss.pos;
    return (diff.x() * diff.x() + diff.y() * diff.y()) < ((bossSize + playerCollisionRadius) * (bossSize + playerCollisionRadius));
}

void ThunderFighterWindow::addExplosion(const QPointF &pos)
{
    Explosion explosion;
    explosion.pos = pos;
    explosion.frame = 0;
    explosion.maxFrames = 15;
    explosions.append(explosion);
}

void ThunderFighterWindow::spawnEnemy()
{
    if (gameState != PLAYING) {
        return;
    }
    
    // Boss存在时不生成其他怪物（包括Boss处于等待阶段时）
    bool bossExists = false;
    for (const Boss &boss : bosses) {
        if (boss.isActive || boss.isWarning || boss.layerTransitionTimer > 0) {
            bossExists = true;
            break;
        }
    }
    if (bossExists) {
        return;
    }
    
    // 按组生成敌机：每组3-6只随机
    int groupSize = QRandomGenerator::global()->bounded(4) + 3; // 3-6只随机
    
    // 随机选择出现方式：0=正上方下来，1=侧面平移，2=对角线
    int spawnMode = QRandomGenerator::global()->bounded(3);
    
    int screenWidth = m_arenaWidth;
    int screenHeight = m_arenaHeight;
    int leftBound = 50;
    int rightBound = screenWidth - 50;
    int centerX = screenWidth / 2;
    int upperY = screenHeight / 3; // 上方1/3区域（靠上方部分）
    
    for (int i = 0; i < groupSize; ++i) {
        Enemy enemy;
        
        // 随机分配敌机类型：大中小（0=小，1=中，2=大）
        enemy.type = QRandomGenerator::global()->bounded(3);
        
        enemy.health = enemy.type + 1 + difficultyLevel; // 生命值等于类型+1，根据难度增加
        enemy.vx = 0.0; // 默认x方向速度为0
        enemy.shootCooldown = 0; // 初始射击冷却为0
        enemy.shootPattern = QRandomGenerator::global()->bounded(4); // 随机初始射击模式（0-3）
        enemy.shootPatternTimer = 0; // 射击模式计时器
        enemy.spawnMode = spawnMode; // 记录出现方式
        enemy.hasReachedTarget = false; // 初始未到达目标
        enemy.enterScreenTimer = -1; // -1表示尚未进入屏幕
        enemy.enemyId = nextEnemyId++; // 分配唯一ID
        
        // 计算目标位置：从下往上2/3屏幕位置按队形排列（即从顶部往下1/3屏幕位置）
        int targetY = screenHeight / 3; // 从顶部往下1/3屏幕位置（相当于从下往上2/3）
        int formationWidth = qMin(groupSize * 60, screenWidth - 100); // 队形宽度
        int startX = centerX - formationWidth / 2;
        int spacing = (groupSize > 1) ? formationWidth / (groupSize - 1) : 0;
        enemy.targetPos = QPointF(startX + i * spacing, targetY);
        
        double x, y;
        
        switch (spawnMode) {
            case 0: // 正上方下来
                {
                    // 从正上方下来
                    x = centerX + QRandomGenerator::global()->bounded(-100, 101); // 中心±100像素范围
                    x = qBound(leftBound, static_cast<int>(x), rightBound);
                    y = -40 - i * 25; // 从上方错开进入
                    enemy.speed = 0.5 + enemy.type * 0.3; // y方向速度随类型增加
                    enemy.vx = 0.0; // 无x方向移动
                }
                break;
                
            case 1: // 侧面平移出来
                {
                    // 从左右两侧靠上方部分平移出来，不朝下移动
                    bool fromLeft = QRandomGenerator::global()->bounded(2) == 0; // 随机选择左右
                    if (fromLeft) {
                        // 从左侧平移出来
                        x = -50 - i * 30; // 从屏幕左侧外进入
                        y = QRandomGenerator::global()->bounded(upperY) + 50; // y在上方1/3区域
                        enemy.vx = 1.0 + enemy.type * 0.2; // 向右移动
                    } else {
                        // 从右侧平移出来
                        x = screenWidth + 50 + i * 30; // 从屏幕右侧外进入
                        y = QRandomGenerator::global()->bounded(upperY) + 50; // y在上方1/3区域
                        enemy.vx = -(1.0 + enemy.type * 0.2); // 向左移动
                    }
                    enemy.speed = 0.0; // 不朝下移动，y方向速度为0
                }
                break;
                
            case 2: // 对角线走出来
                {
                    // 对角线走出来
                    bool fromLeftTop = QRandomGenerator::global()->bounded(2) == 0; // 随机选择左上或右上
                    if (fromLeftTop) {
                        // 从左上角对角线进入
                        x = -50 - i * 25;
                        y = -50 - i * 20;
                        enemy.vx = 1.2 + enemy.type * 0.3; // 向右下移动
                        enemy.speed = 0.8 + enemy.type * 0.2; // y方向速度
                    } else {
                        // 从右上角对角线进入
                        x = screenWidth + 50 + i * 25;
                        y = -50 - i * 20;
                        enemy.vx = -(1.2 + enemy.type * 0.3); // 向左下移动
                        enemy.speed = 0.8 + enemy.type * 0.2; // y方向速度
                    }
                }
                break;
        }
        
        enemy.pos = QPointF(x, y);
        enemies.append(enemy);
    }
}

void ThunderFighterWindow::spawnBoss()
{
    if (m_destroying) {
        return;
    }
    if (gameState != PLAYING) {
        return;
    }
    
    // 如果已经有Boss存在（包括等待阶段），不生成新的
    for (const Boss &boss : bosses) {
        if (boss.isActive || boss.isWarning || boss.layerTransitionTimer > 0) {
            // Boss存在时不生成新的，避免重复生成
            return;
        }
    }
    
    // 确保定时器已停止（防止重复调用，Boss由波次系统控制）
    if (bossSpawnTimerObj && bossSpawnTimerObj->isActive()) {
        bossSpawnTimerObj->stop();
    }
    
    Boss boss;
    boss.pos = QPointF(m_arenaWidth / 2, -100); // 从屏幕外进入
    // 根据Boss击杀次数决定血条层数（单数回合1层，双数回合2层）
    boss.maxLayer = (bossKillCount % 2 == 0) ? 1 : 2; // 第1个Boss是1层，第2个是2层，以此类推
    boss.layer = boss.maxLayer;
    
    // Boss血量增长曲线设计：
    // 每两个boss为一波，每波增长一次
    // 规则：双血条boss的总血量 = 单血条boss的总血量 * 2
    // 
    // 第1波（wave=0）：
    //   Boss1(单血条) = 600, 总血量 = 600
    //   Boss2(双血条) = 600/层, 总血量 = 1200 (是Boss1的2倍) ✓
    // 
    // 第2波（wave=1）：
    //   Boss3(单血条) = 900, 总血量 = 900 (大于Boss1的600，小于Boss2的1200) ✓
    //   Boss4(双血条) = 900/层, 总血量 = 1800 (大于Boss2的1200) ✓
    // 
    // 第3波（wave=2）：
    //   Boss5(单血条) = 1200, 总血量 = 1200 (大于Boss3的900，等于Boss2的1200) ✓
    //   Boss6(双血条) = 1200/层, 总血量 = 2400 (大于Boss4的1800) ✓
    // 以此类推...
    
    int wave = bossKillCount / 2; // 当前是第几波（0, 1, 2, ...）
    int positionInWave = bossKillCount % 2; // 在当前波中的位置（0=单血条, 1=双血条）
    
    // 基础血量（单血条boss的总血量）
    int baseTotalHealth = 600; // 第1波单血条boss的总血量
    int singleBarTotalHealth; // 当前波次单血条boss的总血量
    
    if (wave == 0) {
        // 第1波：单血条boss总血量 = 600
        singleBarTotalHealth = baseTotalHealth;
    } else {
        // 第2波及以后：每波增长50%
        singleBarTotalHealth = static_cast<int>(baseTotalHealth * (1.0 + wave * 0.5));
    }
    
    if (positionInWave == 0) {
        // 单血条boss：总血量 = singleBarTotalHealth
        boss.maxHealth = singleBarTotalHealth;
    } else {
        // 双血条boss：总血量 = singleBarTotalHealth * 2，每层血量 = singleBarTotalHealth
        boss.maxHealth = singleBarTotalHealth; // 每层血量等于单血条boss的总血量
        // 注意：双血条boss的总血量 = maxHealth * 2 = singleBarTotalHealth * 2
    }
    
    boss.health = boss.maxHealth;
    boss.speed = 0.5; // 左右移动速度（降低频率）
    boss.shootCooldown = qMax(15, 30 - difficultyLevel * 2);
    boss.isActive = false; // 初始不激活，等待警告动画
    boss.isWarning = true; // 开始警告阶段
    boss.warningTimer = 120; // 警告动画持续120帧（2秒）
    boss.shootPatternTimer = 0; // 射击模式计时器
    boss.shootPattern = 0; // 初始射击模式
    boss.layerTransitionTimer = 0; // 层转换计时器
    boss.burstCount = 0; // 连发计数
    boss.burstCooldown = 0; // 连发间隔冷却
    bosses.append(boss);
}

void ThunderFighterWindow::updatePowerUps()
{
    for (int i = powerUps.size() - 1; i >= 0; --i) {
        // 计算道具与玩家的距离
        QPointF diff = playerPos - powerUps[i].pos;
        double distance = qSqrt(diff.x() * diff.x() + diff.y() * diff.y());
        double attractDistance = 80.0; // 吸附范围：80像素
        
        // 如果道具在吸附范围内，向玩家移动
        if (distance < attractDistance && distance > 0.1) {
            // 计算向玩家的方向向量
            double dirX = diff.x() / distance;
            double dirY = diff.y() / distance;
            double attractSpeed = 3.0; // 吸附速度
            
            // 向玩家移动
            powerUps[i].vx = dirX * attractSpeed;
            powerUps[i].vy = dirY * attractSpeed;
        } else {
            // 不在吸附范围内，使用物理碰撞运动（参照2D演示中的无限碰撞）
            // 保持当前速度，不随机改变方向
        }
        
        // 使用道具中心点坐标进行计算（更精确）
        double powerUpSize = 15.0; // 道具大小
        double powerUpRadius = powerUpSize / 2.0;
        double centerX = powerUps[i].pos.x() + powerUpRadius;
        double centerY = powerUps[i].pos.y() + powerUpRadius;
        
        // 计算新位置（基于中心点）
        double newCenterX = centerX + powerUps[i].vx;
        double newCenterY = centerY + powerUps[i].vy;
        
        // 检测边界碰撞并反弹（使用物理反射公式，参照2D演示）
        // 左边界碰撞
        if (newCenterX - powerUpRadius < 20) {
            newCenterX = 20 + powerUpRadius;
            powerUps[i].vx = -powerUps[i].vx; // 反射
        }
        // 右边界碰撞
        else if (newCenterX + powerUpRadius > m_arenaWidth - 20) {
            newCenterX = m_arenaWidth - 20 - powerUpRadius;
            powerUps[i].vx = -powerUps[i].vx; // 反射
        }
        
        // 上边界碰撞
        if (newCenterY - powerUpRadius < 20) {
            newCenterY = 20 + powerUpRadius;
            powerUps[i].vy = -powerUps[i].vy; // 反射
        }
        // 下边界碰撞
        else if (newCenterY + powerUpRadius > m_arenaHeight - 20) {
            newCenterY = m_arenaHeight - 20 - powerUpRadius;
            powerUps[i].vy = -powerUps[i].vy; // 反射
        }
        
        // 确保速度大小保持不变（无能量损失，参照2D演示中的无限碰撞）
        // 在反射后，速度大小应该保持不变（反射只改变方向）
        // 这里不需要额外处理，因为反射只是反转速度分量，速度大小自然保持不变
        
        // 更新位置（将中心点坐标转换回左上角坐标）
        powerUps[i].pos = QPointF(newCenterX - powerUpRadius, newCenterY - powerUpRadius);
        
        // 移除超出屏幕的升级道具
        if (powerUps[i].pos.y() > m_arenaHeight + 20) {
            powerUps.removeAt(i);
        }
    }
}

void ThunderFighterWindow::updateShield()
{
    if (hasShield && shieldTimeLeft > 0) {
        // Boss登场/中场切换停火期间：冻结护盾计时
        bool bossInTransition = false;
        for (const Boss &boss : bosses) {
            if (boss.isWarning || boss.layerTransitionTimer > 0) {
                bossInTransition = true;
                break;
            }
        }
        if (bossInTransition) {
            return;
        }

        shieldTimeLeft--;
        
        // 护盾生效时：清除护盾范围内的所有敌机子弹
        // 护盾范围：半径约30像素（与drawShield中的绘制范围一致）
        const double shieldRadius = 30.0;
        const double shieldRadiusSquared = shieldRadius * shieldRadius;
        
        for (int i = enemyBullets.size() - 1; i >= 0; --i) {
            QPointF diff = playerPos - enemyBullets[i].pos;
            double distanceSquared = diff.x() * diff.x() + diff.y() * diff.y();
            if (distanceSquared <= shieldRadiusSquared) {
                // 子弹在护盾范围内，清除该子弹
                enemyBullets.removeAt(i);
            }
        }
        
        if (shieldTimeLeft <= 0) {
            // 护盾到期，清空屏幕所有敌机子弹
            enemyBullets.clear();
            hasShield = false;
        }
    }
}

void ThunderFighterWindow::updateLaser()
{
    // 更新激光系统（在4级和5级时）
    if (playerLevel >= 4) {
        // Boss登场/中场切换时：停止攻击优先级最高（包括暴走常开激光）
        bool bossInTransition = false;
        for (const Boss &boss : bosses) {
            if (boss.isWarning || boss.layerTransitionTimer > 0) {
                bossInTransition = true;
                break;
            }
        }
        if (bossInTransition) {
            laserDuration = 0;
            return;
        }

        // 暴走时：僚机激光处于常开状态
        if (berserkTimeLeft > 0) {
            laserDuration = berserkTimeLeft; // 激光持续时间等于暴走剩余时间
            laserCooldown = 0; // 无冷却
        } else {
            // 非暴走状态：正常的激光循环
            if (laserDuration > 0) {
                // 激光激活期间
                laserDuration--;
            } else {
                // 激光冷却期间
                laserCooldown--;
                if (laserCooldown <= 0) {
                    // 激活激光，持续0.3秒（18帧）
                    laserDuration = 18;
                    laserCooldown = 60; // 1秒冷却（60帧）
                }
            }
        }
    }
}

void ThunderFighterWindow::updateInvincible()
{
    // 更新无敌时间
    if (invincibleTime > 0) {
        invincibleTime--;
        // 更新闪烁计时器（每10帧循环一次）
        invincibleFlashTimer++;
        if (invincibleFlashTimer >= 10) {
            invincibleFlashTimer = 0;
        }
    } else {
        // 无敌时间结束，重置闪烁计时器
        invincibleFlashTimer = 0;
    }
}

void ThunderFighterWindow::updateBerserk()
{
    if (berserkTimeLeft > 0) {
        // Boss登场/中场切换停火期间：冻结暴走计时
        bool bossInTransition = false;
        for (const Boss &boss : bosses) {
            if (boss.isWarning || boss.layerTransitionTimer > 0) {
                bossInTransition = true;
                break;
            }
        }
        if (bossInTransition) {
            return;
        }

        berserkTimeLeft--;
        // 激光逻辑已在updateLaser中处理（暴走时激光常开）
    }
}

void ThunderFighterWindow::updateDrones()
{
    if (berserkTimeLeft <= 0) {
        return;
    }
    
    // 检查Boss状态：如果Boss正在登场或中场切换，浮游炮回到玩家旁
    bool bossInTransition = false;
    for (const Boss &boss : bosses) {
        if (boss.isWarning || boss.layerTransitionTimer > 0) {
            bossInTransition = true;
            break;
        }
    }
    // 检查是否有敌机或Boss
    bool hasEnemies = !enemies.isEmpty();
    bool hasBosses = false;
    for (const Boss &boss : bosses) {
        if (boss.isActive) {
            hasBosses = true;
            break;
        }
    }
    
    // 共享目标：所有浮游炮锁定同一个目标（敌机或Boss）
    static int sharedTargetEnemyId = -1;
    static int sharedTargetBossIndex = -1;
    
    // Boss出场/中场期间，浮游炮返回待机状态，清除共享目标，让它们自然弧形排列（与无目标时一致）
    if (bossInTransition) {
        for (int i = 0; i < drones.size(); ++i) {
            Drone &drone = drones[i];
            drone.state = 0; // 返回待机状态
            drone.targetEnemyId = -1;
            drone.targetBossIndex = -1;
        }
        // 清除共享目标，让浮游炮进入无目标状态
        sharedTargetEnemyId = -1;
        sharedTargetBossIndex = -1;
        // 继续执行后续逻辑，让浮游炮自然进入待机状态的弧形排列
    }
    
    // 检查共享目标是否仍然存在
    if (sharedTargetEnemyId >= 0) {
        bool targetExists = false;
        for (const Enemy &enemy : enemies) {
            if (enemy.enemyId == sharedTargetEnemyId) {
                targetExists = true;
                break;
            }
        }
        if (!targetExists) {
            // 目标消失，重置共享目标
            sharedTargetEnemyId = -1;
        }
    }
    
    if (sharedTargetBossIndex >= 0) {
        bool targetExists = false;
        for (int j = 0; j < bosses.size(); ++j) {
            if (j == sharedTargetBossIndex && bosses[j].isActive) {
                targetExists = true;
                break;
            }
        }
        if (!targetExists) {
            // 目标消失，重置共享目标
            sharedTargetBossIndex = -1;
        }
    }
    
    // 如果没有共享目标且Boss不在出场/转场，选择最近的目标（优先Boss，其次敌机）
    if (sharedTargetEnemyId < 0 && sharedTargetBossIndex < 0 && !bossInTransition) {
        if (hasBosses) {
            // 优先选择Boss
            double minDistance = 10000.0;
            int targetBossIndex = -1;
            
            for (int j = 0; j < bosses.size(); ++j) {
                if (bosses[j].isActive) {
                    QPointF diff = bosses[j].pos - playerPos;
                    double distance = qSqrt(diff.x() * diff.x() + diff.y() * diff.y());
                    if (distance < minDistance) {
                        minDistance = distance;
                        targetBossIndex = j;
                    }
                }
            }
            
            if (targetBossIndex >= 0) {
                sharedTargetBossIndex = targetBossIndex;
            }
        } else if (hasEnemies) {
            // 选择最近的敌机
            double minDistance = 10000.0;
            int targetEnemyIndex = -1;
            
            for (int j = 0; j < enemies.size(); ++j) {
                QPointF diff = enemies[j].pos - playerPos;
                double distance = qSqrt(diff.x() * diff.x() + diff.y() * diff.y());
                if (distance < minDistance) {
                    minDistance = distance;
                    targetEnemyIndex = j;
                }
            }
            
            if (targetEnemyIndex >= 0) {
                sharedTargetEnemyId = enemies[targetEnemyIndex].enemyId;
            }
        }
    }
    
    for (int i = 0; i < drones.size(); ++i) {
        Drone &drone = drones[i];
        if (drone.laserVisualTimer > 0) {
            drone.laserVisualTimer--;
        }
        
        if (drone.state == 0) {
            // 待机状态：弧形排列在玩家战机正前方（朝向上，即-90°方向）
            // 无目标时或Boss出场/转场时，弧形排列
            if ((!hasEnemies && !hasBosses) || (sharedTargetEnemyId < 0 && sharedTargetBossIndex < 0) || bossInTransition) {
                // 计算弧形排列位置（5个浮游炮，弧形排列）
                double arcRadius = 80.0; // 弧形半径
                double arcStartAngle = -M_PI / 2.0 - M_PI / 6.0; // 从-90°-30°开始
                double arcEndAngle = -M_PI / 2.0 + M_PI / 6.0; // 到-90°+30°结束
                const int ds = drones.size();
                const double arcSpan = arcEndAngle - arcStartAngle;
                const double t = (ds <= 1) ? 0.0 : (i / double(ds - 1));
                double angle = arcStartAngle + t * arcSpan;
                
                drone.targetPos = QPointF(
                    playerPos.x() + qCos(angle) * arcRadius,
                    playerPos.y() + qSin(angle) * arcRadius
                );
                
                // 平滑移动到目标位置
                QPointF diff = drone.targetPos - drone.pos;
                double distance = qSqrt(diff.x() * diff.x() + diff.y() * diff.y());
                if (distance > 2.0) {
                    double moveSpeed = 5.0;
                    drone.pos += diff * (moveSpeed / distance);
                } else {
                    drone.pos = drone.targetPos;
                }
                
                // 朝向玩家正前方
                drone.angle = -M_PI / 2.0;
            } else {
                // 有共享目标时，切换到冲向目标状态
                if (sharedTargetBossIndex >= 0) {
                    drone.targetBossIndex = sharedTargetBossIndex;
                    drone.targetEnemyId = -1;
                } else if (sharedTargetEnemyId >= 0) {
                    drone.targetEnemyId = sharedTargetEnemyId;
                    drone.targetBossIndex = -1;
                }
                drone.state = 1;
                // 锁定目标后立即允许激光发射
                drone.laserAttackTimer = 0;
            }
        } else if (drone.state == 1) {
            // 冲向目标状态（敌机或Boss）
            QPointF targetPos;
            bool targetExists = false;
            
            if (drone.targetBossIndex >= 0 && drone.targetBossIndex < bosses.size()) {
                if (bosses[drone.targetBossIndex].isActive) {
                    targetPos = bosses[drone.targetBossIndex].pos;
                    targetExists = true;
                }
            } else if (drone.targetEnemyId >= 0) {
                for (int j = 0; j < enemies.size(); ++j) {
                    if (enemies[j].enemyId == drone.targetEnemyId) {
                        targetPos = enemies[j].pos;
                        targetExists = true;
                        break;
                    }
                }
            }
            
            if (!targetExists) {
                // 目标已消失，返回待机状态
                drone.state = 0;
                int oldTargetId = drone.targetEnemyId;
                int oldBossIndex = drone.targetBossIndex;
                drone.targetEnemyId = -1;
                drone.targetBossIndex = -1;
                if (sharedTargetEnemyId == oldTargetId) {
                    sharedTargetEnemyId = -1;
                }
                if (sharedTargetBossIndex == oldBossIndex) {
                    sharedTargetBossIndex = -1;
                }
                continue;
            }
            
            // 冲向目标
            QPointF diff = targetPos - drone.pos;
            double distance = qSqrt(diff.x() * diff.x() + diff.y() * diff.y());
            
            // 更新朝向：始终朝向目标
            drone.angle = qAtan2(diff.y(), diff.x());
            
            double attackRadius = (drone.targetBossIndex >= 0) ? 50.0 : 30.0; // Boss需要更大的攻击半径
            if (distance < attackRadius) {
                // 到达目标附近，切换到环绕状态
                drone.state = 2;
                drone.orbitTimer = 0;
                drone.laserAttackTimer = 22 + i * 8; // 错开发射帧，减轻单帧 CPU 峰值
                drone.laserVisualTimer = 0;
                // 每个浮游炮使用不同的起始角度和环绕速度，避免重叠
                {
                    const int ds = qMax(1, drones.size());
                    drone.orbitAngle =
                        qAtan2(diff.y(), diff.x()) + (i * 2.0 * M_PI / double(ds)); // 均匀分布起始角度
                }
                drone.orbitRadius = (drone.targetBossIndex >= 0) ? 50.0 : 30.0; // Boss需要更大的环绕半径
                // 随机环绕速度（0.1到0.2之间），每个浮游炮不同
                drone.orbitSpeed = 0.1 + (i * 0.05); // 每个浮游炮速度递增，避免重叠
            } else {
                // 继续冲向目标
                double moveSpeed = 8.0;
                drone.pos += diff * (moveSpeed / distance);
            }
        } else if (drone.state == 2) {
            // 环绕目标状态（敌机或Boss）
            QPointF targetPos;
            bool targetExists = false;
            
            if (drone.targetBossIndex >= 0 && drone.targetBossIndex < bosses.size()) {
                if (bosses[drone.targetBossIndex].isActive) {
                    targetPos = bosses[drone.targetBossIndex].pos;
                    targetExists = true;
                }
            } else if (drone.targetEnemyId >= 0) {
                for (int j = 0; j < enemies.size(); ++j) {
                    if (enemies[j].enemyId == drone.targetEnemyId) {
                        targetPos = enemies[j].pos;
                        targetExists = true;
                        break;
                    }
                }
            }
            
            if (!targetExists) {
                // 目标已消失，返回待机状态
                drone.state = 0;
                int oldTargetId = drone.targetEnemyId;
                int oldBossIndex = drone.targetBossIndex;
                drone.targetEnemyId = -1;
                drone.targetBossIndex = -1;
                if (sharedTargetEnemyId == oldTargetId) {
                    sharedTargetEnemyId = -1;
                }
                if (sharedTargetBossIndex == oldBossIndex) {
                    sharedTargetBossIndex = -1;
                }
                continue;
            }
            
            // 更新朝向：始终朝向目标
            QPointF diff = targetPos - drone.pos;
            drone.angle = qAtan2(diff.y(), diff.x());
            
            // 环绕目标（使用随机速度，避免重叠）
            drone.orbitAngle += drone.orbitSpeed; // 使用每个浮游炮自己的环绕速度
            if (drone.orbitAngle >= 2 * M_PI) {
                drone.orbitAngle -= 2 * M_PI;
            }
            
            drone.pos = QPointF(
                targetPos.x() + qCos(drone.orbitAngle) * drone.orbitRadius,
                targetPos.y() + qSin(drone.orbitAngle) * drone.orbitRadius
            );
        }
    }
}

void ThunderFighterWindow::drawPowerUps(QPainter &painter)
{
    for (const PowerUp &powerUp : powerUps) {
        painter.save();
        painter.translate(powerUp.pos);
        
        if (powerUp.type == 0) {
            // 升级道具（星星形状）
            painter.setBrush(QColor(255, 215, 0)); // 金色
            painter.setPen(QPen(QColor(255, 165, 0), 2));
            
            QPolygonF star;
            for (int i = 0; i < 5; ++i) {
                double angle = i * 2 * M_PI / 5 - M_PI / 2;
                double outerRadius = 12;
                double innerRadius = 6;
                
                // 外点
                star << QPointF(outerRadius * qCos(angle), outerRadius * qSin(angle));
                // 内点
                angle += M_PI / 5;
                star << QPointF(innerRadius * qCos(angle), innerRadius * qSin(angle));
            }
            painter.drawPolygon(star);
        } else if (powerUp.type == 1) {
            // 护盾道具（圆形，带保护符号）
            painter.setBrush(QColor(0, 150, 255)); // 蓝色
            painter.setPen(QPen(QColor(0, 100, 200), 2));
            painter.drawEllipse(-15, -15, 30, 30);
            
            // 绘制护盾符号（圆形）
            painter.setPen(QPen(QColor(255, 255, 255), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(-10, -10, 20, 20);
        } else if (powerUp.type == 2) {
            // 治疗道具（红色十字）
            painter.setBrush(QColor(255, 0, 0)); // 红色
            painter.setPen(QPen(QColor(200, 0, 0), 2));
            painter.drawEllipse(-12, -12, 24, 24);
            
            // 绘制十字
            painter.setPen(QPen(QColor(255, 255, 255), 3));
            painter.drawLine(-8, 0, 8, 0); // 横线
            painter.drawLine(0, -8, 0, 8); // 竖线
        }
        
        painter.restore();
    }
}

void ThunderFighterWindow::drawDrones(QPainter &painter)
{
    for (const Drone &drone : drones) {
        if (drone.laserVisualTimer > 0) {
            double t = drone.laserVisualTimer / 18.0; // 由强到弱衰减
            int outerAlpha = static_cast<int>(90.0 + 130.0 * t);
            int coreAlpha = static_cast<int>(130.0 + 125.0 * t);

            // 外层青蓝辉光
            painter.setPen(QPen(QColor(0, 190, 255, outerAlpha), 5, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(drone.laserStartPos, drone.laserEndPos);
            // 内层高亮核心
            painter.setPen(QPen(QColor(120, 255, 255, coreAlpha), 2, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(drone.laserStartPos, drone.laserEndPos);
        }

        painter.save();
        painter.translate(drone.pos);
        painter.rotate(drone.angle * 180.0 / M_PI); // 旋转到朝向角度
        
        // 绘制浮游炮（小型三角形，带能量效果，朝向目标）
        painter.setPen(QPen(QColor(0, 200, 255), 2));
        painter.setBrush(QColor(0, 150, 255, 200));
        
        // 绘制三角形（指向目标方向）
        QPolygonF triangle;
        triangle << QPointF(0, -8); // 顶点（前方）
        triangle << QPointF(-4, 4); // 左下
        triangle << QPointF(4, 4);  // 右下
        painter.drawPolygon(triangle);
        
        // 绘制能量环
        painter.setPen(QPen(QColor(100, 200, 255), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(0, 0), 8, 8);
        
        painter.restore();
    }
}

void ThunderFighterWindow::drawShield(QPainter &painter)
{
    if (hasShield && shieldTimeLeft > 0) {
        painter.save();
        painter.translate(playerPos);
        
        // 绘制护盾效果（半透明蓝色圆圈）
        int alpha = 150 + (shieldTimeLeft % 20) * 5; // 闪烁效果
        painter.setPen(QPen(QColor(0, 150, 255, alpha), 3));
        painter.setBrush(Qt::NoBrush);
        
        // 绘制多层护盾圆圈
        for (int i = 0; i < 3; ++i) {
            int radius = 20 + i * 5;
            painter.drawEllipse(-radius, -radius, radius * 2, radius * 2);
        }
        
        painter.restore();
    }
}

void ThunderFighterWindow::spawnPowerUp(const QPointF &pos, int type)
{
    PowerUp powerUp;
    powerUp.pos = pos;
    powerUp.type = type;
    // 初始随机方向
    double angle = QRandomGenerator::global()->bounded(360) * M_PI / 180.0;
    double speed = 1.0 + QRandomGenerator::global()->bounded(5) * 0.2;
    powerUp.vx = qCos(angle) * speed;
    powerUp.vy = qSin(angle) * speed + 0.5; // 总体向下
    powerUp.moveTimer = 30 + QRandomGenerator::global()->bounded(30);
    powerUps.append(powerUp);
}

void ThunderFighterWindow::applyPowerUp(int type)
{
    if (type == 0) {
        // 升级道具
        if (playerLevel < 5) {
            playerLevel++;
            bulletCount = qMin(5, playerLevel + 1); // 1级=2发, 2级=3发, ..., 5级=5发
            // 升级时不恢复血量（血量独立于等级）
        } else {
            // 满级后再吃到升级道具：触发"暴走"8秒（无阻力无重力，能量不损失）
            berserkTimeLeft = 480; // 8秒 = 480帧（60fps）
            // 暴走触发：不清屏敌弹，让火力立刻提升
            // 敌机被击毁时会清除其发射的子弹
            playerShootCooldown = 0; // 下一帧立即发射
        }
    } else if (type == 1) {
        // 护盾道具：多次获取时刷新护盾持续时间为8秒
        hasShield = true;
        shieldTimeLeft = 480; // 重置为8秒 = 480帧（60fps），无论当前剩余时间是多少
    } else if (type == 2) {
        // 治疗道具：恢复30点生命值（但不能超过最大血量）
        playerHealth += 30;
        if (playerHealth > maxHealth) {
            playerHealth = maxHealth;
        }
    }
}

void ThunderFighterWindow::tryDropUpgradeAndShield(const QPointF &pos)
{
    // 20%概率掉落道具，在这20%中：20%掉落治疗道具，40%掉落升级道具，40%掉落护盾道具
    int rand = QRandomGenerator::global()->bounded(100);
    if (rand < 20) {
        // 在20%概率内，按比例分配：治疗20%，升级40%，护盾40%
        int typeRand = QRandomGenerator::global()->bounded(100);
        if (typeRand < 20) {
            spawnPowerUp(pos, 2); // 治疗道具（20%）
        } else if (typeRand < 60) {
            spawnPowerUp(pos, 0); // 升级道具（40%）
        } else {
            spawnPowerUp(pos, 1); // 护盾道具（40%）
        }
    }
}

void ThunderFighterWindow::tryDropHealPowerUp(const QPointF &pos)
{
    // 已合并到tryDropUpgradeAndShield中，此函数不再使用
    // 保留函数以避免编译错误，但不再调用
}

bool ThunderFighterWindow::checkPlayerPowerUpCollision(const QPointF &playerPos, const PowerUp &powerUp)
{
    int powerUpSize = 15;
    int playerSize = 15;
    int attractRange = 50; // 加强吸附范围：50像素（原来是30，现在是50）
    QPointF diff = playerPos - powerUp.pos;
    // 增大碰撞检测范围，使道具更容易被拾取
    return (diff.x() * diff.x() + diff.y() * diff.y()) < ((powerUpSize + playerSize + attractRange) * (powerUpSize + playerSize + attractRange));
}

void ThunderFighterWindow::closeEvent(QCloseEvent *event)
{
    stopAllGameTimers();
    event->accept();
}

void ThunderFighterWindow::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_savedGameTimerActive = (gameState == PLAYING || gameState == PAUSED);
    m_savedBossTimerActive = bossSpawnTimerObj->isActive();
    stopAllGameTimers();
}

void ThunderFighterWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_inputTimer) {
        m_inputTimer->start(8);
    }
    if (m_playerMoveTimer) {
        m_playerMoveTimer->start();
    }
    if (m_savedGameTimerActive && gameState == PLAYING && gameTimer) {
        gameTimer->start(16);
        primeGameFrameClock();
    }
    if (m_savedBossTimerActive && gameState == PLAYING) {
        bossSpawnTimerObj->start(bossSpawnInterval);
    }
}
