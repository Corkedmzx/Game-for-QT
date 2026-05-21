#include "chess_engine.h"

#include <cstdlib>

namespace {

bool inBounds(int row, int col)
{
    return row >= 0 && row < ChessEngine::kRows && col >= 0 && col < ChessEngine::kCols;
}

bool moveInBounds(const ChessEngine::Move &mv)
{
    return inBounds(mv.fr, mv.fc) && inBounds(mv.tr, mv.tc);
}

} // namespace

ChessEngine::ChessEngine()
{
    resetToStart();
}

bool ChessEngine::isWhite(Piece p)
{
    return static_cast<int>(p) > 0;
}

bool ChessEngine::isBlack(Piece p)
{
    return static_cast<int>(p) < 0;
}

ChessEngine::Side ChessEngine::sideOf(Piece p)
{
    if (isWhite(p)) {
        return Side::White;
    }
    if (isBlack(p)) {
        return Side::Black;
    }
    return Side::White;
}

void ChessEngine::resetToStart()
{
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            m_board[r][c] = static_cast<int>(Piece::Empty);
        }
    }

    const int back[] = {
        static_cast<int>(Piece::BlackRook), static_cast<int>(Piece::BlackKnight),
        static_cast<int>(Piece::BlackBishop), static_cast<int>(Piece::BlackQueen),
        static_cast<int>(Piece::BlackKing), static_cast<int>(Piece::BlackBishop),
        static_cast<int>(Piece::BlackKnight), static_cast<int>(Piece::BlackRook),
    };
    for (int c = 0; c < kCols; ++c) {
        m_board[0][c] = back[c];
        m_board[7][c] = -back[c];
        m_board[1][c] = static_cast<int>(Piece::BlackPawn);
        m_board[6][c] = static_cast<int>(Piece::WhitePawn);
    }

    m_side = Side::White;
    m_result = Result::Ongoing;
    m_whiteKingCastle = m_whiteQueenCastle = true;
    m_blackKingCastle = m_blackQueenCastle = true;
    m_enPassRow = m_enPassCol = -1;
}

ChessEngine::Piece ChessEngine::at(int row, int col) const
{
    if (!inBounds(row, col)) {
        return Piece::Empty;
    }
    return static_cast<Piece>(m_board[row][col]);
}

void ChessEngine::addSlidingMoves(int row, int col, const int dirs[][2], int dirCount,
                                  QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    for (int d = 0; d < dirCount; ++d) {
        int r = row + dirs[d][0];
        int c = col + dirs[d][1];
        while (inBounds(r, c)) {
            const Piece p = at(r, c);
            if (p == Piece::Empty) {
                out->append({row, col, r, c});
            } else {
                if (sideOf(p) != me) {
                    out->append({row, col, r, c});
                }
                break;
            }
            r += dirs[d][0];
            c += dirs[d][1];
        }
    }
}

void ChessEngine::addKnightMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    const int jumps[8][2] = {{2, 1}, {2, -1}, {-2, 1}, {-2, -1}, {1, 2}, {1, -2}, {-1, 2}, {-1, -2}};
    for (const auto &j : jumps) {
        const int r = row + j[0];
        const int c = col + j[1];
        if (!inBounds(r, c)) {
            continue;
        }
        const Piece p = at(r, c);
        if (p == Piece::Empty || sideOf(p) != me) {
            out->append({row, col, r, c});
        }
    }
}

void ChessEngine::addPawnMoves(int row, int col, QVector<Move> *out) const
{
    const Piece piece = at(row, col);
    const Side me = sideOf(piece);
    const int forward = (me == Side::White) ? -1 : 1;
    const int startRow = (me == Side::White) ? 6 : 1;
    const int promoRow = (me == Side::White) ? 0 : 7;

    const int r1 = row + forward;
    if (inBounds(r1, col) && at(r1, col) == Piece::Empty) {
        Move mv{row, col, r1, col};
        if (r1 == promoRow) {
            mv.promotion = (me == Side::White) ? Piece::WhiteQueen : Piece::BlackQueen;
        }
        out->append(mv);

        if (row == startRow) {
            const int r2 = row + 2 * forward;
            if (at(r2, col) == Piece::Empty) {
                out->append({row, col, r2, col});
            }
        }
    }

    for (int dc : {-1, 1}) {
        const int r = row + forward;
        const int c = col + dc;
        if (!inBounds(r, c)) {
            continue;
        }
        const Piece tgt = at(r, c);
        if (tgt != Piece::Empty && sideOf(tgt) != me) {
            Move mv{row, col, r, c};
            if (r == promoRow) {
                mv.promotion = (me == Side::White) ? Piece::WhiteQueen : Piece::BlackQueen;
            }
            out->append(mv);
        } else if (m_enPassCol == c && m_enPassRow == r && tgt == Piece::Empty) {
            out->append({row, col, r, c});
        }
    }
}

