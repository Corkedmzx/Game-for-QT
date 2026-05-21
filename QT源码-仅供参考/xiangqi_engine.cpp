#include "xiangqi_engine.h"

namespace {

bool inBounds(int row, int col)
{
    return row >= 0 && row < XiangqiEngine::kRows && col >= 0 && col < XiangqiEngine::kCols;
}

bool moveInBounds(const XiangqiEngine::Move &mv)
{
    return inBounds(mv.fr, mv.fc) && inBounds(mv.tr, mv.tc);
}

} // namespace

XiangqiEngine::XiangqiEngine()
{
    resetToStart();
}

bool XiangqiEngine::isRed(Piece p)
{
    return static_cast<int>(p) > 0;
}

bool XiangqiEngine::isBlack(Piece p)
{
    return static_cast<int>(p) < 0;
}

XiangqiEngine::Side XiangqiEngine::sideOf(Piece p)
{
    if (isRed(p)) {
        return Side::Red;
    }
    if (isBlack(p)) {
        return Side::Black;
    }
    return Side::Red;
}

void XiangqiEngine::resetToStart()
{
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            m_board[r][c] = static_cast<int>(Piece::Empty);
        }
    }

    const int back[] = {
        static_cast<int>(Piece::BlackChariot), static_cast<int>(Piece::BlackHorse),
        static_cast<int>(Piece::BlackElephant), static_cast<int>(Piece::BlackAdvisor),
        static_cast<int>(Piece::BlackGeneral), static_cast<int>(Piece::BlackAdvisor),
        static_cast<int>(Piece::BlackElephant), static_cast<int>(Piece::BlackHorse),
        static_cast<int>(Piece::BlackChariot),
    };
    for (int c = 0; c < kCols; ++c) {
        m_board[0][c] = back[c];
        m_board[9][c] = -back[c];
    }
    m_board[2][1] = static_cast<int>(Piece::BlackCannon);
    m_board[2][7] = static_cast<int>(Piece::BlackCannon);
    m_board[7][1] = static_cast<int>(Piece::RedCannon);
    m_board[7][7] = static_cast<int>(Piece::RedCannon);
    for (int c = 0; c < kCols; c += 2) {
        m_board[3][c] = static_cast<int>(Piece::BlackSoldier);
        m_board[6][c] = static_cast<int>(Piece::RedSoldier);
    }

    m_side = Side::Red;
    m_result = Result::Ongoing;
}

XiangqiEngine::Piece XiangqiEngine::at(int row, int col) const
{
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) {
        return Piece::Empty;
    }
    return static_cast<Piece>(m_board[row][col]);
}

bool XiangqiEngine::inPalace(Side side, int row, int col) const
{
    if (col < 3 || col > 5) {
        return false;
    }
    if (side == Side::Red) {
        return row >= 7 && row <= 9;
    }
    return row >= 0 && row <= 2;
}

bool XiangqiEngine::crossesRiver(Side side, int row) const
{
    if (side == Side::Red) {
        return row <= 4;
    }
    return row >= 5;
}

int XiangqiEngine::countBetweenFiles(int r1, int c1, int r2, int c2) const
{
    int cnt = 0;
    if (r1 == r2) {
        const int step = c1 < c2 ? 1 : -1;
        for (int c = c1 + step; c != c2; c += step) {
            if (at(r1, c) != Piece::Empty) {
                ++cnt;
            }
        }
    } else if (c1 == c2) {
        const int step = r1 < r2 ? 1 : -1;
        for (int r = r1 + step; r != r2; r += step) {
            if (at(r, c1) != Piece::Empty) {
                ++cnt;
            }
        }
    }
    return cnt;
}

bool XiangqiEngine::generalsFace() const
{
    int rc = -1;
    int bc = -1;
    int rr = -1;
    int br = -1;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            if (at(r, c) == Piece::RedGeneral) {
                rr = r;
                rc = c;
            } else if (at(r, c) == Piece::BlackGeneral) {
                br = r;
                bc = c;
            }
        }
    }
    if (rc < 0 || bc < 0 || rc != bc) {
        return false;
    }
    return countBetweenFiles(rr, rc, br, bc) == 0;
}

void XiangqiEngine::addChariotMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto &d : dirs) {
        int r = row + d[0];
        int c = col + d[1];
        while (r >= 0 && r < kRows && c >= 0 && c < kCols) {
            const Piece p = at(r, c);
            if (p == Piece::Empty) {
                out->append({row, col, r, c});
            } else {
                if (sideOf(p) != me) {
                    out->append({row, col, r, c});
                }
                break;
            }
            r += d[0];
            c += d[1];
        }
    }
}

void XiangqiEngine::addCannonMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto &d : dirs) {
        int r = row + d[0];
        int c = col + d[1];
        bool jumped = false;
        while (r >= 0 && r < kRows && c >= 0 && c < kCols) {
            const Piece p = at(r, c);
            if (!jumped) {
                if (p == Piece::Empty) {
                    out->append({row, col, r, c});
                } else {
                    jumped = true;
                }
            } else {
                if (p != Piece::Empty) {
                    if (sideOf(p) != me) {
                        out->append({row, col, r, c});
                    }
                    break;
                }
            }
            r += d[0];
            c += d[1];
        }
    }
}

