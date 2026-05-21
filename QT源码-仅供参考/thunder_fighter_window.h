#ifndef THUNDER_FIGHTER_WINDOW_H
#define THUNDER_FIGHTER_WINDOW_H

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "shared_joy_state.h"
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRect>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QFont>
#include <QColor>
#include <climits>

class QMouseEvent;
class QCloseEvent;
class QHideEvent;
class QShowEvent;
// 游戏对象结构
struct Bullet {
    QPointF pos;
    bool isPlayerBullet; // true为玩家子弹，false为敌机子弹
    double vx; // x方向速度
    double vy; // y方向速度
    int penetration; // 穿透次数（0表示普通子弹，>0表示可穿透的激光）
    int colorType; // 颜色类型：0=红色，1=蓝色，2=黄色（用于暴走弹幕）
    int ownerEnemyId; // 发射该子弹的敌机ID（-1表示玩家子弹或未指定）
    /** 生成时写入，碰撞时直接扣血，避免每帧重复浮点伤害公式（同类 STG 常用） */
    int hitDamage = 1;
};

struct Enemy {
    QPointF pos;
    QPointF targetPos; // 目标位置（用于队形排列）
    int type; // 敌机类型（0-2，不同大小和生命值）
    int health;
    double speed; // y方向速度（支持小数）
    double vx; // x方向速度（用于侧面平移和对角线移动）
    int shootCooldown; // 射击冷却计时器
    int shootPattern; // 射击模式（0=直线, 1=扇形, 2=圆形, 3=密集扇形等）
    int shootPatternTimer; // 射击模式切换计时器
    int spawnMode; // 出现方式：0=正上方下来，1=侧面平移，2=对角线
    bool hasReachedTarget; // 是否已到达目标位置
    int enterScreenTimer; // 进入屏幕后的计时器（帧数，进入屏幕1秒内不受伤害）
    int enemyId; // 敌机唯一ID（用于追踪其发射的子弹）
};

struct Explosion {
    QPointF pos;
    int frame; // 爆炸动画帧
    int maxFrames;
};

struct Boss {
    QPointF pos;
    int health; // 当前层生命值
    int maxHealth; // 当前层最大生命值
    int layer; // 当前层数（1或2）
    int maxLayer; // 最大层数（1或2）
    double speed; // 速度（支持小数）
    int shootCooldown; // 射击冷却
    bool isActive; // 是否激活
    bool isWarning; // 是否处于警告阶段
    int warningTimer; // 警告动画计时器
    int shootPatternTimer; // 射击模式切换计时器
    int shootPattern; // 当前射击模式（0=线性, 1=扇形, 2=圆形, 3=密集扇形等）
    int layerTransitionTimer; // 层转换等待计时器（双血条Boss用）
    int burstCount; // 当前连发计数（用于断续射击）
    int burstCooldown; // 连发间隔冷却
};

struct PowerUp {
    QPointF pos;
    double vx; // x方向速度
    double vy; // y方向速度
    int type; // 0=升级道具, 1=护盾道具（5级后）, 2=治疗道具
    int moveTimer; // 运动计时器（用于改变方向）
};

struct Drone {
    QPointF pos; // 当前位置
    QPointF targetPos; // 目标位置（用于弧形排列或攻击目标）
    int targetEnemyId; // 目标敌机ID（-1表示无目标）
    int targetBossIndex; // 目标Boss索引（-1表示无目标）
    int state; // 状态：0=待机（弧形排列），1=冲向目标，2=环绕目标
    int orbitTimer; // 预留计时器
    int laserAttackTimer; // 激光攻击计时器（30帧=0.5秒）
    int laserVisualTimer; // 激光轨迹可见计时器（18帧=0.3秒）
    QPointF laserStartPos; // 激光起点（用于绘制轨迹）
    QPointF laserEndPos; // 激光终点（用于绘制轨迹）
    double orbitAngle; // 环绕角度
    double orbitRadius; // 环绕半径
    double orbitSpeed; // 环绕速度（随机，避免重叠）
    double angle; // 浮游炮朝向角度（用于绘制）
};

/**
 * 雷霆战机游戏窗口类
 * 实现经典的2D射击游戏
 */
class ThunderFighterWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ThunderFighterWindow(SharedJoyState *joyInput = nullptr, QWidget *parent = nullptr);
    ~ThunderFighterWindow();

    /** 进入游戏页时调用，避免切换瞬间产生虚假「按下边沿」 */
    void resetJoyButtonEdges();

    /** 切换页面后多次同步按键边沿，避免仍按住 A 时误判连击 */
    void scheduleJoyEdgeResync();

    /** 退出应用或切页前停止本窗口全部 QTimer（单线程主循环，避免残留节拍） */
    void stopAllGameTimers();
    /** 退出前：断开所有发往本对象的连接并停表，避免析构后定时器槽仍触发导致悬空 this / m_joyInput */
    void prepareForShutdown();

    /** 返回主窗口游戏首页时重置为本游戏内菜单状态（由 MainWindow 调用） */
    void returnToLauncher();