void ChessEngine::addKingMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) {
                continue;
            }
            const int r = row + dr;
            const int c = col + dc;
            if (!inBounds(r, c)) {
                continue;
            }
            const Piece p = at(r, c);
            if (p == Piece::Empty || sideOf(p) != me) {
                out->append({row, col, r, c});
            }
        }
    }
}

void ChessEngine::addCastlingMoves(QVector<Move> *out) const
{
    if (m_side == Side::White) {
        if (m_whiteKingCastle && at(7, 7) == Piece::WhiteRook && at(7, 5) == Piece::Empty
            && at(7, 6) == Piece::Empty && !squareAttackedBy(7, 4, Side::Black)
            && !squareAttackedBy(7, 5, Side::Black) && !squareAttackedBy(7, 6, Side::Black)) {
            out->append({7, 4, 7, 6});
        }
        if (m_whiteQueenCastle && at(7, 0) == Piece::WhiteRook && at(7, 1) == Piece::Empty
            && at(7, 2) == Piece::Empty && at(7, 3) == Piece::Empty
            && !squareAttackedBy(7, 4, Side::Black) && !squareAttackedBy(7, 3, Side::Black)
            && !squareAttackedBy(7, 2, Side::Black)) {
            out->append({7, 4, 7, 2});
        }
    } else {
        if (m_blackKingCastle && at(0, 7) == Piece::BlackRook && at(0, 5) == Piece::Empty
            && at(0, 6) == Piece::Empty && !squareAttackedBy(0, 4, Side::White)
            && !squareAttackedBy(0, 5, Side::White) && !squareAttackedBy(0, 6, Side::White)) {
            out->append({0, 4, 0, 6});
        }
        if (m_blackQueenCastle && at(0, 0) == Piece::BlackRook && at(0, 1) == Piece::Empty
            && at(0, 2) == Piece::Empty && at(0, 3) == Piece::Empty
            && !squareAttackedBy(0, 4, Side::White) && !squareAttackedBy(0, 3, Side::White)
            && !squareAttackedBy(0, 2, Side::White)) {
            out->append({0, 4, 0, 2});
        }
    }
}

QVector<ChessEngine::Move> ChessEngine::pseudoMovesFrom(int row, int col) const
{
    QVector<Move> raw;
    const Piece p = at(row, col);
    if (p == Piece::Empty || sideOf(p) != m_side) {
        return raw;
    }

    switch (p) {
    case Piece::WhitePawn:
    case Piece::BlackPawn:
        addPawnMoves(row, col, &raw);
        break;
    case Piece::WhiteKnight:
    case Piece::BlackKnight:
        addKnightMoves(row, col, &raw);
        break;
    case Piece::WhiteBishop:
    case Piece::BlackBishop: {
        const int dirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        addSlidingMoves(row, col, dirs, 4, &raw);
        break;
    }
    case Piece::WhiteRook:
    case Piece::BlackRook: {
        const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        addSlidingMoves(row, col, dirs, 4, &raw);
        break;
    }
    case Piece::WhiteQueen:
    case Piece::BlackQueen: {
        const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        addSlidingMoves(row, col, dirs, 8, &raw);
        break;
    }
    case Piece::WhiteKing:
    case Piece::BlackKing:
        addKingMoves(row, col, &raw);
        break;
    default:
        break;
    }
    return raw;
}

