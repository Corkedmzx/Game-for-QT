#ifndef TETRIS_WINDOW_H
#define TETRIS_WINDOW_H

#include <QVector>
#include <QWidget>

#include "shared_joy_state.h"

class QCloseEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QTimer;

/** 俄罗斯方块：菜单/暂停与雷霆战机一致；键盘 A/D 左右、W 旋转、S 硬降；摇杆轴同向 */
class TetrisWindow : public QWidget
{
    Q_OBJECT

public:
    explicit TetrisWindow(SharedJoyState *joyInput = nullptr, QWidget *parent = nullptr);
    ~TetrisWindow();

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
    void tickGame();
    void tickLineClearAnim();
    void pollJoyButtons();
    void tickJoyAxes();

private:
    static constexpr int kCols = 10;
    static constexpr int kRows = 20;
    static constexpr int kCell = 24;

    enum GameState { MENU, PLAYING, PAUSED, GAME_OVER };
    enum PauseOverlayKind { PauseOverlayNone = 0, PauseOverlayMain, PauseOverlayExitConfirm };

    void initGame();
    void resetGame();
    void spawnPiece();
    bool pieceFits(int type, int rot, int px, int py) const;
    void lockPiece();
    int findFullLines();
    void applyLineClear();
    void beginLineClearAnim();
    void finishLineClearAnim();
    void tryShift(int dx, int dy);
    void tryRotate();
    void hardDrop();

    void openPauseMenu();
    void resumeFromPause();
    void resetJoyStickDiscreteState();
    void handleEscapeKey();
    void cyclePauseFocus();
    void activatePauseSelection();
    void drawPauseOverlay(class QPainter &painter);
    void drawBoardCells(QPainter &painter, int ox, int oy) const;
    QRect pauseOverlayPanelRect() const;
    QRect pauseOverlayButtonRect(int row) const;

    int gridAt(int row, int col) const;
    void setGridAt(int row, int col, int value);

    SharedJoyState *m_joyInput = nullptr;
    bool m_destroying = false;
    QTimer *m_gameTimer = nullptr;
    QTimer *m_clearAnimTimer = nullptr;
    QTimer *m_inputTimer = nullptr;
    QTimer *m_axisTimer = nullptr;

    bool m_lineClearAnim = false;
    int m_clearRows[kRows] = {};
    int m_clearLineCount = 0;
    int m_clearAnimStep = 0;
    static constexpr int kClearFlashSteps = 8;
    static constexpr int kClearCollapseSteps = 6;

    int m_curType = 0;
    int m_curRot = 0;
    int m_curX = 0;
    int m_curY = 0;
    int m_nextType = 0;

    GameState m_gameState = MENU;
    PauseOverlayKind m_pauseOverlay = PauseOverlayNone;
    int m_pauseFocusIndex = 0;

    int m_score = 0;
    int m_lines = 0;
    int m_dropIntervalMs = 800;

    bool m_keyA = false;
    bool m_keyD = false;
    bool m_keyW = false;
    bool m_keyS = false;

    quint32 m_prevJoyButtons = 0;
    qint64 m_lastJoyMenuActionMs = 0;

    /** 摇杆：滞回 + 边沿；左右 DAS；上/下（旋转/硬降）仅边沿一次，避免串口连续帧连发 */
    bool m_joyLatchL = false;
    bool m_joyLatchR = false;
    bool m_joyLatchU = false;
    bool m_joyLatchD = false;
    int m_joyDasL = 0;
    int m_joyDasR = 0;
    qint64 m_lastJoyVertMs = 0;
    qint64 m_lastKbdVertMs = 0;

    /** 堆上棋盘，避免类内 800 字节数组在 MSVC 下破坏 QWidget/QObject 布局 */
    QVector<int> m_grid;
};

#endif
