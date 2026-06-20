#include <iostream>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cmath>

#include "./state.hpp"
#include "config.hpp"
#include "../../policy/game_history.hpp"


/*============================================================
 * KP (King-Piece) Evaluation tables
 *
 * Always compiled. Toggled at runtime via use_kp_eval param.
 *============================================================*/

// new_updata
// 子力價值改成題目指定的權重，讓評估與搜尋更容易區分精細局面。
static const int eval_material[7] = {0, 200, 600, 700, 800, 2000, 1000000};

// new_updata
// 5x6 專用 PST：白方直接查表，黑方採 180 度翻轉，讓兩邊共用同一套位置觀念。
static const int eval_pst[6][BOARD_H][BOARD_W] = {
    {
        { -8,  -4,  -2,  -4,  -8 },
        {  6,   8,  10,   8,   6 },
        { 10,  14,  16,  14,  10 },
        { 18,  22,  24,  22,  18 },
        { 32,  36,  40,  36,  32 },
        {  0,   0,   0,   0,   0 },
    },
    {
        { -16,  -8,  -4,  -8, -16 },
        {  -8,   8,  14,   8,  -8 },
        {  -4,  14,  22,  14,  -4 },
        {  -4,  14,  22,  14,  -4 },
        {  -8,   8,  14,   8,  -8 },
        { -16,  -8,  -4,  -8, -16 },
    },
    {
        { -12,  -4,  -2,  -4, -12 },
        {  -4,   8,  10,   8,  -4 },
        {  -2,  12,  14,  12,  -2 },
        {  -2,  12,  14,  12,  -2 },
        {  -4,   8,  10,   8,  -4 },
        { -12,  -4,  -2,  -4, -12 },
    },
    {
        { -6,  -2,   0,  -2,  -6 },
        {  0,   2,   4,   2,   0 },
        {  2,   4,   6,   4,   2 },
        {  2,   4,   6,   4,   2 },
        {  4,   6,   8,   6,   4 },
        { 10,  10,  12,  10,  10 },
    },
    {
        { -8,  -4,  -2,  -4,  -8 },
        { -2,   4,   6,   4,  -2 },
        {  0,   8,  10,   8,   0 },
        {  0,   8,  10,   8,   0 },
        { -2,   4,   6,   4,  -2 },
        { -8,  -4,  -2,  -4,  -8 },
    },
    {
        {  30,  36,  40,  36,  30 },
        {  18,  24,  28,  24,  18 },
        {   6,  10,  14,  10,   6 },
        {  -8,  -4,   0,  -4,  -8 },
        { -22, -18, -12, -18, -22 },
        { -36, -30, -24, -30, -36 },
    },
};

// new_updata
// 機動性僅做微幅修正，避免單純「走步數多」被過度加分。
static inline int mobility_bonus(int legal_count){
    return legal_count * 6;
}

// new_updata
// 黑方做 180 度翻轉後查 PST，讓評估方向一致。
static inline int pst_score_for(int piece_type, int player, int row, int col){
    if(piece_type <= 0 || piece_type > 6){
        return 0;
    }
    if(player == 0){
        return eval_pst[piece_type - 1][row][col];
    }
    return eval_pst[piece_type - 1][BOARD_H - 1 - row][BOARD_W - 1 - col];
}

// new_updata
// 判斷是否為 passed pawn：同一路線前方及左右前方皆無敵方 pawn 阻擋。
static bool is_passed_pawn(const State* state, int player, int row, int col){
    int opp = 1 - player;
    if(state->board.board[player][row][col] != 1){
        return false;
    }
    if(player == 0){
        for(int r = 0; r < row; ++r){
            for(int dc = -1; dc <= 1; ++dc){
                int c = col + dc;
                if(c >= 0 && c < BOARD_W && state->board.board[opp][r][c] == 1){
                    return false;
                }
            }
        }
    } else {
        for(int r = row + 1; r < BOARD_H; ++r){
            for(int dc = -1; dc <= 1; ++dc){
                int c = col + dc;
                if(c >= 0 && c < BOARD_W && state->board.board[opp][r][c] == 1){
                    return false;
                }
            }
        }
    }
    return true;
}