bool ChessEngine::pieceAttacksSquare(Piece p, int row, int col, int tr, int tc) const
{
    if (p == Piece::Empty) {
        return false;
    }
    const Side s = sideOf(p);
    if (p == Piece::WhitePawn || p == Piece::BlackPawn) {
        const int forward = (s == Side::White) ? -1 : 1;
        return (row + forward == tr) && (col - 1 == tc || col + 1 == tc);
    }
    if (p == Piece::WhiteKnight || p == Piece::BlackKnight) {
        const int dr = qAbs(tr - row);
        const int dc = qAbs(tc - col);
        return (dr == 2 && dc == 1) || (dr == 1 && dc == 2);
    }
    if (p == Piece::WhiteKing || p == Piece::BlackKing) {
        return qAbs(tr - row) <= 1 && qAbs(tc - col) <= 1;
    }

    const bool bishopLike = (p == Piece::WhiteBishop || p == Piece::BlackBishop
                             || p == Piece::WhiteQueen || p == Piece::BlackQueen);
    const bool rookLike = (p == Piece::WhiteRook || p == Piece::BlackRook || p == Piece::WhiteQueen
                           || p == Piece::BlackQueen);
    const int dr = tr - row;
    const int dc = tc - col;
    if (dr == 0 && dc == 0) {
        return false;
    }
    if (rookLike && (dr == 0 || dc == 0)) {
        const int stepR = (dr == 0) ? 0 : (dr > 0 ? 1 : -1);
        const int stepC = (dc == 0) ? 0 : (dc > 0 ? 1 : -1);
        int r = row + stepR;
        int c = col + stepC;
        while (r != tr || c != tc) {
            if (at(r, c) != Piece::Empty) {
                return false;
            }
            r += stepR;
            c += stepC;
        }
        return true;
    }
    if (bishopLike && qAbs(dr) == qAbs(dc)) {
        const int stepR = dr > 0 ? 1 : -1;
        const int stepC = dc > 0 ? 1 : -1;
        int r = row + stepR;
        int c = col + stepC;
        while (r != tr || c != tc) {
            if (at(r, c) != Piece::Empty) {
                return false;
            }
            r += stepR;
            c += stepC;
        }
        return true;
    }
    return false;
}

bool ChessEngine::squareAttackedBy(int row, int col, Side bySide) const
{
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const Piece p = at(r, c);
            if (p == Piece::Empty || sideOf(p) != bySide) {
                continue;
            }
            if (pieceAttacksSquare(p, r, c, row, col)) {
                return true;
            }
        }
    }
    return false;
}

bool ChessEngine::wouldBeLegal(const Move &mv) const
{
    if (!moveInBounds(mv)) {
        return false;
    }
    const Side mover = m_side;
    ChessEngine sim = *this;
    sim.applyMoveInternal(mv);
    return !sim.isInCheck(mover);
}

QVector<ChessEngine::Move> ChessEngine::legalMovesFrom(int row, int col) const
{
    QVector<Move> legal;
    if (m_result != Result::Ongoing) {
        return legal;
    }
    const Piece p = at(row, col);
    if (p == Piece::Empty || sideOf(p) != m_side) {
        return legal;
    }

    QVector<Move> raw = pseudoMovesFrom(row, col);
    if (p == Piece::WhiteKing || p == Piece::BlackKing) {
        addCastlingMoves(&raw);
    }

    for (const Move &mv : raw) {
        if (wouldBeLegal(mv)) {
            legal.append(mv);
        }
    }
    return legal;
}

bool ChessEngine::isLegalMove(const Move &mv) const
{
    for (const Move &m : legalMovesFrom(mv.fr, mv.fc)) {
        if (m.tr == mv.tr && m.tc == mv.tc) {
            return true;
        }
    }
    return false;
}

