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
| M5 | 文本路增强：reranker、表格 cell 定位、引用图扩展、LLM 元数据 | 设计 |
| M6 | 版本管理与运维健壮性：版本生命周期、一致性、降级、灰度迁移 | 设计 |
| M7–M8 | 视觉路（page / block 级） | 设计 |
| M9 | both 模式 + auto 路由 | 设计 |

> 当前文本路水平（100 题富指标，rule planner，top-20，文档侧 dense/BM25 双文本后）：Group Recall@20≈0.997、Complete@20≈0.99、nDCG@20≈0.80、Distractor-before-gold≈0.07。
>
> 文档侧双文本：dense 用干净 `embedding_text`、BM25 用富化 `bm25_text`（标准号/路径/条款/确定性检索词），修复了"正确条文极短、BM25 漏召"类问题（如通用硅酸盐水泥安定性两种判定方法 → 召回升至 top1）。

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