signals:
    /** 请求回到主窗口「游戏」标签页的项目展示区（ESC / 摇杆 D / 暂停菜单确认） */
    void requestReturnToSetup();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void updateGame(); // 游戏主循环（主线程 QTimer）
    void spawnEnemy(); // 生成敌机
    void pollJoyHardwareButtons();
    /** 与鼠标 Move 事件类似：高频刷新键盘/摇杆位移，与 updateGame 负载解耦 */
    void tickPlayerMoveInput();

private:
    // 游戏状态
    enum GameState {
        MENU,      // 菜单
        PLAYING,   // 游戏中
        PAUSED,    // 暂停
        GAME_OVER  // 游戏结束
    };
    
    GameState gameState;

    /** 暂停时的叠加菜单（仅 gameState==PAUSED 时使用） */
    enum PauseOverlayKind {
        PauseOverlayNone = 0,
        PauseOverlayMain,
        PauseOverlayExitConfirm
    };
    PauseOverlayKind m_pauseOverlay = PauseOverlayNone;
    int m_pauseFocusIndex = 0;
    
    // 玩家相关
    QPointF playerPos;
    int playerSpeed;
    int playerHealth;
    int maxHealth;
    int playerLevel; // 玩家升级等级（0-5）
    int bulletCount; // 子弹数量（弹道数）
    int shootCooldownBase; // 基础射击冷却
    int playerShootCooldown; // 玩家射击冷却（帧数）
    bool hasShield; // 是否有护盾
    int shieldTimeLeft; // 护盾剩余时间（帧数，60fps）
    int invincibleTime; // 无敌时间（帧数，60fps，3秒=180帧）
    int invincibleFlashTimer; // 闪烁计时器（用于闪烁效果）
    QList<QPointF> wingmanPositions; // 僚机位置列表
    int laserCooldown; // 穿透激光冷却
    int laserDuration; // 激光持续时间（帧数，0表示未激活）
    int berserkTimeLeft; // 暴走剩余时间（帧数，60fps；0表示未激活）
    double berserkBarrageAngle; // 暴走弹幕旋转角度（用于三角函数扫射效果）
    
    // 游戏对象列表
    QList<Bullet> playerBullets;
    QList<Bullet> enemyBullets;
    QList<Enemy> enemies;
    QList<Explosion> explosions;
    QList<Boss> bosses;
    QList<PowerUp> powerUps;
    QList<Drone> drones; // 浮游炮列表
    
    // 游戏参数
    int score;
    int level;
    int enemySpawnTimer;
    int enemySpawnInterval;
    int enemyGroupSpawnTimer; // 敌机组生成等待计时器
    int enemyGroupSpawnInterval; // 敌机组生成间隔（帧数，约2-3秒）
    int bossSpawnTimer; // Boss生成计时器
    int bossSpawnInterval; // Boss生成间隔（毫秒）
    int bossKillCount; // Boss击杀计数
    int difficultyLevel; // 难度等级（每击杀一个Boss增加）
    int enemyWaveCount; // 敌机波数计数（每5波生成一次Boss）
    int totalWaveCount; // 全局波次计数（包含Boss波次：5、10、15...）
    int nextEnemyId; // 下一个敌机ID（用于唯一标识敌机）

    /** 敌机波次真空期（不用 static，避免新一局沿用上一局状态） */
    bool m_spawnInVacuumPeriod = false;
    int m_spawnVacuumTimer = 0;

    // 键盘状态
    bool keyLeft;
    bool keyRight;
    bool keyUp;
    bool keyDown;
    bool keyW;
    bool keyA;
    bool keyS;
    bool keyD;
    bool keySpace;
    bool keyPause;

    /** 摇杆轴滞回：避免中立点附近抖动导致断断续续移动 */
    bool m_joyMotionActive = false;

    // 鼠标拖动状态
    bool mouseDragging;
    QPointF mouseDragOffset; // playerPos(中心点) - 鼠标位置
    
    /** 主线程 ~60fps 游戏节拍（与绘制同在 GUI 线程，无附加工作线程） */
    QTimer *gameTimer = nullptr;
    /** 测量相邻两次 updateGame 间隔，供键盘/摇杆位移与 16ms 参考帧对齐 */
    QElapsedTimer m_gameFrameClock;
    /** 与 setFixedSize 一致；用于游戏逻辑边界（可与 width()/height 一致） */
    int m_arenaWidth = 800;
    int m_arenaHeight = 600;

    QTimer *enemySpawnTimerObj;
    QTimer *bossSpawnTimerObj;
    /** 独立的按键轮询，暂停/菜单/结束时仍可响应摇杆 */
    QTimer *m_inputTimer = nullptr;
    /** 键盘/摇杆位移节拍（与鼠标 move 同为高频输入通道） */
    QTimer *m_playerMoveTimer = nullptr;
    QElapsedTimer m_kbdJoyMoveClock;
    quint32 m_prevJoyButtons = 0;
    qint64 m_lastJoyMenuActionMs = 0;

    /** 切到校准页时暂停定时器，避免与校准页并发访问共享状态 */
    bool m_savedGameTimerActive = false;
    bool m_savedBossTimerActive = false;

    /** HUD：仅在数值变化时重建 QString，减轻 paintEvent 中文本布局（DirectWrite）压力 */
    QFont m_hudFont;
    int m_hudSigScore = INT_MIN;
    int m_hudSigLevel = INT_MIN;
    int m_hudSigWave = INT_MIN;
    int m_hudSigPlane = INT_MIN;
    int m_hudSigHealth = INT_MIN;
    int m_hudSigShieldSec = INT_MIN;
    int m_hudSigBerserkSec = INT_MIN;
    int m_hudSigDiff = INT_MIN;
    bool m_hudSigHasShield = false;
    bool m_hudSigBerserkLine = false;
    QString m_hudTextScore;
    QString m_hudTextLevel;
    QString m_hudTextWave;
    QString m_hudTextPlane;
    QString m_hudTextHealth;
    QString m_hudTextShield;
    QString m_hudTextBerserk;
    QString m_hudTextDifficulty;
    void invalidateHudTextCache();

    SharedJoyState *m_joyInput = nullptr;
    bool m_destroying = false;
    
    // 辅助函数
    void initGame();
    void resetGame();
    void openPauseMenu();
    void resumeFromPause();
    void handleEscapeKey();
    void cyclePauseFocus();
    void activatePauseSelection();
    void drawPauseOverlay(QPainter &painter);
    QRect pauseOverlayPanelRect() const;
    QRect pauseOverlayButtonRect(int row) const;
    /** 射击与僚机逻辑（位移由 tickPlayerMoveInput 处理） */
    void updatePlayer();
    void applyKeyboardJoyMovementStep(double moveScale);
    void primeGameFrameClock();
    void updateBullets();
    void updateEnemies();
    void updateBosses();
    void updatePowerUps();
    void updateShield();
    void updateLaser();
    void updateInvincible(); // 更新无敌时间
    void updateBerserk(); // 更新暴走时间
    void updateDrones(); // 更新浮游炮
    void updateExplosions();
    void checkCollisions();
    void drawBackground(QPainter &painter);
    void drawPlayer(QPainter &painter);
    void drawBullets(QPainter &painter);
    void drawEnemies(QPainter &painter);
    void drawBosses(QPainter &painter);
    void drawPowerUps(QPainter &painter);
    void drawDrones(QPainter &painter); // 绘制浮游炮
    void drawShield(QPainter &painter);
    void drawExplosions(QPainter &painter);
    void drawUI(QPainter &painter);
    void drawMenu(QPainter &painter);
    void drawGameOver(QPainter &painter);
    void addExplosion(const QPointF &pos);
    void spawnBoss();
    void spawnPowerUp(const QPointF &pos, int type);
    void applyPowerUp(int type);
    void tryDropUpgradeAndShield(const QPointF &pos);
    void tryDropHealPowerUp(const QPointF &pos);
    bool checkBulletEnemyCollision(const Bullet &bullet, const Enemy &enemy, bool skipBroadPhase = false);
    bool checkBulletBossCollision(const Bullet &bullet, const Boss &boss);
    bool checkPlayerEnemyCollision(const QPointF &playerPos, const Enemy &enemy);
    bool checkPlayerBossCollision(const QPointF &playerPos, const Boss &boss);
    bool checkPlayerBulletCollision(const QPointF &playerPos, const Bullet &bullet);
    bool checkPlayerPowerUpCollision(const QPointF &playerPos, const PowerUp &powerUp);
    bool checkLaserEnemyCollision(const QPointF &laserStart, const QPointF &laserEnd, const Enemy &enemy);
    bool checkLaserBossCollision(const QPointF &laserStart, const QPointF &laserEnd, const Boss &boss);
};

#endif // THUNDER_FIGHTER_WINDOW_H