void ChessEngine::applyMoveInternal(const Move &mv)
{
    const Piece moving = at(mv.fr, mv.fc);
    const Side mover = sideOf(moving);

    m_enPassRow = m_enPassCol = -1;

    /* 王车易位 */
    if ((moving == Piece::WhiteKing || moving == Piece::BlackKing) && mv.fc == 4
        && qAbs(mv.tc - mv.fc) == 2) {
        m_board[mv.tr][mv.tc] = static_cast<int>(moving);
        m_board[mv.fr][mv.fc] = static_cast<int>(Piece::Empty);
        if (mv.tc == 6) {
            m_board[mv.fr][5] = static_cast<int>(moving == Piece::WhiteKing ? Piece::WhiteRook
                                                                            : Piece::BlackRook);
            m_board[mv.fr][7] = static_cast<int>(Piece::Empty);
        } else {
            m_board[mv.fr][3] = static_cast<int>(moving == Piece::WhiteKing ? Piece::WhiteRook
                                                                            : Piece::BlackRook);
            m_board[mv.fr][0] = static_cast<int>(Piece::Empty);
        }
    } else {
        /* 吃过路 */
        if ((moving == Piece::WhitePawn || moving == Piece::BlackPawn) && mv.fc != mv.tc
            && at(mv.tr, mv.tc) == Piece::Empty) {
            m_board[mv.fr][mv.tc] = static_cast<int>(Piece::Empty);
        }
        m_board[mv.tr][mv.tc] = static_cast<int>(moving);
        m_board[mv.fr][mv.fc] = static_cast<int>(Piece::Empty);
    }

    /* 升变 */
    if (moving == Piece::WhitePawn || moving == Piece::BlackPawn) {
        const int promoRow = (mover == Side::White) ? 0 : 7;
        if (mv.tr == promoRow) {
            Piece promo = mv.promotion;
            if (promo == Piece::Empty) {
                promo = (mover == Side::White) ? Piece::WhiteQueen : Piece::BlackQueen;
            }
            m_board[mv.tr][mv.tc] = static_cast<int>(promo);
        }
        if ((mover == Side::White && mv.fr == 6 && mv.tr == 4)
            || (mover == Side::Black && mv.fr == 1 && mv.tr == 3)) {
            m_enPassRow = mv.fr + ((mover == Side::White) ? -1 : 1);
            m_enPassCol = mv.fc;
        }
    }

    /* 易位权 */
    if (moving == Piece::WhiteKing) {
        m_whiteKingCastle = m_whiteQueenCastle = false;
    } else if (moving == Piece::BlackKing) {
        m_blackKingCastle = m_blackQueenCastle = false;
    }
    if (mv.fr == 7 && mv.fc == 0) {
        m_whiteQueenCastle = false;
    }
    if (mv.fr == 7 && mv.fc == 7) {
        m_whiteKingCastle = false;
    }
    if (mv.fr == 0 && mv.fc == 0) {
        m_blackQueenCastle = false;
    }
    if (mv.fr == 0 && mv.fc == 7) {
        m_blackKingCastle = false;
    }
    if (at(7, 0) != Piece::WhiteRook) {
        m_whiteQueenCastle = false;
    }
    if (at(7, 7) != Piece::WhiteRook) {
        m_whiteKingCastle = false;
    }
    if (at(0, 0) != Piece::BlackRook) {
        m_blackQueenCastle = false;
    }
    if (at(0, 7) != Piece::BlackRook) {
        m_blackKingCastle = false;
    }

    m_side = (m_side == Side::White) ? Side::Black : Side::White;
}

bool ChessEngine::applyMove(const Move &mv)
{
    if (m_result != Result::Ongoing || !isLegalMove(mv)) {
        return false;
    }
    applyMoveInternal(mv);
    updateResult();
    return true;
}

bool ChessEngine::isInCheck(Side side) const
{
    int kr = -1;
    int kc = -1;
    const Piece king = (side == Side::White) ? Piece::WhiteKing : Piece::BlackKing;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            if (at(r, c) == king) {
                kr = r;
                kc = c;
                break;
            }
        }
    }
    if (kr < 0) {
        return true;
    }
    const Side opp = (side == Side::White) ? Side::Black : Side::White;
    return squareAttackedBy(kr, kc, opp);
}

bool ChessEngine::hasLegalMove(Side side) const
{
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            if (sideOf(at(r, c)) == side) {
                const QVector<Move> ms = legalMovesFrom(r, c);
                if (!ms.isEmpty()) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool ChessEngine::isDrawByRule() const
{
    int white = 0;
    int black = 0;
    bool wMinor = false;
    bool bMinor = false;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const Piece p = at(r, c);
            if (isWhite(p)) {
                ++white;
                if (p == Piece::WhiteBishop || p == Piece::WhiteKnight) {
                    wMinor = true;
                }
            } else if (isBlack(p)) {
                ++black;
                if (p == Piece::BlackBishop || p == Piece::BlackKnight) {
                    bMinor = true;
                }
            }
        }
    }
    if (white <= 1 && black <= 1) {
        return true;
    }
    if (white <= 2 && black <= 2 && !wMinor && !bMinor) {
        return true;
    }
    return false;
}

void ChessEngine::updateResult()
{
    if (m_result != Result::Ongoing) {
        return;
    }
    const bool inCheck = isInCheck(m_side);
    const bool canMove = hasLegalMove(m_side);
    if (!canMove) {
        if (inCheck) {
            m_result = (m_side == Side::White) ? Result::BlackWin : Result::WhiteWin;
        } else {
            m_result = Result::Draw;
        }
        return;
    }
    if (isDrawByRule()) {
        m_result = Result::Draw;
    }
}

void ChessEngine::resign(Side side)
{
    if (m_result != Result::Ongoing) {
        return;
    }
    m_result = (side == Side::White) ? Result::BlackWin : Result::WhiteWin;
}

void ChessEngine::flagTimeout(Side side)
{
    resign(side);
}

void ChessEngine::declareDraw()
{
    if (m_result != Result::Ongoing || !isDrawByRule()) {
        return;
    }
    m_result = Result::Draw;
}
