#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include "state.hpp"
#include "minimax.hpp"

namespace {
    // new_updata
    // 置換表大小：1 << 23，約 8.3M entries。
    static constexpr uint32_t TT_SIZE = 1u << 23;

    enum TTFlag : uint8_t {
        TT_EXACT = 0,
        TT_LOWER = 1,
        TT_UPPER = 2,
    };

    struct TTEntry {
        uint64_t key = 0;
        int score = 0;
        int depth = 0;
        uint32_t move = 0;
        uint8_t flag = 0;
    };

    static TTEntry* g_tt_table = nullptr;
    static bool g_tt_ready = false;

    static uint32_t g_killers[128][2] = {};
    static int g_history[2][BOARD_H * BOARD_W][BOARD_H * BOARD_W] = {};

    static inline int square_index(const Point& p){
        return static_cast<int>(p.first) * BOARD_W + static_cast<int>(p.second);
    }

    static inline uint32_t encode_move(const Move& m){
        return (static_cast<uint32_t>(square_index(m.first)) << 8) |
               static_cast<uint32_t>(square_index(m.second));
    }

    static inline Move decode_move(uint32_t code){
        int from = static_cast<int>(code >> 8);
        int to = static_cast<int>(code & 0xFFu);
        return Move(Point(from / BOARD_W, from % BOARD_W), Point(to / BOARD_W, to % BOARD_W));
    }

    static inline bool same_encoded_move(uint32_t a, const Move& m){
        return a == encode_move(m);
    }

    static void ensure_tt(){
        if(!g_tt_ready){
            g_tt_table = new TTEntry[TT_SIZE]();
            g_tt_ready = true;
        }
    }

    static inline uint32_t tt_index(uint64_t key){
        return static_cast<uint32_t>(key & (TT_SIZE - 1));
    }

    static inline const TTEntry* tt_probe(uint64_t key){
        ensure_tt();
        return &g_tt_table[tt_index(key)];
    }

    static int pvs(State* state, int depth, int alpha, int beta, GameHistory& history, SearchContext& ctx, const MMParams& p, int ply);
    static inline int pvs_negamax(State* next, bool same, int depth, int alpha, int beta, GameHistory& history, SearchContext& ctx, const MMParams& p, int ply){
        if(same){
            return pvs(next, depth, alpha, beta, history, ctx, p, ply);
        }
        return -pvs(next, depth, -beta, -alpha, history, ctx, p, ply);
    }

    static inline void tt_store(uint64_t key, int depth, int score, uint32_t move, uint8_t flag){
        ensure_tt();
        uint32_t idx = tt_index(key);
        TTEntry& entry = g_tt_table[idx];
        if(entry.key != key || entry.depth <= depth){
            entry.key = key;
            entry.depth = depth;
            entry.score = score;
            entry.move = move;
            entry.flag = flag;
        }
    }

    static inline int tt_decode_score(int score, int ply){
        // new_updata: 修正 TT 絕殺錯亂！讀取時把 ply 扣回來
        if(score >= P_MAX - 1000){
            return score - ply;
        }
        if(score <= M_MAX + 1000){
            return score + ply;
        }
        return score;
    }

    static inline int tt_encode_score(int score, int ply){
        // new_updata: 修正 TT 絕殺錯亂！存入 TT 前必須獨立化 ply 距離
        if(score >= P_MAX - 1000){
            return score + ply;
        }
        if(score <= M_MAX + 1000){
            return score - ply;
        }
        return score;
    }

    static inline bool is_capture_move(const State* state, const Move& action){
        int opp = 1 - state->player;
        return state->board.board[opp][action.second.first][action.second.second] != 0;
    }

    static inline bool is_promotion_move(const State* state, const Move& action){
        int piece = state->board.board[state->player][action.first.first][action.first.second];
        return piece == 1 && (action.second.first == 0 || action.second.first == BOARD_H - 1);
    }

    static inline bool is_tactical_move(const State* state, const Move& action){
        return is_capture_move(state, action) || is_promotion_move(state, action);
    }

    static inline bool is_within(int r, int c){
        return r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W;
    }

