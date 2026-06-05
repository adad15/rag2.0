# M2c-1 结构化建树（抽象层级条款树）设计

- 日期：2026-06-05
- 类型：子项目 spec（M2c 第一刀：结构化建树；三文本 / 落库 / 引擎修复另立 spec）
- 来源：[M2b 解析质量 spec](2026-06-03-m2b-ocr-quality-design.md)、[M2b 进度与交接](../2026-06-04-m2b-progress-and-handoff.md) §5.2、[总览路线图](2026-05-31-rag-overview-roadmap-design.md) M2
- 分支：V2.1
- 状态：已与项目负责人确认设计，待写实现计划
- 背景：M2b 把 `parse_cache/<id>.json` 的 `elements` 洗成了干净、带 `region`/`clause_no`/`suspect` 的富 IR（`schema_version=2`）。但当前 ingest 仍走 M1 老路（`split_clauses` 逐页朴素切），**完全没消费富 elements**。M2c 要把这条扁平元素流变成**有层级的条款树**，供检索消费。M2c 整体太大，本 spec 只做第一刀——**结构化建树**。

---

## 0. 在 M2c 几刀里的位置

```
M2c-1  结构化建树（元素流 → 抽象层级条款树 + page_clause_map）—— 本 spec   纯 C++ 转换
M2c-2  三文本生成（atomic / retrieval / context）                          依赖 M2c-1
M2c-3  落库与接线（PG schema 扩展 + Milvus 标量 + 替换 ingest 的 split_clauses） 依赖 M2c-1/2
M2c-4  §4 引擎垃圾修复（块中部拆分 / 续接归位 / 乱码质检门禁）               依赖 M2c-1
```

拆分理由：建树器（作用域栈 + 多格式识别）是整个 M2c 的**核心与风险**；做成纯函数 + 多份文档 fixture 的单测，能把"识别每个标题是几级"这个最难的点单独打磨到对，不被落库/embedding 干扰。后面几层相对机械。

**接缝**：沿用项目"可缓存接缝"哲学。M2c-1 产出 `data/tree_cache/<id>.json`，是 M2c-1↔M2c-2/3 的接缝——不重跑 OCR 就能反复迭代建树器。

---

## 1. 一句话现状与目标

- **现状**：`parse_cache` 里是一条扁平 `elements` 流，带 `clause_no`/`region`/`type`/`raw_label`/`title`/`text`。`element.level` 是 app.py 写死的 1（假的），`type` 是 PP-Structure `block_label` 的子串映射（排版意义自洽，但**不表达逻辑层级**），`raw_label` 的 title/text 区分按排版判定、不可靠。
- **目标**：消费 `elements`，产出**抽象层级条款树**——每个节点带层级（1/2/3…）、原始号、标题、（叶子的）正文、页范围、父子关系、路径式 `node_id`；外加 `page_clause_map`。序列化到 `tree_cache`，并出 `treecheck` CLI 体检。**不生成三文本、不碰 PG/Milvus/embedding、不改检索、不修 §4 引擎垃圾。**

---

## 2. 核心设计决策（来自 brainstorm 的关键结论）

1. **抛弃语义命名（章/节/条），只用抽象层级（1/2/3…）。** 不是所有规范都有"章节条"这套术语，纯层级模型更通用。
2. **定级主键 = `clause_no` 点深度**（它带真层级）；**`element.level` 忽略（假的）、`type`/`raw_label` 不作定级依据**（排版判定不可靠）。`type=Table/Formula`（`"table"/"formula"` 子串）仍可信，留用于识别表格/公式。
3. **格式映射表 + 目次驱动**：每种文档格式一张映射表（`clause_no`/文本模式 → 抽象层级）；上文先读目次（目次是文档自己声明的结构）选格式，目次糊/缺则从正文兜底推断。
4. **作用域栈**：同一个号 `N` 在根是 L1、在 `T` 号作用域内是 L3——含义依赖作用域，故需阅读顺序维护作用域栈。
5. **检索单元 = 分支终端号节点（条级叶子）**，预期落在三级标题（A=L3）；按格式可设更深预期（汇编 L4，兑现"条级"）。深度不齐的分支各取自己的终端，浅分支（如总则 `1.0.x`）不漏。
6. **路径式 `node_id`**：汇编里 `2.1` 在每个试验内重号，裸号会撞车，故 `node_id` 用整条路径。

---

## 3. 数据模型