// new_updata
// 取得兵升變距離，越接近底線威脅越大。
static inline int pawn_distance_to_promotion(int player, int row){
    return player == 0 ? row : (BOARD_H - 1 - row);
}


/*============================================================
 * evaluate() — runtime-selectable eval strategy
 *============================================================*/

int State::evaluate(
    bool use_kp_eval,
    bool use_mobility,
    const GameHistory* history
){
    (void)history; // just to suppress warning

    // new_updata
    // 終局直接回傳最大分，讓搜尋優先收斂到最短勝法。
    if(this->game_state == WIN){
        return P_MAX;
    }

    // new_updata
    // 舊版模板評分保留作為註解，避免直接刪除原始框架。
    /*
    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];
    int self_score = 0, oppn_score = 0;
    ...
    return self_score - oppn_score + bonus;
    */

    // new_updata
    // 新版評估：以我方視角累積分數，材料 + PST + 王安全 + 機動性。
    int score = 0;
    int my_king_r = -1, my_king_c = -1;
    int op_king_r = -1, op_king_c = -1;
    int material_diff = 0;

    for(int r = 0; r < BOARD_H; ++r){
        for(int c = 0; c < BOARD_W; ++c){
            int my_piece = this->board.board[this->player][r][c];
            int op_piece = this->board.board[1 - this->player][r][c];

            if(my_piece){
                score += eval_material[my_piece];
                material_diff += eval_material[my_piece];
                score += pst_score_for(my_piece, this->player, r, c);
                if(my_piece == 6){
                    my_king_r = r;
                    my_king_c = c;
                }
            }

            if(op_piece){
                score -= eval_material[op_piece];
                material_diff -= eval_material[op_piece];
                score -= pst_score_for(op_piece, 1 - this->player, r, c);
                if(op_piece == 6){
                    op_king_r = r;
                    op_king_c = c;
                }
            }
        }
    }

    // new_updata
    // 王距離越近，越有利於主動方；這是很輕量的攻王獎勵。
    if(my_king_r != -1 && op_king_r != -1){
        int king_dist = std::max(std::abs(my_king_r - op_king_r), std::abs(my_king_c - op_king_c));
        score += (3 - std::min(3, king_dist)) * 12;
    }

    // new_updata
    // 若開啟 mobility，依目前合法步數給微幅加成，輔助搜索偏向更活躍的局面。
    if(use_mobility){
        score += mobility_bonus(static_cast<int>(this->legal_actions.size()));
    }

    // new_updata
    // 檢查雙方 Passed Pawn，敵方威脅給予巨額懲罰，我方 Passed Pawn 給予前推紅利。
    int minor_material = 0;
    for(int r = 0; r < BOARD_H; ++r){
        for(int c = 0; c < BOARD_W; ++c){
            int my_piece = this->board.board[this->player][r][c];
            int opp_piece = this->board.board[1 - this->player][r][c];
            if(my_piece > 1 && my_piece != 6){ // new_updata: 殘局逼殺只計算大子，不含兵
                minor_material += 1;
            }
            if(opp_piece > 1 && opp_piece != 6){ // new_updata: 殘局逼殺只計算大子，不含兵
                minor_material += 1;
            }

            if(my_piece == 1 && is_passed_pawn(this, this->player, r, c)){
                int dist = pawn_distance_to_promotion(this->player, r);
                score += 180 * (BOARD_H - dist);
            }
            if(opp_piece == 1 && is_passed_pawn(this, 1 - this->player, r, c)){
                int dist = pawn_distance_to_promotion(1 - this->player, r);
                int penalty = 0;
                if(dist <= 1){
                    penalty = 1800;
                } else if(dist == 2){
                    penalty = 900;
                }
                // If we're evaluating from Black's perspective, increase penalty
                // for opponent (White) passed pawns to prioritize blocking/defense.
                if(this->player == 1){
                    penalty = (penalty * 3) / 2; // +50%
                }
                score -= penalty;
            }
        }
    }

    // Extra safety consideration for Black (player==1):
    // Penalize positions where many White pieces are close to Black king.
    if(this->player == 1 && my_king_r != -1 && my_king_c != -1){
        int danger = 0;
        for(int r = 0; r < BOARD_H; ++r){
            for(int c = 0; c < BOARD_W; ++c){
                int opp_piece = this->board.board[0][r][c]; // White pieces
                if(opp_piece){
                    int dist = std::abs(r - my_king_r) + std::abs(c - my_king_c);
                    if(dist <= 2){
                        // closer and heavier pieces increase danger more
                        int weight = 0;
                        switch(opp_piece){
                            case 1: weight = 1; break; // pawn
                            case 2: weight = 4; break; // rook
                            case 3: weight = 3; break; // knight
                            case 4: weight = 3; break; // bishop
                            case 5: weight = 6; break; // queen
                            default: weight = 2; break;
                        }
                        danger += (3 - dist) * weight; // dist 0..2 -> multiplier 3,2,1
                    }
                }
            }
        }
        // Apply a scaled penalty to the evaluation to make Black prefer defensive moves.
        score -= danger * 60;
    }

    // new_updata
    // 殘局逼殺邏輯：大子稀少時，國王距離越近越好，敵王偏離中心越多越好。
    if(minor_material <= 4 && my_king_r != -1 && op_king_r != -1){
        int king_manhattan = std::abs(my_king_r - op_king_r) + std::abs(my_king_c - op_king_c);
        int bonus_scale = 18 + std::clamp(material_diff / 100, -8, 18);
        score += (12 - king_manhattan) * bonus_scale;

        constexpr int center_r = BOARD_H / 2;
        constexpr int center_c = BOARD_W / 2;
        int opp_center_dist = std::abs(op_king_r - center_r) + std::abs(op_king_c - center_c);
        score += opp_center_dist * (26 + std::clamp(material_diff / 160, -6, 12));
    }

    // new_updata: step penalty encourages faster resolution while avoiding over-aggressive move bias.
    score -= this->step;

    // new_updata
    // use_kp_eval 目前保留參數語意，但評分已統一走新版高分估值。
    (void)use_kp_eval;

    return score;
}



