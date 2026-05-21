#ifndef GAME_2048_WINDOW_H
#define GAME_2048_WINDOW_H

#include <QWidget>

#include "shared_joy_state.h"

class QCloseEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QTimer;

/** 2048：菜单/暂停与雷霆战机一致；键盘 WASD 上下左右；摇杆轴方向同义 */
class Game2048Window : public QWidget
{
    Q_OBJECT

public:
    explicit Game2048Window(SharedJoyState *joyInput = nullptr, QWidget *parent = nullptr);
    ~Game2048Window();

    void prepareForShutdown();
    void scheduleJoyEdgeResync();
    void returnToLauncher();

signals:
    void requestReturnToSetup();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void pollJoyButtons();
    void tickJoyAxes();

private:
    enum GameState { MENU, PLAYING, PAUSED, GAME_OVER };
    enum PauseOverlayKind { PauseOverlayNone = 0, PauseOverlayMain, PauseOverlayExitConfirm };

    static constexpr int kSize = 4;
    static constexpr int kTilePx = 88;

    void initGame();
    void resetGame();
    bool tryMove(int dir); // 0 up 1 right 2 down 3 left
    void addRandomTile();
    bool canMoveAny() const;
    int maxTileValue() const;

    void openPauseMenu();
    void resumeFromPause();
    void resetJoyStickDiscreteState();
    void handleEscapeKey();
    void cyclePauseFocus();
    void activatePauseSelection();
    void drawPauseOverlay(class QPainter &painter);
    QRect pauseOverlayPanelRect() const;
    QRect pauseOverlayButtonRect(int row) const;

    int m_board[kSize][kSize];
    int m_score = 0;
    GameState m_gameState = MENU;
    PauseOverlayKind m_pauseOverlay = PauseOverlayNone;
    int m_pauseFocusIndex = 0;

    bool m_keyW = false;
    bool m_keyS = false;
    bool m_keyA = false;
    bool m_keyD = false;

    SharedJoyState *m_joyInput = nullptr;
    bool m_destroying = false;
    QTimer *m_inputTimer = nullptr;
    QTimer *m_axisTimer = nullptr;
    quint32 m_prevJoyButtons = 0;
    qint64 m_lastJoyMenuActionMs = 0;

    /** 摇杆：滞回 + 边沿 + DAS，避免连续帧一步变多步 */
    bool m_joyLatchU = false;
    bool m_joyLatchD = false;
    bool m_joyLatchL = false;
    bool m_joyLatchR = false;
    int m_joyDasU = 0;
    int m_joyDasD = 0;
    int m_joyDasL = 0;
    int m_joyDasR = 0;
    qint64 m_lastKbdVertMs = 0;
};

#endif