void XiangqiEngine::addHorseMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    const int offs[8][4] = {
        {-2, -1, -1, 0}, {-2, 1, -1, 0}, {2, -1, 1, 0}, {2, 1, 1, 0},
        {-1, -2, 0, -1}, {-1, 2, 0, 1}, {1, -2, 0, -1}, {1, 2, 0, 1},
    };
    for (const auto &o : offs) {
        const int br = row + o[2];
        const int bc = col + o[3];
        const int tr = row + o[0];
        const int tc = col + o[1];
        if (br < 0 || br >= kRows || bc < 0 || bc >= kCols || at(br, bc) != Piece::Empty) {
            continue;
        }
        if (tr < 0 || tr >= kRows || tc < 0 || tc >= kCols) {
            continue;
        }
        const Piece tgt = at(tr, tc);
        if (tgt == Piece::Empty || sideOf(tgt) != me) {
            out->append({row, col, tr, tc});
        }
    }
}

void XiangqiEngine::addElephantMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    const int offs[4][4] = {{-2, -2, -1, -1}, {-2, 2, -1, 1}, {2, -2, 1, -1}, {2, 2, 1, 1}};
    for (const auto &o : offs) {
        const int er = row + o[2];
        const int ec = col + o[3];
        const int tr = row + o[0];
        const int tc = col + o[1];
        if (crossesRiver(me, tr)) {
            continue;
        }
        if (er < 0 || er >= kRows || ec < 0 || ec >= kCols || at(er, ec) != Piece::Empty) {
            continue;
        }
        if (tr < 0 || tr >= kRows || tc < 0 || tc >= kCols) {
            continue;
        }
        const Piece tgt = at(tr, tc);
        if (tgt == Piece::Empty || sideOf(tgt) != me) {
            out->append({row, col, tr, tc});
        }
    }
}

void XiangqiEngine::addAdvisorMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    const int offs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (const auto &o : offs) {
        const int tr = row + o[0];
        const int tc = col + o[1];
        if (!inPalace(me, tr, tc)) {
            continue;
        }
        const Piece tgt = at(tr, tc);
        if (tgt == Piece::Empty || sideOf(tgt) != me) {
            out->append({row, col, tr, tc});
        }
    }
}

void XiangqiEngine::addGeneralMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    const int offs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto &o : offs) {
        const int tr = row + o[0];
        const int tc = col + o[1];
        if (!inPalace(me, tr, tc)) {
            continue;
        }
        const Piece tgt = at(tr, tc);
        if (tgt == Piece::Empty || sideOf(tgt) != me) {
            out->append({row, col, tr, tc});
        }
    }
    /* 将帅对脸吃子 */
    if (me == Side::Red) {
        for (int r = row - 1; r >= 0; --r) {
            const Piece p = at(r, col);
            if (p == Piece::Empty) {
                continue;
            }
            if (p == Piece::BlackGeneral) {
                out->append({row, col, r, col});
            }
            break;
        }
    } else {
        for (int r = row + 1; r < kRows; ++r) {
            const Piece p = at(r, col);
            if (p == Piece::Empty) {
                continue;
            }
            if (p == Piece::RedGeneral) {
                out->append({row, col, r, col});
            }
            break;
        }
    }
}

void XiangqiEngine::addSoldierMoves(int row, int col, QVector<Move> *out) const
{
    const Side me = sideOf(at(row, col));
    const int forward = (me == Side::Red) ? -1 : 1;
    const int trF = row + forward;
    if (trF >= 0 && trF < kRows) {
        const Piece tgt = at(trF, col);
        if (tgt == Piece::Empty || sideOf(tgt) != me) {
            out->append({row, col, trF, col});
        }
    }
    if (crossesRiver(me, row)) {
        for (int dc : {-1, 1}) {
            const int tc = col + dc;
            if (tc < 0 || tc >= kCols) {
                continue;
            }
            const Piece tgt = at(row, tc);
            if (tgt == Piece::Empty || sideOf(tgt) != me) {
                out->append({row, col, row, tc});
            }
        }
    }
}

QVector<XiangqiEngine::Move> XiangqiEngine::pseudoMovesFrom(int row, int col) const
{
    QVector<Move> raw;
    const Piece p = at(row, col);
    if (p == Piece::Empty) {
        return raw;
    }

    switch (p) {
    case Piece::RedChariot:
    case Piece::BlackChariot:
        addChariotMoves(row, col, &raw);
        break;
    case Piece::RedCannon:
    case Piece::BlackCannon:
        addCannonMoves(row, col, &raw);
        break;
    case Piece::RedHorse:
    case Piece::BlackHorse:
        addHorseMoves(row, col, &raw);
        break;
    case Piece::RedElephant:
    case Piece::BlackElephant:
        addElephantMoves(row, col, &raw);
        break;
    case Piece::RedAdvisor:
    case Piece::BlackAdvisor:
        addAdvisorMoves(row, col, &raw);
        break;
    case Piece::RedGeneral:
    case Piece::BlackGeneral:
        addGeneralMoves(row, col, &raw);
        break;
    case Piece::RedSoldier:
    case Piece::BlackSoldier:
        addSoldierMoves(row, col, &raw);
        break;
    default:
        break;
    }
    return raw;
}

