#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include "shared_joy_state.h"

class JoystickSetupWidget;
class QCloseEvent;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTimer;
class ThunderFighterWindow;
class TetrisWindow;
class Game2048Window;
class XiangqiWindow;
class ChessWindow;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    /** 退出前兜底：停止游戏内所有 QTimer，避免残留触发 */
    void shutdownGameTimers();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    /** 游戏内请求回到「游戏」标签页的项目展示区 */
    void onReturnToLauncher();
    /** 串口已连接时：主页 B 选项目、A 确认、C 切标签（与 ThunderFighterWindow 内逻辑互斥） */
    void pollLauncherJoyButtons();

private:
    void enterThunderGame();
    void enterTetrisGame();
    void enterGame2048();
    void enterXiangqiGame();
    void enterChessGame();
    void cycleLauncherGameFocus();
    void updateLauncherGameVisual();
    void ensureXiangqiWindow();
    void ensureChessWindow();

    /** 启动器卡片按钮：0 雷霆 / 1 方块 / 2 2048 / 3 象棋 / 4 国际象棋 */
    QPushButton *launcherGameButton(int index) const;
    void syncMainJoyButtonEdges();
    void ensureTetrisWindow();

    /** 幂等：先停游戏/串口再同步拆掉中央控件树，避免成员 m_joyState 先于子控件析构 */
    void teardownBeforeJoyStateDestroyed();

    SharedJoyState m_joyState{};
    QTabWidget *m_tabs = nullptr;
    /** 「游戏」标签内：0=项目列表 1=雷霆 2=俄罗斯方块 3=2048 */
    QStackedWidget *m_gameStack = nullptr;
    QPushButton *m_btnThunder = nullptr;
    QPushButton *m_btnTetris = nullptr;
    QPushButton *m_btn2048 = nullptr;
    QPushButton *m_btnXiangqi = nullptr;
    QPushButton *m_btnChess = nullptr;
    QWidget *m_wrapTetris = nullptr;
    class QVBoxLayout *m_tetrisVBox = nullptr;
    JoystickSetupWidget *m_setup = nullptr;
    ThunderFighterWindow *m_game = nullptr;
    TetrisWindow *m_tetris = nullptr;
    Game2048Window *m_game2048 = nullptr;
    XiangqiWindow *m_xiangqi = nullptr;
    QWidget *m_wrapXiangqi = nullptr;
    ChessWindow *m_chess = nullptr;
    QWidget *m_wrapChess = nullptr;
    bool m_teardownDone = false;

    QTimer *m_joyPollTimer = nullptr;
    quint32 m_prevJoyMaskMain = 0;
    qint64 m_lastJoyTabMs = 0;
    qint64 m_lastJoyCycleMs = 0;
    qint64 m_lastJoyConfirmMs = 0;
    int m_launcherGameIndex = 0;
    static constexpr int kLauncherGameCount = 5;
};

#endif
