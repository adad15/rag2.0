#pragma once
#include <string>
#include <vector>
#include <set>

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
