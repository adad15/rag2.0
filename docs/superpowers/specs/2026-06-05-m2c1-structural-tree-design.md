# M2c-1 结构化建树（抽象层级条款树）设计

- 日期：2026-06-05
- 类型：子项目 spec（M2c 第一刀：结构化建树；三文本 / 落库另立 spec）
- 来源：[M2b 解析质量 spec](2026-06-03-m2b-ocr-quality-design.md)、[M2b 进度与交接](../2026-06-04-m2b-progress-and-handoff.md) §5.2、[总览路线图](2026-05-31-rag-overview-roadmap-design.md) M2
- 分支：V2.1
- 状态：已与项目负责人确认设计，待写实现计划
- 背景：M2b 把 `parse_cache/<id>.json` 的 `elements` 洗成了干净、带 `region`/`clause_no`/`suspect` 的富 IR（`schema_version=2`）。原计划在 M2c-1 前插一层 M2b-2「PP-Structure 精度修复」（证据透传 + 块内显式边界拆分 + 质量门禁），但评估后认为 ROI 不划算（标记类工作无消费闭环、置信度聚合依赖未验证的引擎字段）而**取消**；M2c-1 直接消费 M2b 的 `schema_version=2` 缓存。当前 ingest 仍走 M1 老路（`split_clauses` 逐页朴素切），**完全没消费富 elements**。M2c 要把这条 v2 扁平元素流变成**有层级的条款树**，供检索消费。M2c 整体太大，本 spec 只做第一刀——**结构化建树**。

---

## 0. 在 M2c 几刀里的位置

```
M2c-1  结构化建树（v2 元素流 → 抽象层级条款树 + page_clause_map）—— 本 spec  纯 C++ 转换
M2c-2  三文本生成（atomic / retrieval / context）                          依赖 M2c-1
M2c-3  落库与接线（PG schema 扩展 + Milvus 标量 + 替换 ingest 的 split_clauses） 依赖 M2c-1/2
```

（原本前置一层 M2b-2「PP-Structure 精度修复」已取消，见背景；M2c-1 直接消费 M2b 的 v2 缓存。）

拆分理由：建树器（作用域栈 + 多格式识别）是整个 M2c 的**核心与风险**；做成纯函数 + 多份文档 fixture 的单测，能把"识别每个标题是几级"这个最难的点单独打磨到对，不被落库/embedding 干扰。后面几层相对机械。

**接缝**：沿用项目"可缓存接缝"哲学。M2c-1 产出 `data/tree_cache/<id>.json`，是 M2c-1↔M2c-2/3 的接缝——不重跑 OCR 就能反复迭代建树器。

---

## 1. 一句话现状与目标

- **现状**：M2c-1 消费 M2b 的 `parse_cache` v2：一条扁平 `elements` 流，带 `clause_no`、`region`、`type`、`raw_label`、`title`、`text`、`suspect`。`element.level` 是 app.py 写死的 1（假的），`type` 是 PP-Structure `block_label` 的子串映射（排版意义自洽，但**不表达逻辑层级**），`raw_label` 的 title/text 区分按排版判定、不可靠。
- **目标**：消费 v2 `elements`，产出**抽象层级条款树**——每个节点带层级（1/2/3…）、原始号、标题、（叶子的）正文、图表题附件、页范围、父子关系、路径式 `node_id`；外加 `page_clause_map`。序列化到 `tree_cache`，并出 `treecheck` CLI 体检。**不生成三文本、不碰 PG/Milvus/embedding、不改检索。** PP-Structure 的原始块边界混乱（号埋在块中部）作为已知局限处理，见 §10。

---

## 2. 核心设计决策（来自 brainstorm 的关键结论）

1. **抛弃语义命名（章/节/条），只用抽象层级（1/2/3…）。** 不是所有规范都有"章节条"这套术语，纯层级模型更通用。
2. **定级主键 = `clause_no` 点深度**（它带真层级）；**`element.level` 忽略（假的）、`type`/`raw_label` 不作定级依据**（排版判定不可靠）。`type=Table/Formula`（`"table"/"formula"` 子串）仍可信，留用于识别表格/公式。
3. **格式映射表 + 目次驱动**：每种文档格式一张映射表（`clause_no`/文本模式 → 抽象层级）；上文先读目次（目次是文档自己声明的结构）选格式，目次糊/缺则从正文兜底推断。
4. **作用域栈**：同一个号 `N` 在根是 L1、在 `T` 号作用域内是 L3——含义依赖作用域，故需阅读顺序维护作用域栈。
5. **检索单元 = 格式定义的检索层级节点**，普通十进制规范与试验规程/汇编型都默认落在 **L3**。深于检索层级的下级编号不单独成为检索节点，而是并入当前 L3 正文；深度不足的分支退到其最深一级（如总则 `1.0.x`）。
6. **路径式 `node_id`**：汇编里 `2`、`3` 这类内部章号会在每个试验方法下重号，裸号会撞车，故 `node_id` 用整条路径。
7. **图表题不是检索主单元，而是条款附件**：`figure_title/chart_title/table_title` 或 `图/表/续表/附图/附表` 前缀不建树节点、不抠条款号；但不能直接丢弃，必须挂到最近的当前叶子，供 M2c-2 生成 `retrieval_text` 与后续附件回显使用。