/*============================================================
 * Zobrist hash for transposition table
 *============================================================*/
static uint64_t zobrist_piece[2][7][BOARD_H][BOARD_W];
static uint64_t zobrist_side;
static bool zobrist_ready = false;

static void init_zobrist(){
    uint64_t s = 0x7A35C9D1E4F02B68ULL;
    auto rand64 = [&s]() -> uint64_t {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    };
    for(int p = 0; p < 2; p++){
        for(int t = 0; t < 7; t++){
            for(int r = 0; r < BOARD_H; r++){
                for(int c = 0; c < BOARD_W; c++){
                    zobrist_piece[p][t][r][c] = rand64();
                }
            }
        }
    }
    zobrist_side = rand64();
    zobrist_ready = true;
}

uint64_t State::compute_hash_full() const{
    if(!zobrist_ready){
        init_zobrist();
    }
    uint64_t h = 0;
    for(int p = 0; p < 2; p++){
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int piece = this->board.board[p][r][c];
                if(piece){
                    h ^= zobrist_piece[p][piece][r][c];
                }
            }
        }
    }
    if(this->player){
        h ^= zobrist_side;
    }
    return h;
}


/**
 * @brief return next state after the move
 *
 * @param move
 * @return State*
 */
State* State::next_state(const Move& move){
    if(!zobrist_ready){ init_zobrist(); }

    Board next = this->board;
    Point from = move.first, to = move.second;
    int p = this->player;
    int opp = 1 - p;

    int8_t orig_piece = next.board[p][from.first][from.second];
    int8_t moved = orig_piece;
    //promotion for pawn
    if(moved == 1 && (to.first==BOARD_H-1 || to.first==0)){
        moved = 5;
    }

    /* Incremental hash update */
    uint64_t h = this->hash();
    h ^= zobrist_side;  /* toggle side to move */

    /* XOR out piece from source */
    h ^= zobrist_piece[p][orig_piece][from.first][from.second];

    /* XOR out captured piece at destination */
    int8_t captured = next.board[opp][to.first][to.second];
    if(captured){
        h ^= zobrist_piece[opp][captured][to.first][to.second];
        next.board[opp][to.first][to.second] = 0;
    }

    /* XOR in piece at destination */
    h ^= zobrist_piece[p][moved][to.first][to.second];

    next.board[p][from.first][from.second] = 0;
    next.board[p][to.first][to.second] = moved;

    State* ns = new State(next, opp);
    ns->step = this->step + 1;
    ns->zobrist_hash = h;
    ns->zobrist_valid = true;
    return ns;
}


