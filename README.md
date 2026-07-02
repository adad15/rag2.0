# rag2.0 —— 规范文档 RAG 系统

面向工程技术规范、道路桥梁规范、试验规程、材料标准等专业文档，构建一个 **可溯源、可维护、可评估、可持续更新** 的规范知识检索与辅助决策系统。

> 目标不是做通用问答，而是做规范条文检索 + 溯源回答：回答必须给出标准号、规范名称、版本、条款号等来源，并对召回不足、版本冲突、废止规范等情况明确提示或拒答。

## 架构

C++ 主程序负责确定性编排（入库流程、结构化处理、查询路由、多路召回、融合、上下文组装、溯源校验）；重模型推理与文档视觉理解放在 Python/GPU 服务。

```text
            ┌──────────────── C++ 主程序 (本仓库) ────────────────┐
            │  入库: 文件 → 解析 → 条级分片 → 元数据 → PG → 向量化 → Milvus │
            │  查询: query → 查询理解 → 多路召回 → 融合 → 回填 → DeepSeek 生成 │
            └───┬──────────┬───────────┬───────────┬───────────┘
                ▼          ▼           ▼           ▼
          解析服务     Embedding    Milvus      PostgreSQL
        (MinerU/poppler) (Qwen3)   向量/BM25    结构化底座
                                                  │
                                            DeepSeek API (生成)
```

- **PostgreSQL** 是系统底座（标准元数据、条款树、表格结构化、版本/映射关系）；Milvus 只负责召回，权威字段一律回查 PG。
- **文本路为主，视觉路为补充**；条款号原子性不可破坏（一个 `clause_id` 对应一个条款/款/项单元）。

## 技术栈

| 层 | 选型 |
|---|---|
| 主程序 | C++20，VS2022 / MSBuild |
| 依赖管理 | vcpkg（清单模式 manifest） |
| HTTP | cpp-httplib[openssl]（统一访问 Milvus / Embedding / DeepSeek） |
| JSON | nlohmann/json |
| PostgreSQL | libpqxx 8.0 |
| 向量库 | Milvus 2.5（REST API v2，非 gRPC） |
| PDF | poppler-cpp（原生 PDF）/ MinerU（扫描件、表格、版面，规划中） |
| 日志 | spdlog |
| 测试 | doctest |
| 生成 | DeepSeek API（OpenAI 兼容） |
| Embedding | Qwen3-Embedding-8B（经 OpenAI 兼容端点，如 SiliconFlow） |

## 目录结构

```text
rag2.0.sln                  解决方案（x64 Debug/Release）
vcpkg.json                  依赖清单（builtin-baseline 锁版本）
rag2.0/        rag2.0.vcxproj         应用工程
rag2.0.tests/  rag2.0.tests.vcxproj   doctest 测试工程
src/                        源码（两工程通配符共享）
  rag_config.h              环境变量配置
  http/        http_client          HTTP 封装
  db/          pg_client + schema.sql 结构化底座
  milvus/      milvus_rest          Milvus REST v2 客户端
  embedding/   embedding_client     文本→向量（契约）
  parse/       parser + poppler     解析器契约与实现
  ingest/      clause_splitter ...  分片与入库
  retrieve/    retriever/candidate  检索契约与候选
  generate/    deepseek/context ... 生成与上下文
tests/                      单元测试
docs/superpowers/           设计 spec 与实现计划（M0–M9）
```

## 构建

前置：Windows + VS2022（v143）、vcpkg（本仓库假设位于 `D:\vcpkg` 并已 `vcpkg integrate install`）。

