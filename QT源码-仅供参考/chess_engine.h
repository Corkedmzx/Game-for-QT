#ifndef CHESS_ENGINE_H
#define CHESS_ENGINE_H

#include <QVector>

/** 国际象棋规则（8×8，row0 黑方底线，row7 白方底线；白先） */
class ChessEngine
{
public:
    enum class Piece : int {
        Empty = 0,
        WhitePawn = 1,
        WhiteKnight,
        WhiteBishop,
        WhiteRook,
        WhiteQueen,
        WhiteKing,
        BlackPawn = -1,
        BlackKnight = -2,
        BlackBishop = -3,
        BlackRook = -4,
        BlackQueen = -5,
        BlackKing = -6,
    };

    enum class Side { White, Black };

    enum class Result {
        Ongoing,
        WhiteWin,
        BlackWin,
        Draw,
    };

    struct Move {
        int fr = 0;
        int fc = 0;
        int tr = 0;
        int tc = 0;
        /** 升变目标；Empty 表示非升变或默认后 */
        Piece promotion = Piece::Empty;
    };

    static constexpr int kRows = 8;
    static constexpr int kCols = 8;

    ChessEngine();

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

    void updateResult();
    bool isDrawByRule() const;

    void resign(Side side);
    void flagTimeout(Side side);
    void declareDraw();

    static bool isWhite(Piece p);
    static bool isBlack(Piece p);
    static Side sideOf(Piece p);

private:
    void addPawnMoves(int row, int col, QVector<Move> *out) const;
    void addKnightMoves(int row, int col, QVector<Move> *out) const;
    void addSlidingMoves(int row, int col, const int dirs[][2], int dirCount, QVector<Move> *out) const;
    void addKingMoves(int row, int col, QVector<Move> *out) const;
    void addCastlingMoves(QVector<Move> *out) const;
    QVector<Move> pseudoMovesFrom(int row, int col) const;
    bool pieceAttacksSquare(Piece p, int row, int col, int tr, int tc) const;
    bool wouldBeLegal(const Move &mv) const;
    void applyMoveInternal(const Move &mv);

    int m_board[kRows][kCols] = {};
    Side m_side = Side::White;
    Result m_result = Result::Ongoing;

    bool m_whiteKingCastle = true;
    bool m_whiteQueenCastle = true;
    bool m_blackKingCastle = true;
    bool m_blackQueenCastle = true;
    int m_enPassRow = -1;
    int m_enPassCol = -1;
};

#endif