    static bool is_in_check(const State* state){
        int player = state->player;
        int opp = 1 - player;
        int king_r = -1, king_c = -1;

        for(int r = 0; r < BOARD_H; ++r){
            for(int c = 0; c < BOARD_W; ++c){
                if(state->board.board[player][r][c] == 6){
                    king_r = r;
                    king_c = c;
                    break;
                }
            }
            if(king_r != -1){
                break;
            }
        }

        if(king_r == -1){
            return false;
        }

        // Pawn attacks.
        if(player == 0){
            if(king_r > 0){
                if(king_c > 0 && state->board.board[opp][king_r - 1][king_c - 1] == 1){
                    return true;
                }
                if(king_c < BOARD_W - 1 && state->board.board[opp][king_r - 1][king_c + 1] == 1){
                    return true;
                }
            }
        } else {
            if(king_r < BOARD_H - 1){
                if(king_c > 0 && state->board.board[opp][king_r + 1][king_c - 1] == 1){
                    return true;
                }
                if(king_c < BOARD_W - 1 && state->board.board[opp][king_r + 1][king_c + 1] == 1){
                    return true;
                }
            }
        }

        // Knight attacks.
        static const int knight_offsets[8][2] = {
            {2, 1}, {2, -1}, {-2, 1}, {-2, -1}, {1, 2}, {1, -2}, {-1, 2}, {-1, -2}
        };
        for(auto& off : knight_offsets){
            int r = king_r + off[0];
            int c = king_c + off[1];
            if(is_within(r, c) && state->board.board[opp][r][c] == 3){
                return true;
            }
        }

        // King adjacency.
        for(int dr = -1; dr <= 1; ++dr){
            for(int dc = -1; dc <= 1; ++dc){
                if(dr == 0 && dc == 0){
                    continue;
                }
                int r = king_r + dr;
                int c = king_c + dc;
                if(is_within(r, c) && state->board.board[opp][r][c] == 6){
                    return true;
                }
            }
        }

        // Sliding attacks.
        static const int dr[8] = {0, 0, 1, -1, 1, 1, -1, -1};
        static const int dc[8] = {1, -1, 0, 0, 1, -1, 1, -1};

        for(int idx = 0; idx < 8; ++idx){
            int r = king_r + dr[idx];
            int c = king_c + dc[idx];
            while(is_within(r, c)){
                int piece = state->board.board[player][r][c];
                if(piece){
                    break;
                }
                int opp_piece = state->board.board[opp][r][c];
                if(opp_piece){
                    if(idx < 4){
                        if(opp_piece == 2 || opp_piece == 5){
                            return true;
                        }
                    } else {
                        if(opp_piece == 4 || opp_piece == 5){
                            return true;
                        }
                    }
                    break;
                }
                r += dr[idx];
                c += dc[idx];
            }
        }

        return false;
    }

    static inline bool is_passed_pawn(const State* state, int player, int row, int col){
        int opp = 1 - player;
        if(player == 0){
            for(int r = 0; r < row; ++r){
                for(int dc = -1; dc <= 1; ++dc){
                    int c = col + dc;
                    if(is_within(r, c) && state->board.board[opp][r][c] == 1){
                        return false;
                    }
                }
            }
        } else {
            for(int r = row + 1; r < BOARD_H; ++r){
                for(int dc = -1; dc <= 1; ++dc){
                    int c = col + dc;
                    if(is_within(r, c) && state->board.board[opp][r][c] == 1){
                        return false;
                    }
                }
            }
        }
        return true;
    }

    static inline int get_pawn_promotion_distance(int player, int row){
        return player == 0 ? row : (BOARD_H - 1 - row);
    }

    static inline int mvv_lva(const State* state, const Move& action){
        int opp = 1 - state->player;
        int victim = state->board.board[opp][action.second.first][action.second.second];
        int attacker = state->board.board[state->player][action.first.first][action.first.second];
        return victim * 16 - attacker;
    }