```powershell
# 依赖在首次构建时按 vcpkg.json 自动还原（复用二进制缓存）
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
    rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m

# 跑单元测试
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

也可直接用 VS2022 打开 `rag2.0.sln` 构建。

### 构建注意事项（已在工程中处理）

- **`/utf-8` 必需**：源码为 UTF-8 且含中文注释，中文 locale 下 MSVC 默认按 GBK 解析会误读、吞掉声明。两个工程已加 `/utf-8`。
- **C++20**：libpqxx 8.0 头文件要求 C++20。
- **Milvus 走 REST**：避免 Windows 上构建 gRPC C++ SDK 的麻烦。
- **DLL 部署**：vcpkg 构建后用 `pwsh` 自动拷贝依赖 DLL 到 exe 旁；需安装 PowerShell 7。运行时也可把 `<vcpkg-installed>\x64-windows\debug\bin` 加入 PATH。

## 配置（环境变量）

参见 [`.env.example`](.env.example)。关键项：

| 变量 | 说明 |
|---|---|
| `RAG_PG_CONNINFO` | PostgreSQL 连接串 |
| `RAG_MILVUS_BASE_URL` / `RAG_MILVUS_TOKEN` | Milvus REST 地址与令牌 |
| `RAG_EMBED_BASE_URL` / `RAG_EMBED_PATH` / `RAG_EMBED_MODEL` / `RAG_EMBED_DIM` / `RAG_EMBED_KEY` | Embedding 服务（OpenAI 兼容） |
| `RAG_DEEPSEEK_BASE_URL` / `RAG_DEEPSEEK_KEY` / `RAG_DEEPSEEK_MODEL` | DeepSeek 生成 |
| `RAG_RERANK_MODE` | 重排模式：`off\|light\|model\|hybrid`（默认 `off`） |
| `RAG_RERANK_MODEL` / `RAG_RERANK_INSTRUCTION` | M5.2 模型 reranker：模型名、任务指令 |
| `RAG_RERANK_KEY` | 模型 reranker 鉴权 key（空则回退 `RAG_DEEPSEEK_KEY`） |
| `RAG_RERANK_BASE_URL` / `RAG_RERANK_PATH` / `RAG_RERANK_TIMEOUT_SEC` | 模型 reranker 服务地址、路径、超时秒数 |
| `RAG_DOC_PATH` | 待入库 PDF 路径 |

## 用法

```powershell
rag2.exe smoke    # 连通性冒烟：PostgreSQL / Milvus / DeepSeek / poppler
rag2.exe ingest   # 入库一份文档（文本路 MVP 可用）
rag2.exe query "你的问题"   # 三路召回 + RRF + 方法号置顶 + DeepSeek 溯源回答
```

## 路线图

采用 **walking skeleton 先行、再横向加厚** 的策略（详见 `docs/superpowers/`）：

| 里程碑 | 内容 | 状态 |
|---|---|---|
| **M0** | 环境与依赖就绪 + 四项连通性冒烟 | ✅ 完成 |
| **M1** | 端到端最小骨架：poppler → PG → 单路 dense 召回 → DeepSeek 溯源回答 | ✅ 完成 |
| **M2** | 入库正规化：条款层级树、三文本分离、OCR/MinerU 质量、分片持久化 | ✅ 完成 |
| **M3** | 文本路三路召回：查询理解 + dense/BM25/PG精确 + RRF + 列举关键词直查补召回 + 方法号/条款号置顶 | ✅ 完成 |
| **M4** | 评估闭环：100 题标注集、Group Recall/Complete + nDCG/Distractor/Redundancy 富指标、rule/LLM 双 planner | ✅ 完成 |
| **M5.1** | Reranker 框架 + 轻量规则重排（`rerank.mode=off\|light`，仅普通题；RRF 序小步加分 + 同条款去重） | ✅ 框架完成（默认 off） |
| **M5.2** | 大模型 reranker（`rerank.mode=model\|hybrid`，Qwen3-Reranker-8B + 打分缓存） | ✅ 完成（默认 off，四模式实测中性）|
| M5.3+ | 表格 cell 定位、引用图扩展、LLM 元数据 | 设计 |
| M6 | 版本管理与运维健壮性：版本生命周期、一致性、降级、灰度迁移 | 设计 |
| M7–M8 | 视觉路（page / block 级） | 设计 |
| M9 | both 模式 + auto 路由 | 设计 |

> 当前文本路水平（100 题富指标，rule planner，top-20，文档侧 dense/BM25 双文本后）：Group Recall@20≈0.997、Complete@20≈0.99、nDCG@20≈0.80、Distractor-before-gold≈0.07。
>
> 文档侧双文本：dense 用干净 `embedding_text`、BM25 用富化 `bm25_text`（标准号/路径/条款/确定性检索词），修复了"正确条文极短、BM25 漏召"类问题（如通用硅酸盐水泥安定性两种判定方法 → 召回升至 top1）。
>
> M5.1 LightReranker：`RAG_RERANK_MODE=off|light`（仅普通题，列举题走原覆盖链路）。rule planner 下 off-vs-light 大体中性（Distractor@20 0.68→0.60↓、Redundancy@20 0.256→0.264 微升、Recall/Complete/nDCG 持平），故**默认 off**；其关键词加分需 LLM planner（key_terms 非空）才显价值，待后续标定/接 M5.2 模型 reranker 再评。
>
> M5.2 ModelReranker：新增 `RAG_RERANK_MODE=model|hybrid`（保留 `off|light`）。模型 = 硅基流动 `Qwen/Qwen3-Reranker-8B`（`/v1/rerank`），支持任务指令 `RAG_RERANK_INSTRUCTION`；rerank 打分落盘缓存（`data/rerank_cache/`，键含 model+instruction+query+候选集+doc 版本），eval 可重跑确定、省 token。语义上仅非列举题参与：`model` 失败/超时/非法响应回退 RRF 原顺序，`hybrid` 回退 LightReranker；模型成功后与 light 走同一 `RAG_RERANK_MAX_PER_CLAUSE` 多样性后处理，条款/方法号 pin 仍在最后。默认仍 `off`。**四模式 rich eval 实测**（100 题，rule planner，top-20）：Recall@20/Complete@20 四模式持平（0.997/0.99，不伤召回）；model/hybrid 仅 nDCG@20 微升（0.799→0.807）、nDCG@1 微降（0.885→0.865），而 Redundancy@20（0.253→0.284）、Distractor Hit@20（0.906→0.941）反而变差，Distractor-before-gold 四模式全为 0.0706（无改善）——即 model/hybrid **未明显降低排序风险，故默认保持 off**；model≈hybrid（本轮 model 全程未失败、hybrid 兜底未触发，退化为同 model）。研判：本评测集干扰项取自检索器自身 dense 近邻，对纯语义 reranker 天然不利，其价值更可能在 `llm` planner（key_terms 非空）下显现，待后续评。复现：切 `RAG_RERANK_MODE` 后 `rag2.exe eval eval/retrieval_questions_100.annotated.json 30 rule --rich`。**另在 `llm` planner 下同跑一轮,结论一致**:model 的 nDCG 增益扩大(nDCG@1 由 rule 下 −0.02 转为 +0.025、nDCG@20→0.807),但 **Distractor-before-gold 反升(llm-off 0.059→llm-model 0.082)**、Redundancy@20/Distractor Hit@20 仍差 → 仍不过门槛,**默认 off 不变**。附带发现:**换 planner 比换 reranker 更值**——光把 planner rule→llm 就把 off 的 Distractor-before-gold 从 0.071 降到 0.059(查询理解是更大的杠杆),且 llm 下 light 的 Recall@20/Complete@20 达 1.0/1.0。下一步性价比在 planner 侧,不在重排侧。
>
> qp-v2 planner prompt 尝试（2026-07-02，**已回滚**）：让 GeneralFact 也产出 key_terms/sparse/dense 三件套 + 靶向 few-shot + 安全阀。机制上完全生效（194/194 题 sparse 非空、判别词干净），但**伤召回**：llm/off Complete@20 0.99→0.96、Group Recall@20 0.997→0.970，nDCG/Distractor-before-gold 全面变差 → 破"召回不降"硬门槛，按 spec 判定回滚（revert 后 qp-v1 缓存即时复原，doctest 227 绿）。根因两条：① 满库词 ban list 一刀切**误伤复合实体**——"通用硅酸盐水泥"含被禁词"水泥"被 LLM 剥掉，该题召回 0/1 全灭；② "一句话聚焦重述"对**多要点题**（G=2/3 证据组）天然丢证据，3 道超薄罩面题新失败。教训：对召回已近满分的系统，激进查询改写是负优化；若再试（qp-v3）需按题型分流（多要点题不改写）、复合实体保护、且改写只做加法不做减法。spec/plan：`docs/superpowers/{specs,plans}/2026-07-02-qp-v2-planner-prompt*`。

## 评估与检索自查

```powershell
# 单查检索自查（看三路召回与排序）
rag2.exe retrievecheck "通用硅酸盐水泥安定性需要通过哪两种方法判定合格？" 20

# 富指标评估（Group Recall/Complete + nDCG/Distractor/Redundancy）
rag2.exe eval eval/retrieval_questions_100.annotated.json 30 rule --rich

# 生成侧评估（条款引用 + 数值准确率）
rag2.exe eval eval/dataset_seed.json 30 rule --gen
```

> 标注集 `eval/retrieval_questions_100.annotated.json` 由 `scripts/mine_annotations.py` 生成；
> 完整人审文件用 `python scripts/mine_annotations.py --review-only --in <annotated.json>` 重建。

## 设计文档

- 总体技术设计：[`docs/规范文档rag系统技术设计文档_整合版.md`](docs/规范文档rag系统技术设计文档_整合版.md)
- 路线图与各里程碑设计/计划：[`docs/superpowers/`](docs/superpowers/)
