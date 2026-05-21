#ifndef XIANGQI_ENGINE_H
#define XIANGQI_ENGINE_H

#include <QVector>

/** 中国象棋规则引擎（10 行 × 9 列，row0 为黑方底线，row9 为红方底线） */
class XiangqiEngine
{
public:
    enum class Piece : int {
        Empty = 0,
        RedGeneral = 1,
        RedAdvisor,
        RedElephant,
        RedHorse,
        RedChariot,
        RedCannon,
        RedSoldier,
        BlackGeneral = -1,
        BlackAdvisor = -2,
        BlackElephant = -3,
        BlackHorse = -4,
        BlackChariot = -5,
        BlackCannon = -6,
        BlackSoldier = -7,
    };

    enum class Side { Red, Black };

    enum class Result {
        Ongoing,
        RedWin,
        BlackWin,
        Draw,
    };

    struct Move {
        int fr = 0;
        int fc = 0;
        int tr = 0;
        int tc = 0;
    };

    static constexpr int kRows = 10;
    static constexpr int kCols = 9;

    XiangqiEngine();

    void resetToStart();
    Piece at(int row, int col) const;
    Side sideToMove() const { return m_side; }
    Result result() const { return m_result; }

    QVector<Move> legalMovesFrom(int row, int col) const;
    bool isLegalMove(const Move &mv) const;
    bool applyMove(const Move &mv);

    bool isInCheck(Side side) const;
    bool hasLegalMove(Side side) const;
    bool squareAttackedBy(int row, int col, Side bySide) const;

    /** 无子可动且未被将：和棋；无子可动且被将：负 */
    void updateResult();

    /** 双方都无法将死对方（子力不足等） */
    bool isDrawByRule() const;

    void resign(Side side);
    void flagTimeout(Side side);
    /** 双方同意且符合规则和棋条件 */
    void declareDraw();

    static bool isRed(Piece p);
    static bool isBlack(Piece p);
    static Side sideOf(Piece p);

private:
    bool inPalace(Side side, int row, int col) const;
    bool crossesRiver(Side side, int row) const;
    int countBetweenFiles(int r1, int c1, int r2, int c2) const;
    bool generalsFace() const;

    void addChariotMoves(int row, int col, QVector<Move> *out) const;
    void addCannonMoves(int row, int col, QVector<Move> *out) const;
    void addHorseMoves(int row, int col, QVector<Move> *out) const;
    void addElephantMoves(int row, int col, QVector<Move> *out) const;
    void addAdvisorMoves(int row, int col, QVector<Move> *out) const;
    void addGeneralMoves(int row, int col, QVector<Move> *out) const;
    void addSoldierMoves(int row, int col, QVector<Move> *out) const;
    QVector<Move> pseudoMovesFrom(int row, int col) const;

    bool wouldBeLegal(const Move &mv) const;

    int m_board[kRows][kCols] = {};
    Side m_side = Side::Red;
    Result m_result = Result::Ongoing;
};

#endif