    static void order_moves(State* state, std::vector<Move>& moves, uint32_t tt_move, int ply){
        int count = static_cast<int>(moves.size());
        int scores[64];

        for(int i = 0; i < count; ++i){
            const Move& move = moves[i];
            int score = 0;
            if(tt_move && same_encoded_move(tt_move, move)){
                score = 1000000;
            } else if(is_capture_move(state, move)){
                score = 900000 + mvv_lva(state, move);
            } else if(state->board.board[state->player][move.first.first][move.first.second] == 1
                      && get_pawn_promotion_distance(state->player, move.second.first)
                         < get_pawn_promotion_distance(state->player, move.first.first)){
                int dist = get_pawn_promotion_distance(state->player, move.second.first);
                score = 450000 + (3 - std::min(dist, 3)) * 12000;
            } else if(ply >= 0 && ply < 128 && same_encoded_move(g_killers[ply][0], move)){
                score = 500000;
            } else if(ply >= 0 && ply < 128 && same_encoded_move(g_killers[ply][1], move)){
                score = 400000;
            } else {
                int from_sq = square_index(move.first);
                int to_sq = square_index(move.second);
                score = g_history[state->player][from_sq][to_sq];
            }
            scores[i] = score;
        }

        for(int i = 1; i < count; ++i){
            int key_score = scores[i];
            Move key_move = moves[i];
            int j = i - 1;
            while(j >= 0 && scores[j] < key_score){
                scores[j + 1] = scores[j];
                moves[j + 1] = moves[j];
                --j;
            }
            scores[j + 1] = key_score;
            moves[j + 1] = key_move;
        }
    }

    static inline bool is_non_endgame(const State* state){
        int material = 0;
        for(int p = 0; p < 2; ++p){
            for(int r = 0; r < BOARD_H; ++r){
                for(int c = 0; c < BOARD_W; ++c){
                    int piece = state->board.board[p][r][c];
                    if(piece && piece != 6){
                        material += 1;
                    }
                }
            }
        }
        // Require more material before applying aggressive null-move / LMR pruning.
        return material > 12;
    }

    static int quiescence(State* state, int alpha, int beta, GameHistory& history, SearchContext& ctx, const MMParams& p, int ply){
        // 進入 QS 節點：只探索戰術性走法以避免“評估震盪”。
        ctx.nodes++;
        if(ctx.stop){
            return 0;
        }

        if(state->legal_actions.empty()){
            state->get_legal_actions();
        }

        // 如果已經是勝負或和棋，直接返回終局分數。
        if(state->game_state == WIN){
            return P_MAX - ply;
        }
        if(state->game_state == DRAW){
            return 0;
        }

        int rep_score;
        if(state->check_repetition(history, rep_score)){
            return rep_score;
        }

        // 站立評估：不先走棋的靜態評估。
        int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        int best_score = std::numeric_limits<int>::min() / 4;

        best_score = stand_pat;
        if(stand_pat >= beta){
            // Beta 截斷：靜態評估已經足夠好，無需深入搜尋。
            return best_score;
        }
        if(alpha < stand_pat){
            alpha = stand_pat;
        }

        // 只考慮可引發捕獲或晉升的戰術性走法。
        for(auto& action : state->legal_actions){
            if(!is_tactical_move(state, action)){
                continue;
            }

            State* next = state->next_state(action);
            int score = -quiescence(next, -beta, -alpha, history, ctx, p, ply + 1);
            delete next;

            if(ctx.stop){
                return 0;
            }

            if(score > best_score){
                best_score = score;
            }
            if(score > alpha){
                alpha = score;
            }
            if(best_score >= beta){
                // Beta 截斷：找到足夠好的戰術走法。
                return best_score;
            }
        }

        return best_score;
    }

