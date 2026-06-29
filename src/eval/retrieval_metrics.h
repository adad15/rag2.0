#pragma once
#include <string>
#include <vector>
#include <set>
#include <cmath>

// 点查：candidate_keys 里第一个等于 gold_key 的 1-based 排名；无命中返回 0。纯函数。
int first_hit_rank(const std::vector<std::string>& candidate_keys,
                   const std::string& gold_key);

// reciprocal rank：rank>0 → 1.0/rank；否则 0.0。
double reciprocal_rank(int rank);

// hit@k：rank 落在 [1,k] 返回 true（rank==0 即未命中，恒 false）。
bool hit_at_k(int rank, int k);

// 覆盖查：candidate_methods 里出现的、属于 gold_methods 的**去重**个数。纯函数。
int covered_count(const std::vector<std::string>& candidate_methods,
                  const std::vector<std::string>& gold_methods);

// 富指标核心：前 k 个候选覆盖了多少个证据组。
// group_keys[i] = 第 i 组的"可接受标识集"；cand_keys_by_rank[r] = 排名 r 候选的标识集
//（标识形如 "cid:<chunk_id>" / "m:<method_no>" / "c:<standard_id>|<clause_no>"）。
// 组被覆盖 = 它的标识集与某个 rank<k 的候选标识集有交集。空标识集的组永不被覆盖。纯函数。
int covered_groups_at_k(const std::vector<std::set<std::string>>& group_keys,
                        const std::vector<std::set<std::string>>& cand_keys_by_rank,
                        int k);

// 集合命中：前 k 个候选里属于 id_set 的去重个数。纯函数。
int count_in_set_at_k(const std::vector<std::string>& cand_ids_by_rank,
                      const std::set<std::string>& id_set, int k);

// 首个落在 id_set 的 1-based 排名；无命中返回 0。纯函数。
int first_rank_in_set(const std::vector<std::string>& cand_ids_by_rank,
                      const std::set<std::string>& id_set);

// 干扰项是否排在 gold 之前：first_distractor_rank>0 且
//（first_gold_rank==0[gold 未命中] 或 first_distractor_rank<first_gold_rank）。纯函数。
bool distractor_before_gold(int first_distractor_rank, int first_gold_rank);

// nDCG@k：gains_by_rank[r]=排名 r 候选增益(去重后:组首命中2/acceptable 1/重复0)；
// achievable_gains=可达增益多重集(G 个 2 + |A| 个 1)。折扣 1/log2(rank+1)。IDCG=0 返回 0。纯函数。
double ndcg_at_k(const std::vector<double>& gains_by_rank,
                 const std::vector<double>& achievable_gains, int k);

// Redundancy@k：前 k 中"冗余证据"占"有效证据"之比。
// sig_by_rank[r]=该候选证据签名集(覆盖的必要组 "g:<i>" + 方法 "m:<no>";空=非证据)。
// 有效证据=签名非空;冗余=签名非空但未引入任何新签名元素(全被更靠前候选见过)。纯函数。
double redundancy_at_k(const std::vector<std::set<std::string>>& sig_by_rank, int k);