```cpp
// src/structure/clause_tree.h
struct TreeNode {
    std::string node_id;     // 路径式: "std:5/5.2/5.2.6" | "std:4/T0306-1994/2/2.1"
    int level = 0;           // 抽象层级 1/2/3...(映射表给)
    std::string number;      // 原始号: "5.2.6" | "T 0306—1994" | ""(虚拟节点)
    std::string title;       // 标题文本(去号)
    std::string text;        // 正文(仅叶子: 本段 + 并入的无号续段/列项)
    int page_start = 0;
    int page_end = 0;
    std::string parent_id;
    std::vector<std::string> child_ids;
    bool is_leaf = false;    // 检索单元(L3 或最深级)
    bool has_table = false;  // 含表格(table_html 留给 M5)
    std::string suspect;     // 透传 M2b 的 suspect + 树级异常(如 "gap")
};

struct ClauseTree {
    std::string standard_id;
    std::string standard_no;
    std::vector<TreeNode> nodes;                              // 扁平存储, 靠 id 连
    std::map<int, std::vector<std::string>> page_clause_map;  // 页号 → 叶子 node_id 列表
    std::string format_profile;                              // "A_decimal" | "B_testno"
    int schema_version = 1;                                  // tree_cache 自身版本
};
```

`node_id` 路径式是汇编重号不撞车的关键。`nodes` 扁平存、靠 `parent_id`/`child_ids` 连，便于 JSON 序列化与单测断言。

---

## 4. 组件（新建 `src/structure/`）

| 文件 | 职责 | 可单测 |
|---|---|---|
| `clause_tree.h` | §3 数据结构 + tree↔json（`tree_to_json` / `tree_from_json`） | ✓ |
| `format_profile.{h,cpp}` | 映射表（模式→层级）+ 格式选择 + 检索深度 | ✓ |
| `toc_parser.{h,cpp}` | 解析目次 blob → 格式提示 + 顶层骨架 | ✓ |
| `region_segmenter.{h,cpp}` | 区域分割（body/explanation/appendix/front_matter/toc），见 §7 | ✓ |
| `tree_builder.{h,cpp}` | 作用域栈主算法（§5） | ✓ |
| `main.cpp`（改） | 加 `treecheck <cache>` 子命令 | — |

每个单元职责单一、接口清晰、可独立单测。

---

## 5. 主算法（tree_builder）

输入 `ParsedDoc`，输出 `ClauseTree`：

1. **区域分割**（region_segmenter）：切出 body / explanation / appendix / front_matter / toc。**每个内容区各建一棵树**（body 树、explanation 镜像树分开，防撞号——缓存里那两个目次 blob 已证实存在镜像结构）。
2. **格式探测**：toc_parser 解析目次 blob：列了 `T dddd—yyyy` → 选 **B_testno**，否则 **A_decimal**。**目次缺失/糊掉 → 兜底**：扫正文 `clause_no` 模式推断（出现 `T` 号 → B，否则 A）。
3. **选映射表**（§6）。
4. **作用域栈走元素流**（阅读顺序）：
   - 文本/号命中 `T` 正则（`T\s*\d{4}[—\-]\d{4}`）→ 按表定级，压栈建节点；
   - 有 `clause_no` → 按表 + 当前作用域定级，弹栈到该级父、建节点、压栈；
   - 无号文本 → 文本前缀命中"图/表/续表/附图/附表" → 当 caption（不入树，或挂当前叶子的 caption）；否则**并入当前叶子的 `text`**；
   - `type==Table/Formula` → 挂到当前叶子（`has_table=true`，`table_html` 留给 M5）；
   - **跳级/缺中间级**（如 L1 直接到 L3）→ 插虚拟节点（`number=""`）+ 标 `suspect="gap"`。
5. **标叶子**：`is_leaf = 带正文且无子条款节点的终端号节点`（即该分支最深的号），聚合正文。映射表的"检索深度"（A=L3/B=L4）是**预期深度**，实际叶子按"分支终端"取——这样深度不齐的分支（如 `1.0.x` 只到 L2、`5.x.y` 到 L3）都能正确取到条级叶子，不漏总则类浅分支。
6. **页信息**：每节点 `page_start/page_end`；构建 `page_clause_map`（页号 → 该页出现的叶子 `node_id`）。
7. **序列化** `tree_cache/<id>.json` + `treecheck` 输出体检表。

---

## 6. 映射表（纯层级，建表即定粒度）

数据驱动：加新格式 = 加一张表，**不改算法**。

**A_decimal**（目次列 `5.1 路基` 这类十进制时选）：

