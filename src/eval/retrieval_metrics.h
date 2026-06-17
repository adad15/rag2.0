#pragma once
#include <string>
#include <vector>

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
