#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"

// RRF：对多路候选按各自排名融合（score = Σ 1/(k+rank)），
// 按 chunk_id 去重、合并来源标记（"dense+exact"），降序排序，截断 top_k。纯函数。
std::vector<Candidate> rrf_fuse(const std::vector<std::vector<Candidate>>& lists,
                                int k, int top_k);

// 条款号精确命中置顶：pinned 中的 chunk_id 提到最前。
// 已在 fused 中 → 上移并去重，source 追加 "+pin"；
// 不在 → 插入第一位，source = "exact_pin"，score = 1.0。最后截断 top_k。纯函数。
std::vector<Candidate> pin_exact_clause(const std::vector<Candidate>& fused,
                                        const std::vector<std::string>& pinned_chunk_ids,
                                        int top_k);

// 错片下压：chunk_id 在 key_hit_ids 中的候选稳定排到前面（组内保持 fused 原序），
// 其余排后，最后截断 top_k。不删除、纯函数。用于列举题"含关键词的提前"。
std::vector<Candidate> demote_without_keyterms(const std::vector<Candidate>& fused,
                                               const std::vector<std::string>& key_hit_ids,
                                               int top_k);