---

## 3. 数据模型

```cpp
// src/structure/clause_tree.h
struct TreeNode {
    std::string node_id;     // 路径式: "std:5/5.2/5.2.6" | "std:4/T0306-1994/2"
    int level = 0;           // 抽象层级 1/2/3...(映射表给)
    std::string number;      // 原始号: "5.2.6" | "T 0306—1994" | ""(虚拟节点)
    std::string title;       // 标题文本(去号)
    std::string text;        // 正文(仅叶子: 本段 + 并入的无号续段/列项)
    int page_start = 0;
    int page_end = 0;
    std::string parent_id;
    std::vector<std::string> child_ids;
    bool is_leaf = false;    // 检索单元(格式定义检索层级, 或分支最深级)
    bool has_table = false;  // 含表格(table_html 留给 M5)
    bool has_figure = false; // 含图/图表题; 图片本体路径留给 M7/M8
    std::vector<std::string> captions; // 图/表/图表题附件, M2c-2 并入 retrieval_text
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
   - 若识别出的层级深于该格式的检索深度（如 B 中试验内 `2.1` 深于 L3）→ 不建节点，并入当前检索节点正文；
   - 无号文本 → 文本前缀命中"图/表/续表/附图/附表" 或 `is_caption=true` → 当 caption：**不建节点，但挂到当前叶子的 `captions`**；否则并入当前叶子的 `text`（含无号列项 / 续段）；
   - `type==Table/Formula` → 挂到当前叶子（`has_table=true`，`table_html` 留给 M5）；
   - **跳级/缺中间级**（如 L1 直接到 L3）→ 插虚拟节点（`number=""`）+ 标 `suspect="gap"`。
5. **标检索单元**：`is_leaf = 达到该格式检索深度的节点，或分支最深节点`。映射表的"检索深度"（A=L3/B=L3）是预期深度；深于检索深度的编号并入当前检索节点，深度不齐的浅分支（如 `1.0.x` 只到 L2）退到最深节点，不漏总则类浅分支。
6. **页信息**：每节点 `page_start/page_end`；构建 `page_clause_map`（页号 → 该页出现的叶子 `node_id`）。
7. **序列化** `tree_cache/<id>.json` + `treecheck` 输出体检表。

**Caption / 图片策略**：M2b 的 `is_caption` 只解决"不要把图表题误当条款"；M2c-1 进一步负责"不要把图表题丢掉"。树节点只保存 caption 文本与 `has_figure/has_table` 标记，不保存图片文件、不做视觉向量。M2c-2 生成 `retrieval_text` 时应把 `captions` 作为"相关图表"追加到条款检索文本；M2c-3 落库时可把 caption 写成条款附件。真正的 `bbox/image_path/visual` 检索仍后置到 M7/M8。

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
| 试验作用域内 `N` | **L3** | ✓（检索单元）|
| 试验作用域内 `N.M` | L4 | 并入当前 L3 |
| 试验作用域内 `N.M.K` | L5 | 并入当前 L3 |

检索深度 = **L3**。对试验规程而言，`T` 方法号下的 `1/2/3...` 通常是"目的与适用范围 / 仪具与材料 / 试验步骤 / 计算"等三级标题，粒度更适合作为检索单元；`2.1/2.2` 等细项默认并入所属 L3 正文。

**通用规则**：检索单元 = 该格式设定的检索深度那一级；某文档层级不足该深度时，退到其最深一级；某文档层级深于检索深度时，深层编号并入最近的检索单元正文。

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
- 边界：缺中间级、无号续接段并入、caption 挂载、表格挂载。

**fixture 前提（关键）：**
- 格式 A 可用现有 `12215131224082667446.json`（JTC5210）立即开工。
- **格式 B 需要一份汇编型 cache**——手里没有。**实现/测试格式 B 之前，须先 ingest 一本汇编规范（如集料试验规程汇编）生成缓存**，作为 fixture。这是格式 B 的前置任务（task-zero）。

**验收：** `treecheck` 在 JTC5210 上输出合理树（章数、L3 叶子数、page_map 全覆盖、suspect 清单）；在汇编缓存上 L2=试验号、L3=试验内检索单元，`2.1/2.2` 等细项并入 L3 正文且重号不撞。

---

## 10. 明确不解决（防范围蔓延）

原计划由前置的 M2b-2 处理 PP-Structure 的块边界混乱 / 列项缺失 / 区域乱码，该层**已取消**（ROI 不划算）。这些引擎级问题在本 spec 里**作为已知局限接受**，不在建树时治：

- **块中部埋号条款**：揉块"[上一条尾巴][块中部新条款号 N.M.K 正文]"中，`parse_clause_no`（只认块首）抠不到中部的号，该块被当无号正文**并入上一条**，于是这条埋号条款**不会成为独立树节点**（其文本仍在，但落在错误父条下）。交接文档判断真正受影响的真条款是少数，故先接受。
- **列项缺失 / 区域乱码**：不自动标记（无人工复核闭环时标了也无人消费）。`suspect` 仅透传 M2b 已有的 seq/short。

本 spec 仍然**不生成三文本、不碰 PG/Milvus、不改检索、不自动补缺失文字、不猜测块内尾巴归属**。

> **未来选项（不进本期）**：若实测埋号条款影响显著，可在 `tree_builder` 走元素流时加一步「块中部扫号拆分」——复用 `parse_clause_no` 的守卫（命中点前为句号/分号/冒号、后跟中文标题、拒数值单位），把块中部的合法条款号拆成独立元素再建节点。这是当初 M2b-2 中唯一有真"修复"价值的点，作为可选增强保留在此备忘。

---

## 10.1 2026-06-08 回归 bug 清单与修复口径

本轮真实缓存抽查暴露出 5 个 M2c-1 必须修的结构化 bug：

1. **图内说明误判为 caption**：`图中：MQI——...`、`表中：PCI——...` 是图表内部解释文本，应并入当前条款 `text`，不能进入 `captions`。根因是 caption 规则把所有 `图/表` 前缀都当题名，修复口径是在 `is_caption_label` 与 `tree_builder::looks_like_caption` 同时排除 `图中/表中`。
2. **正式附录没有成为树节点**：`parse_cache` 已有 `附录A/附录B/附录C`、`B.0.1/C.0.1` 等内容，但 `tree_builder` 只用数字 `clause_no` 建节点，导致附录标题和字母条款无节点可挂。修复口径是在 Appendix 区域内识别 `附录X` 为 L1，识别 `X.0.1 / X.1.1` 为其下条款节点。
3. **表格/显式公式块只打标不落 tree_cache**：此前 `Table/Formula` 只更新 `has_table`，`table_html` 与公式文本没有进入树缓存。修复口径是在 `TreeNode` 增加轻量附件字段：`table_htmls`、`formulas`、`has_formula`；`Table` 保存 HTML，`Formula` 保存公式文本并并入节点 `text`；图片本体仍不入缓存。
4. **页全文有独立公式、elements 缺公式块**：真实 JTC5210 缓存里，`pages[].text` 能看到 `\mathrm{MQI}=...` 等独立公式行，但 `elements[].text` 只剩“按式计算/式中”解释，导致只消费 `elements` 的 `tree_cache` 丢公式主体。修复口径是在 M2c-1 建树后，从同页 `pages[].text` 按页内最近条款号回填“独立公式行”到对应 `TreeNode.text/formulas`，并置 `has_formula=true`；只处理无中文、含等号和数学标记的独立行，避免把普通内联数学表达式都升级成公式。
5. **条文说明被误留在 appendix 分组**：真实缓存里正式附录之后出现独立 `条文说明` 文本块，但旧 `tag_regions` 只允许 Heading 切换，导致后续说明节点写成 `:appendix:`。修复口径是在 `tag_regions` 与 M2c `region_segmenter` 中，对 Appendix 后出现的独立 `条文说明` 做 explanation 切换；正文区普通文本 `条文说明` 仍不触发。

这 5 类都属于结构化接缝 bug，不是 OCR 模型没有输出。修复后 `parse_cache` 仍保留模型原始证据，`tree_cache` 才承载 M2c-1 的最终结构判断。

---

## 11. 影响的现有文件

- **新增**：`src/structure/`（clause_tree.h、format_profile.{h,cpp}、toc_parser.{h,cpp}、region_segmenter.{h,cpp}、tree_builder.{h,cpp}）+ 对应 `tests/test_*.cpp`。
- **改动**：`src/main.cpp`（加 `treecheck` 子命令）；vcxproj 加新文件。
- **不改**：M2b 全部（parser.h / ocr_normalize / app.py / parse_cache）、ingest_pipeline、PG/Milvus/检索——这些是后续 spec 的事。