    static int pvs(State* state, int depth, int alpha, int beta, GameHistory& history, SearchContext& ctx, const MMParams& p, int ply){
        ctx.nodes++;
        if(ply > ctx.seldepth){
            ctx.seldepth = ply;
        }
        if(ctx.stop){
            return 0;
        }

        if(state->legal_actions.empty()){
            state->get_legal_actions();
        }

        if(state->game_state == WIN){
            return P_MAX - ply;
        }
        if(state->game_state == DRAW){
            return 0;
        }

        int rep_score;
        if(state->check_repetition(history, rep_score)){
            return rep_score;
        }

        history.push(state->hash());
        uint64_t hash_key = state->hash();

        uint32_t tt_move = 0;
        const TTEntry* entry = tt_probe(hash_key);
        if(entry->key == hash_key){
            tt_move = entry->move;
            if(entry->depth >= depth){
                int cached_score = tt_decode_score(entry->score, ply);
                if(entry->flag == TT_EXACT){
                    history.pop(state->hash());
                    return cached_score;
                }
                if(entry->flag == TT_LOWER && cached_score >= beta){
                    history.pop(state->hash());
                    return cached_score;
                }
                if(entry->flag == TT_UPPER && cached_score <= alpha){
                    history.pop(state->hash());
                    return cached_score;
                }
            }
        }

        uint32_t current_best_move = tt_move;

        if(depth <= 0){
            int score = quiescence(state, alpha, beta, history, ctx, p, ply);
            history.pop(state->hash());
            return score;
        }

        if(depth <= 2){
            int stand = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
            if(stand - depth * 150 >= beta){
                history.pop(state->hash());
                return stand;
            }
        }

        bool do_null = depth >= 8 && is_non_endgame(state) && !is_in_check(state) && state->legal_actions.size() > 2;
        if(do_null){
            BaseState* null_base = state->create_null_state();
            if(null_base){
                State* null_state = static_cast<State*>(null_base);
                history.push(null_state->hash());
                int score = -pvs(null_state, depth - 2, -beta, -beta + 1, history, ctx, p, ply + 1);
                history.pop(null_state->hash());
                delete null_state;
                if(ctx.stop){
                    history.pop(state->hash());
                    return 0;
                }
                if(score >= beta){
                    history.pop(state->hash());
                    return score;
                }
            }
        }

        order_moves(state, state->legal_actions, tt_move, ply);

        int best_score = M_MAX;
        int alpha_orig = alpha;
        bool first_child = true;
        int move_count = 0;

        for(auto& action : state->legal_actions){
            if(ctx.stop){
                break;
            }

            State* next = state->next_state(action);
            bool same = next->same_player_as_parent();
            int next_depth = same ? depth : depth - 1;
            int score;

            if(first_child){
                score = pvs_negamax(next, same, next_depth, alpha, beta, history, ctx, p, ply + 1);
            } else {
                int reduction = 0;
                if(!is_tactical_move(state, action) && move_count >= 8 && next_depth > 7 && is_non_endgame(state)){
                    reduction = 1;
                }

                if(reduction > 0){
                    int scout_score = pvs_negamax(next, same, reduction, alpha, alpha + 1, history, ctx, p, ply + 1);
                    if(ctx.stop){
                        delete next;
                        break;
                    }
                    if(scout_score > alpha){
                        score = pvs_negamax(next, same, next_depth, alpha, beta, history, ctx, p, ply + 1);
                    } else {
                        score = scout_score;
                    }
                } else {
                    score = pvs_negamax(next, same, next_depth, alpha, alpha + 1, history, ctx, p, ply + 1);
                    if(score > alpha && score < beta){
                        score = pvs_negamax(next, same, next_depth, alpha, beta, history, ctx, p, ply + 1);
                    }
                }
            }

            delete next;

            if(ctx.stop){
                break;
            }

            if(score > best_score){
                best_score = score;
                current_best_move = encode_move(action); // new_updata: 修正致命錯誤，記錄當前最佳步
            }
            if(score > alpha){
                alpha = score;
            }

            if(score >= beta){
                if(!is_tactical_move(state, action) && !same_encoded_move(tt_move, action)){
                    uint32_t code = encode_move(action);
                    if(ply >= 0 && ply < 128){ // new_updata: 修正致命錯誤，避免 Killer Move 陣列越界
                        if(!same_encoded_move(g_killers[ply][0], action)){
                            g_killers[ply][1] = g_killers[ply][0];
                            g_killers[ply][0] = code;
                        }
                    }
                    int from_sq = square_index(action.first);
                    int to_sq = square_index(action.second);
                    g_history[state->player][from_sq][to_sq] += depth * depth;
                }

                int store_score = tt_encode_score(best_score, ply);
                tt_store(hash_key, depth, store_score, encode_move(action), TT_LOWER);
                history.pop(state->hash());
                return best_score;
            }

            first_child = false;
            ++move_count;
        }

        uint8_t flag = TT_EXACT;
        if(best_score <= alpha_orig){
            flag = TT_UPPER;
        } else if(best_score >= beta){
            flag = TT_LOWER;
        }
        uint32_t best_move = current_best_move;
        if(best_move == 0 && !state->legal_actions.empty()){
            best_move = encode_move(state->legal_actions[0]);
        }
        int store_score = tt_encode_score(best_score, ply);
        tt_store(hash_key, depth, store_score, best_move, flag);

        history.pop(state->hash());
        return best_score;
    }