| clause_no 模式 | 层级 | 是否叶子 |
|---|---|---|
| `N` | L1 | |
| `N.M`（M≠0） | L2 | |
| `N.M.K`（M≠0） | **L3** | ✓（终端 → 条级叶子）|
| `N.0.K`（章直属，`.0.` 占位、无真节） | L2 | ✓（终端 → 条级叶子）|
| 无号列项 1/2/3 | — | 并入叶子 |

预期检索深度 = **L3**；`.0.` 占位级折叠（`1.0.1` 直接挂章 1，不造虚拟 `1.0` 节点）。叶子按"分支终端号节点"取（见 §5.5），故 `1.0.1`（L2 终端）与 `5.1.1`（L3 终端）都是条级叶子。

**B_testno**（目次列 `T 0306—1994` 时选）：

| 模式（看作用域） | 层级 | 是否叶子 |
|---|---|---|
| 根的 `N` | L1 | |
| `T dddd—yyyy` | L2 | |
| 试验作用域内 `N` | L3 | |
| 试验作用域内 `N.M` | **L4** | ✓（检索单元）|
| 试验作用域内 `N.M.K` | L5 | 并入叶子 |

检索深度 = **L4**（兑现"汇编也要条级 `2.1`"）。

**通用规则**：检索单元 = 该格式设定的检索深度那一级；某文档层级不足该深度时，退到其最深一级。

---

## 7. 区域分割归属（决策：移进 M2c-1）

M2b 的 `tag_regions` 是按扁平 JTC5210 调的（靠"目次/附录/条文说明标题 + 首章检测"分区），汇编型大概率分错。**决策：M2c-1 自己重做区域分割**（format-aware），M2b 的 `region` 字段降级为弱提示（M2c-1 不强信）。好处：所有结构解释集中一处、单测覆盖、迭代不重灌。**M2b 不改**（保持"诚实清洗、不碰结构"定位）。

---

## 8. 错误处理（沿用 M2b "warn 不抛"风格）

- 目次糊/缺 → 兜底从正文推断格式 + `spdlog::warn`；
- 无法识别的号 → 并入当前叶子正文 + 标 `suspect`；
- 跳级/缺中间级 → 插虚拟节点 + `suspect="gap"`；
- 某内容区无叶子 → warn（不抛）；
- 纯函数为主，非关键失败不抛异常，调用方无需 try。

---

## 9. 测试 + fixture 前提

**单测：**
- `format_profile`：模式 → 层级映射（A/B 两表全模式覆盖）；
- `toc_parser`：喂真实 JTC5210 目次 blob，断言识别出 A_decimal + 顶层骨架；喂含 `T` 号的 blob 断言 B_testno；
- `tree_builder`（格式 A）：对 JTC5210 缓存断言树形（L1≈7 章；条级叶子≈95，其中 `N.0.K` 类约 13 在 L2、`N.M.K` 类约 82 在 L3）、`page_clause_map` 覆盖、虚拟节点；
- 边界：缺中间级、无号续接段并入、caption 识别、表格挂载。

**fixture 前提（关键）：**
- 格式 A 可用现有 `12215131224082667446.json`（JTC5210）立即开工。
- **格式 B 需要一份汇编型 cache**——手里没有。**实现/测试格式 B 之前，须先 ingest 一本汇编规范（如集料试验规程汇编）生成缓存**，作为 fixture。这是格式 B 的前置任务（task-zero）。

**验收：** `treecheck` 在 JTC5210 上输出合理树（章数、L3 叶子数、page_map 全覆盖、suspect 清单）；在汇编缓存上 L2=试验号、L4 叶子=条。

---

## 10. 明确不解决（防范围蔓延）

§4 那三类 PP-Structure 引擎垃圾——揉块埋号的 ~77 块、跨页丢的列项、区域乱码——**本 spec 照搬带 `suspect` 进树，不主动救**，留 M2c-4。因此第一版叶子数会少于"理论条数"，属预期内。本 spec 也**不生成三文本、不碰 PG/Milvus、不改检索**。

---

## 11. 影响的现有文件

- **新增**：`src/structure/`（clause_tree.h、format_profile.{h,cpp}、toc_parser.{h,cpp}、region_segmenter.{h,cpp}、tree_builder.{h,cpp}）+ 对应 `tests/test_*.cpp`。
- **改动**：`src/main.cpp`（加 `treecheck` 子命令）；vcxproj 加新文件。
- **不改**：M2b 全部（parser.h / ocr_normalize / app.py / parse_cache）、ingest_pipeline、PG/Milvus/检索——这些是后续 spec 的事。
