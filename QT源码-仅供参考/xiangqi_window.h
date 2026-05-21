#ifndef XIANGQI_WINDOW_H
#define XIANGQI_WINDOW_H

#include <QVector>
#include <QWidget>

#include "shared_joy_state.h"
#include "xiangqi_engine.h"

class QCloseEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QTimer;

/** 中国象棋：P1 键盘；P2 摇杆；分侧菜单；双人确认不阻塞对方 */
class XiangqiWindow : public QWidget
{
    Q_OBJECT

public:
    explicit XiangqiWindow(SharedJoyState *joyInput = nullptr, QWidget *parent = nullptr);
    ~XiangqiWindow();

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
    void tickJoyCursor();
    void tickThinkTimer();

private:
    enum GameState { MENU, PLAYING, GAME_OVER };
    enum PickPhase { PickPiece, PickDestination };
    enum PauseOverlayKind {
        PauseOverlayNone = 0,
        PauseOverlayMain,
        PauseOverlayDualConfirm,
    };
    enum DualAction { DualPause, DualResume, DualExit, DualDraw };

    struct SideMenu {
        bool open = false;
        PauseOverlayKind overlay = PauseOverlayNone;
        int focusIndex = 0;
    };

    static constexpr int kCell = 56;
    static constexpr int kThinkSeconds = 60;
    static constexpr int kSidePanelW = 212;
    static constexpr int kMenuBtnW = 92;
    static constexpr int kMenuBtnH = 46;

    void initGame();
    void startMatch();
    void resetPick();
    void refreshHighlights();
    void restartThinkClock();
    void closeAllMenus();

    QRect boardRect() const;
    int riverCenterY() const;
    QPoint intersection(int row, int col) const;
    bool pointToCell(const QPoint &pt, int *row, int *col) const;

    void openMenuRed();
    void openMenuBlack();
    void closeMenuRed();
    void closeMenuBlack();
    void toggleMenuRed();
    void toggleMenuBlack();

    void cycleMenuFocus(SideMenu *menu, int delta);
    void activateMenuSelection(bool redSide);
    void menuSurrenderRed();
    void menuSurrenderBlack();

    void beginDualConfirm(DualAction action);
    void cancelDualConfirm();
    void resetDualConfirm();
    bool dualConfirmSatisfied() const;
    void applyDualAction();
    void registerDualOkRed();
    void registerDualOkBlack();
    void trySurrender(XiangqiEngine::Side side);

    void p1MoveCursor(int dr, int dc);
    void p2MoveCursor(int dr, int dc);
    void p1Confirm();
    void p2Confirm();

    void drawBoard(QPainter &painter) const;
    void drawPieces(QPainter &painter) const;
    void drawHighlights(QPainter &painter) const;
    void drawHud(QPainter &painter) const;
    void drawSideMenu(QPainter &painter, bool redSide) const;
    void drawMenuButtons(QPainter &painter) const;
    void drawCheckAlert(QPainter &painter, bool redSide) const;

    QRect menuButtonRectRed() const;
    QRect menuButtonRectBlack() const;
    QRect sideMenuPanelRect(bool redSide) const;
    QRect sideMenuButtonRect(bool redSide, int row) const;
    int pauseMenuItemCount() const;

    XiangqiEngine m_engine;
    GameState m_gameState = MENU;
    PickPhase m_pickPhase = PickPiece;

    SideMenu m_redMenu;
    SideMenu m_blackMenu;

    bool m_dualPending = false;
    DualAction m_dualAction = DualPause;
    bool m_redDualOk = false;
    bool m_blackDualOk = false;

    int m_cursorR = 7;
    int m_cursorC = 4;
    int m_selR = -1;
    int m_selC = -1;
    QVector<XiangqiEngine::Move> m_highlights;

    int m_thinkRemain = kThinkSeconds;
    /** 双方确认「暂停」后为 true，双方计时与走子均停止 */
    bool m_matchPaused = false;

    bool m_keyW = false;
    bool m_keyS = false;
    bool m_keyA = false;
    bool m_keyD = false;

    SharedJoyState *m_joyInput = nullptr;
    bool m_destroying = false;
    QTimer *m_inputTimer = nullptr;
    QTimer *m_axisTimer = nullptr;
    QTimer *m_thinkTimer = nullptr;
    quint32 m_prevJoyButtons = 0;
    qint64 m_lastJoyMenuActionMs = 0;
    qint64 m_lastKbdCursorMs = 0;

    bool m_joyLatchU = false;
    bool m_joyLatchD = false;
    bool m_joyLatchL = false;
    bool m_joyLatchR = false;
    int m_joyDasU = 0;
    int m_joyDasD = 0;
    int m_joyDasL = 0;
    int m_joyDasR = 0;
};

#endif