static const int move_table_rook_bishop[8][7][2] = {
  {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7}},
  {{0, -1}, {0, -2}, {0, -3}, {0, -4}, {0, -5}, {0, -6}, {0, -7}},
  {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}},
  {{-1, 0}, {-2, 0}, {-3, 0}, {-4, 0}, {-5, 0}, {-6, 0}, {-7, 0}},
  {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}},
  {{1, -1}, {2, -2}, {3, -3}, {4, -4}, {5, -5}, {6, -6}, {7, -7}},
  {{-1, 1}, {-2, 2}, {-3, 3}, {-4, 4}, {-5, 5}, {-6, 6}, {-7, 7}},
  {{-1, -1}, {-2, -2}, {-3, -3}, {-4, -4}, {-5, -5}, {-6, -6}, {-7, -7}},
};

// [ Hackathon TODO 2-1 ]
// fill the knight move table
static const int move_table_knight[8][2] = {
    {2, 1}, {2, -1}, {-2, 1}, {-2, -1},
    {1, 2}, {1, -2}, {-1, 2}, {-1, -2}
};
//
static const int move_table_king[8][2] = {
  {1, 0}, {0, 1}, {-1, 0}, {0, -1}, 
  {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
};


/*============================================================
 * Naive move generation (array-based, branch-heavy)
 *============================================================*/
void State::get_legal_actions_naive(){
    this->game_state = NONE;
    std::vector<Move> all_actions;
    all_actions.reserve(64);
    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];

    int now_piece, oppn_piece;
    for(int i=0; i<BOARD_H; i+=1){
        for(int j=0; j<BOARD_W; j+=1){
            if((now_piece=self_board[i][j])){
                switch(now_piece){
                    case 1: //pawn
                        if(this->player && i<BOARD_H-1){
                            //black
                            if(!oppn_board[i+1][j] && !self_board[i+1][j]){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j)));
                            }
                            if(j<BOARD_W-1 && (oppn_piece=oppn_board[i+1][j+1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j+1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if(j>0 && (oppn_piece=oppn_board[i+1][j-1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j-1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }else if(!this->player && i>0){
                            //white
                            if(!oppn_board[i-1][j] && !self_board[i-1][j]){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j)));
                            }
                            if(j<BOARD_W-1 && (oppn_piece=oppn_board[i-1][j+1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j+1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if(j>0 && (oppn_piece=oppn_board[i-1][j-1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j-1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }
                        break;

                    case 2: //rook
                    case 4: //bishop
                    case 5: //queen
                        int st, end;
                        switch(now_piece){
                            case 2: st=0; end=4; break; //rook
                            case 4: st=4; end=8; break; //bishop
                            case 5: st=0; end=8; break; //queen
                            default: st=0; end=-1;
                        }
                        for(int part=st; part<end; part+=1){
                            auto move_list = move_table_rook_bishop[part];
                            for(int k=0; k<std::max(BOARD_H, BOARD_W); k+=1){
                                int p[2] = {move_list[k][0] + i, move_list[k][1] + j};

                                if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                    break;
                                }
                                now_piece = self_board[p[0]][p[1]];
                                if(now_piece){
                                    break;
                                }

                                all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                                oppn_piece = oppn_board[p[0]][p[1]];
                                if(oppn_piece){
                                    if(oppn_piece==6){
                                        this->game_state = WIN;
                                        this->legal_actions = all_actions;
                                        return;
                                    }else{
                                        break;
                                    }
                                };
                            }
                        }
                        break;

                    case 3: //knight
                        // [ Hackathon TODO 2-2 ]
                        // complete knight's movement, you can refer to other pieces' movement
                        for(auto move: move_table_knight){
                            // 計算目標座標
                            int p[2] = {move[0] + i, move[1] + j};

                            // 檢查是否超出棋盤邊界
                            if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                continue;
                            }
                            
                            // 檢查目標位置是否被自己的棋子擋住
                            now_piece = self_board[p[0]][p[1]];
                            if(now_piece){
                                continue;
                            }

                            // 加入合法步清單
                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            // 檢查是否吃掉對方的國王
                            oppn_piece = oppn_board[p[0]][p[1]];
                            if(oppn_piece == 6){
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;
                        //

                    case 6: //king
                        for(auto move: move_table_king){
                            int p[2] = {move[0] + i, move[1] + j};

                            if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                continue;
                            }
                            now_piece = self_board[p[0]][p[1]];
                            if(now_piece){
                                continue;
                            }

                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            oppn_piece = oppn_board[p[0]][p[1]];
                            if(oppn_piece==6){
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;
                }
            }
        }
    }
    this->legal_actions = all_actions;
}


/*============================================================
 * Bitboard move generation
 *
 * 6x5 = 30 squares fit in a uint32_t.
 * Square (r,c) -> bit index r*5+c.
 * Precomputed attack masks for leapers (knight, king, pawn).
 * Bit-scan loop (__builtin_ctz) replaces nested array iteration.
 *============================================================*/
#define BB_SQ(r, c)  ((r) * BOARD_W + (c))
#define BB_ROW(sq)   ((sq) / BOARD_W)
#define BB_COL(sq)   ((sq) % BOARD_W)

// Precomputed attack tables (initialized once)
static uint32_t bb_knight[30];       // knight attack mask per square
static uint32_t bb_king[30];         // king attack mask per square
static uint32_t bb_pawn_push[2][30]; // pawn push target per player/square
static uint32_t bb_pawn_cap[2][30];  // pawn capture targets per player/square
static bool bb_ready = false;

// Sliding piece direction vectors (0-3: rook, 4-7: bishop, 0-7: queen)
static const int bb_dr[8] = {0, 0, 1, -1, 1, 1, -1, -1};
static const int bb_dc[8] = {1, -1, 0, 0, 1, -1, 1, -1};

static void bb_init(){
    static const int kn_dr[8] = {1, 1, -1, -1, 2, 2, -2, -2};
    static const int kn_dc[8] = {2, -2, 2, -2, 1, -1, 1, -1};
    static const int ki_dr[8] = {1, 0, -1, 0, 1, 1, -1, -1};
    static const int ki_dc[8] = {0, 1, 0, -1, 1, -1, 1, -1};

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int sq = BB_SQ(r, c);

            // Knight
            bb_knight[sq] = 0;
            for(int d = 0; d < 8; d++){
                int nr = r + kn_dr[d], nc = c + kn_dc[d];
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    bb_knight[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            // King
            bb_king[sq] = 0;
            for(int d = 0; d < 8; d++){
                int nr = r + ki_dr[d], nc = c + ki_dc[d];
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    bb_king[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            // Pawn (player 0 = white, advances up = row-1)
            bb_pawn_push[0][sq] = 0;
            bb_pawn_cap[0][sq] = 0;
            if(r > 0){
                bb_pawn_push[0][sq] = 1u << BB_SQ(r-1, c);
                if(c > 0){
                    bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c-1);
                }
                if(c < BOARD_W-1){
                    bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c+1);
                }
            }

            // Pawn (player 1 = black, advances down = row+1)
            bb_pawn_push[1][sq] = 0;
            bb_pawn_cap[1][sq] = 0;
            if(r < BOARD_H-1){
                bb_pawn_push[1][sq] = 1u << BB_SQ(r+1, c);
                if(c > 0){
                    bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c-1);
                }
                if(c < BOARD_W-1){
                    bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c+1);
                }
            }
        }
    }
    bb_ready = true;
}

void State::get_legal_actions_bitboard(){
    if(!bb_ready){
        bb_init();
    }

    this->game_state = NONE;
    this->legal_actions.clear();
    this->legal_actions.reserve(64);

    int self = this->player;
    int oppn = 1 - self;

    // Build occupancy bitmasks and piece-type lookup
    uint32_t self_occ = 0, oppn_occ = 0;
    int self_pt[30] = {};  // piece type at each square (self)
    int oppn_pt[30] = {};  // piece type at each square (opponent)

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int sq = BB_SQ(r, c);
            if(this->board.board[self][r][c]){
                self_occ |= 1u << sq;
                self_pt[sq] = this->board.board[self][r][c];
            }
            if(this->board.board[oppn][r][c]){
                oppn_occ |= 1u << sq;
                oppn_pt[sq] = this->board.board[oppn][r][c];
            }
        }
    }

    uint32_t all_occ = self_occ | oppn_occ;

    // Iterate own pieces via bit scan
    uint32_t pieces = self_occ;
    while(pieces){
        int sq = __builtin_ctz(pieces);
        pieces &= pieces - 1;
        int r = BB_ROW(sq), c = BB_COL(sq);
        int piece = self_pt[sq];
        uint32_t targets = 0;

        switch(piece){
            case 1: { // Pawn
                uint32_t push = bb_pawn_push[self][sq] & ~all_occ;
                uint32_t cap = bb_pawn_cap[self][sq] & oppn_occ;
                // Check for king capture in captures
                uint32_t cap_scan = cap;
                while(cap_scan){
                    int to = __builtin_ctz(cap_scan);
                    cap_scan &= cap_scan - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                targets = push | cap;
                break;
            }

            case 3: { // Knight
                targets = bb_knight[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while(opp_targets){
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 6: { // King
                targets = bb_king[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while(opp_targets){
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 2: // Rook
            case 4: // Bishop
            case 5: { // Queen
                int d_start = (piece == 4) ? 4 : 0;
                int d_end   = (piece == 2) ? 4 : 8;
                for(int d = d_start; d < d_end; d++){
                    int cr = r + bb_dr[d], cc = c + bb_dc[d];
                    while(cr >= 0 && cr < BOARD_H && cc >= 0 && cc < BOARD_W){
                        int to = BB_SQ(cr, cc);
                        uint32_t to_bit = 1u << to;
                        if(self_occ & to_bit){
                            break; // own piece blocks
                        }

                        if((oppn_occ & to_bit) && oppn_pt[to] == 6){
                            this->game_state = WIN;
                            this->legal_actions.push_back(
                                Move(Point(r, c), Point(cr, cc)));
                            return;
                        }

                        targets |= to_bit;
                        if(oppn_occ & to_bit){
                            break; // captured, stop sliding
                        }
                        cr += bb_dr[d]; cc += bb_dc[d];
                    }
                }
                break;
            }
        }

        // Convert target bitmask to Move objects
        while(targets){
            int to = __builtin_ctz(targets);
            targets &= targets - 1;
            this->legal_actions.push_back(
                Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
        }
    }
}


/*============================================================
 * Dispatcher
 *============================================================*/
void State::get_legal_actions(){
    #ifdef USE_BITBOARD
    get_legal_actions_bitboard();
    #else
    get_legal_actions_naive();
    #endif
}


const char piece_table[2][7][5] = {
  {" ", "♙", "♖", "♘", "♗", "♕", "♔"},
  {" ", "♟", "♜", "♞", "♝", "♛", "♚"}
};
/**
 * @brief encode the output for command line output
 * 
 * @return std::string 
 */
std::string State::encode_output() const{
    std::stringstream ss;
    int now_piece;
    for(int i=0; i<BOARD_H; i+=1){
        for(int j=0; j<BOARD_W; j+=1){
            if((now_piece = this->board.board[0][i][j])){
                ss << std::string(piece_table[0][now_piece]);
            }else if((now_piece = this->board.board[1][i][j])){
                ss << std::string(piece_table[1][now_piece]);
            }else{
                ss << " ";
            }
            ss << " ";
        }
        ss << "\n";
    }
    return ss.str();
}


/**
 * @brief encode the state to the format for player
 * 
 * @return std::string 
 */
std::string State::encode_state(){
    std::stringstream ss;
    ss << this->player;
    ss << "\n";
    for(int pl=0; pl<2; pl+=1){
        for(int i=0; i<BOARD_H; i+=1){
            for(int j=0; j<BOARD_W; j+=1){
                ss << int(this->board.board[pl][i][j]);
                ss << " ";
            }
            ss << "\n";
        }
        ss << "\n";
    }
    return ss.str();
}


BaseState* State::create_null_state() const{
    State* s = new State(this->board, 1 - this->player);
    s->step = this->step + 1;
    s->zobrist_valid = false;
    s->game_state = this->game_state;
    s->get_legal_actions();
    return s;
}


/* === Board serialization === */
static const char* piece_chars = ".PRNBQK";
static const char* piece_chars_lower = ".prnbqk";

std::string State::encode_board() const{
    std::string s;
    for(int r = 0; r < BOARD_H; r++){
        if(r > 0){
            s += '/';
        }
        for(int c = 0; c < BOARD_W; c++){
            int w = board.board[0][r][c];
            int b = board.board[1][r][c];
            if(w > 0 && w <= 6){
                s += piece_chars[w];
            }else if(b > 0 && b <= 6){
                s += piece_chars_lower[b];
            }else{
                s += '.';
            }
        }
    }
    return s;
}

void State::decode_board(const std::string& s, int side_to_move){
    player = side_to_move;
    game_state = UNKNOWN;
    zobrist_valid = false;
    board = Board{};
    int r = 0, c = 0;
    for(char ch : s){
        if(ch == '/'){
            r++;
            c = 0;
            continue;
        }
        if(r >= BOARD_H || c >= BOARD_W){
            break;
        }
        if(ch >= 'A' && ch <= 'Z'){
            for(int p = 1; p <= 6; p++){
                if(piece_chars[p] == ch){
                    board.board[0][r][c] = p;
                    break;
                }
            }
        }else if(ch >= 'a' && ch <= 'z'){
            for(int p = 1; p <= 6; p++){
                if(piece_chars_lower[p] == ch){
                    board.board[1][r][c] = p;
                    break;
                }
            }
        }
        c++;
    }
    get_legal_actions();
}


/* (Zobrist tables moved above next_state) */


/*============================================================
 * Cell display for protocol (d command)
 *============================================================*/
std::string State::cell_display(int row, int col) const{
    int w = static_cast<int>(board.board[0][row][col]);
    int b = static_cast<int>(board.board[1][row][col]);
    if(w){
        const char* names = ".PRNBQK";
        return std::string(" ") + names[w] + " ";
    }else if(b){
        const char* names = ".prnbqk";
        return std::string(" ") + names[b] + " ";
    }else{
        return " . ";
    }
}

/* === Repetition: chess 3-fold rule === */
bool State::check_repetition(const GameHistory& history, int& out_score) const {
    if(history.count(hash()) >= 2){
        out_score = 0;  /* draw */
        return true;
    }
    return false;
}