    static SearchResult search_root(State* state, int depth, GameHistory& history, SearchContext& ctx, const MMParams& p, int alpha, int beta){
        // 根節點搜尋：使用固定 alpha/beta 或 aspiration window 進行主線搜索。
        SearchResult result;
        result.depth = depth;
        result.score = M_MAX;

        if(state->legal_actions.empty()){
            state->get_legal_actions();
        }

        history.push(state->hash());
        uint32_t tt_move = 0;
        uint64_t hash_key = state->hash();
        const TTEntry* entry = tt_probe(hash_key);
        if(entry->key == hash_key){
            tt_move = entry->move;
        }
        order_moves(state, state->legal_actions, tt_move, 0);

        // 根節點採用完整搜索，每個子節點都會進入 PVS。
        for(auto& action : state->legal_actions){
            if(ctx.stop){
                break;
            }

            State* next = state->next_state(action);
            bool repeat_risk = history.count(next->hash()) >= 1;
            int next_depth = next->same_player_as_parent() ? depth : depth - 1;
            int score = -pvs(next, next_depth, -beta, -alpha, history, ctx, p, 1);
            delete next;

            if(ctx.stop){
                break;
            }

            if(repeat_risk && std::abs(score) < 2000){
                score -= 1200; // 風險重複的根走法會被懲罰，以避免在非明顯勝勢下逼和。
            }

            if(score > result.score){
                result.score = score;
                result.best_move = action;
                result.pv = {action};
            }
            if(score > alpha){
                alpha = score;
            }
            if(alpha >= beta){
                break;
            }
        }

        history.pop(state->hash());

        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        return result;
    }
}

/*============================================================
 * MiniMax — eval_ctx
 *
 * Negamax without pruning. Caller manages memory.
 *============================================================*/
int MiniMax::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
){
    // new_updata
    if(state->legal_actions.empty()){
        state->get_legal_actions();
    }
    return pvs(state, depth, M_MAX, P_MAX, history, ctx, p, ply);
}


/*============================================================
 * MiniMax — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult MiniMax::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult final_result;
    final_result.depth = depth;
    int previous_score = 0;
    bool have_complete_layer = false;

    // new_updata
    // 每次根搜尋重置 killer 與 history heuristic，避免不同深度間的污染。
    std::memset(g_killers, 0, sizeof(g_killers));
    std::memset(g_history, 0, sizeof(g_history));

    if(state->legal_actions.empty()){
        state->get_legal_actions();
    }

    for(int current_depth = 1; current_depth <= depth; ++current_depth){
        if(ctx.stop){
            break;
        }

        int alpha = M_MAX;
        int beta = P_MAX;
        if(current_depth >= 3){
            alpha = previous_score - 50;
            beta = previous_score + 50;
        }

        SearchResult layer_result = search_root(state, current_depth, history, ctx, p, alpha, beta);
        if(ctx.stop){
            break;
        }

        if(current_depth >= 3 && (layer_result.score <= alpha || layer_result.score >= beta)){
            layer_result = search_root(state, current_depth, history, ctx, p, M_MAX, P_MAX);
            if(ctx.stop){
                break;
            }
        }

        if(!ctx.stop){
            have_complete_layer = true;
            previous_score = layer_result.score;
            final_result = layer_result;
        }

        if(p.report_partial && ctx.on_root_update && !ctx.stop){
            ctx.on_root_update({final_result.best_move, final_result.score, current_depth, 0, static_cast<int>(state->legal_actions.size())});
        }
    }

    if(!have_complete_layer){
        final_result.score = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    }
    final_result.nodes = ctx.nodes;
    final_result.seldepth = ctx.seldepth;
    return final_result;
}


/*============================================================
 * MiniMax — default_params / param_defs
 *============================================================*/
ParamMap MiniMax::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> MiniMax::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