QVector<XiangqiEngine::Move> XiangqiEngine::legalMovesFrom(int row, int col) const
{
    QVector<Move> legal;
    if (m_result != Result::Ongoing) {
        return legal;
    }
    const Piece p = at(row, col);
    if (p == Piece::Empty || sideOf(p) != m_side) {
        return legal;
    }

    for (const Move &mv : pseudoMovesFrom(row, col)) {
        if (wouldBeLegal(mv)) {
            legal.append(mv);
        }
    }
    return legal;
}

bool XiangqiEngine::squareAttackedBy(int row, int col, Side bySide) const
{
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const Piece p = at(r, c);
            if (p == Piece::Empty || sideOf(p) != bySide) {
                continue;
            }
            for (const Move &mv : pseudoMovesFrom(r, c)) {
                if (mv.tr == row && mv.tc == col) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool XiangqiEngine::wouldBeLegal(const Move &mv) const
{
    if (!moveInBounds(mv)) {
        return false;
    }
    XiangqiEngine sim = *this;
    sim.m_board[mv.tr][mv.tc] = sim.m_board[mv.fr][mv.fc];
    sim.m_board[mv.fr][mv.fc] = static_cast<int>(Piece::Empty);
    return !sim.isInCheck(m_side) && !sim.generalsFace();
}

bool XiangqiEngine::isLegalMove(const Move &mv) const
{
    const QVector<Move> ms = legalMovesFrom(mv.fr, mv.fc);
    for (const Move &m : ms) {
        if (m.tr == mv.tr && m.tc == mv.tc) {
            return true;
        }
    }
    return false;
}

bool XiangqiEngine::applyMove(const Move &mv)
{
    if (m_result != Result::Ongoing || !moveInBounds(mv) || !isLegalMove(mv)) {
        return false;
    }
    m_board[mv.tr][mv.tc] = m_board[mv.fr][mv.fc];
    m_board[mv.fr][mv.fc] = static_cast<int>(Piece::Empty);
    m_side = (m_side == Side::Red) ? Side::Black : Side::Red;
    updateResult();
    return true;
}

bool XiangqiEngine::isInCheck(Side side) const
{
    int gr = -1;
    int gc = -1;
    const Piece g = (side == Side::Red) ? Piece::RedGeneral : Piece::BlackGeneral;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            if (at(r, c) == g) {
                gr = r;
                gc = c;
                break;
            }
        }
    }
    if (gr < 0) {
        return true;
    }
    const Side opp = (side == Side::Red) ? Side::Black : Side::Red;
    return squareAttackedBy(gr, gc, opp);
}

void XiangqiEngine::resign(Side side)
{
    if (m_result != Result::Ongoing) {
        return;
    }
    m_result = (side == Side::Red) ? Result::BlackWin : Result::RedWin;
}

void XiangqiEngine::flagTimeout(Side side)
{
    resign(side);
}

void XiangqiEngine::declareDraw()
{
    if (m_result != Result::Ongoing || !isDrawByRule()) {
        return;
    }
    m_result = Result::Draw;
}

bool XiangqiEngine::hasLegalMove(Side side) const
{
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            if (sideOf(at(r, c)) == side && !legalMovesFrom(r, c).isEmpty()) {
                return true;
            }
        }
    }
    return false;
}

bool XiangqiEngine::isDrawByRule() const
{
    int red = 0;
    int black = 0;
    bool redChariot = false;
    bool blackChariot = false;
    bool redHorse = false;
    bool blackHorse = false;
    bool redCannon = false;
    bool blackCannon = false;

    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const Piece p = at(r, c);
            if (isRed(p)) {
                ++red;
                if (p == Piece::RedChariot) {
                    redChariot = true;
                } else if (p == Piece::RedHorse) {
                    redHorse = true;
                } else if (p == Piece::RedCannon) {
                    redCannon = true;
                }
            } else if (isBlack(p)) {
                ++black;
                if (p == Piece::BlackChariot) {
                    blackChariot = true;
                } else if (p == Piece::BlackHorse) {
                    blackHorse = true;
                } else if (p == Piece::BlackCannon) {
                    blackCannon = true;
                }
            }
        }
    }

    if (red <= 1 && black <= 1) {
        return true;
    }
    if (!redChariot && !redHorse && !redCannon && !blackChariot && !blackHorse && !blackCannon) {
        return true;
    }
    return false;
}

void XiangqiEngine::updateResult()
{
    if (m_result != Result::Ongoing) {
        return;
    }
    const bool inCheck = isInCheck(m_side);
    const bool canMove = hasLegalMove(m_side);

    if (!canMove) {
        if (inCheck) {
            m_result = (m_side == Side::Red) ? Result::BlackWin : Result::RedWin;
        } else {
            m_result = Result::Draw;
        }
        return;
    }

    if (isDrawByRule()) {
        m_result = Result::Draw;
    }
}
