# M2c-1 结构化建树 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal（一句话）:** 把 M2b 产出的扁平 `parse_cache/<id>.json` 元素流，整理成一棵"抽象层级的条款树"（带页码映射），写到 `data/tree_cache/<id>.json`，并提供 `treecheck` 命令体检。

**Architecture（大白话）:** 读缓存 → 先看目录判断这本是"普通规范"还是"试验合订本"、选对应规则 → 按阅读顺序走一遍元素，边走边用"我现在在谁底下"的栈把它们搭成上下级 → 达到该格式检索层级的节点当作检索单元（层级不足则取最深节点，层级过深则并入当前检索单元）→ 给每条起一个带完整路径的唯一名字 → 存盘。**纯 C++ 转换，不碰数据库、不碰向量、不改检索。**

**Tech Stack:** C++17、doctest（测试）、nlohmann/json（读写 JSON）、spdlog（日志）、MSBuild + vcpkg。

---

## 工程约定（每个任务都会用到，先读这里）

**构建（仓库根目录 `D:\vs2022 code\rag2.0` 下 PowerShell 执行）:**
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
> 下文各任务里出现的 `& "D:\vs2022\...\MSBuild.exe" ...` 是上面这条完整命令的简写，执行时请用上面的完整路径。

**跑全部测试:**
```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```

**只跑某个测试用例（doctest 按用例名过滤）:**
```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="用例名"
```

**登记新文件（关键，否则编译不到）:** 每新增一个 `src/structure/*.cpp` 或 `*.h`，都要在**两个**工程文件里加一行：
- 主工程 `rag2.0\rag2.0.vcxproj`：`.cpp` 加到 `<ClCompile>` 段，`.h` 加到 `<ClInclude>` 段。
- 测试工程 `rag2.0.tests\rag2.0.tests.vcxproj`：`.cpp`（含 `src` 的和 `tests` 的）都加到 `<ClCompile>` 段。测试工程已配 `AdditionalIncludeDirectories=..\src`，所以 `#include "structure/xxx.h"` 能找到。

**编码:** 所有源文件存成 **UTF-8**（项目用 `/utf-8` 编译，中文注释必须 UTF-8，否则 MSVC 会误解析）。

**提交信息结尾统一加:**
```
Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

## 文件结构（先看清要建哪些、各管什么）

新建目录 `src/structure/`：

| 文件 | 大白话职责 |
|---|---|
| `clause_tree.h` | 定义"树节点"和"整棵树"长什么样 + 树和 JSON 互转的函数声明 |
| `clause_tree.cpp` | 树↔JSON 的实现 + 写盘 |
| `format_profile.h/.cpp` | "数编号定第几级"的规则表：数点、认试验号、把编号翻译成层级 |
| `toc_parser.h/.cpp` | 看目录判断这本是普通规范(A)还是合订本(B)；看不到目录就看正文兜底 |
| `region_segmenter.h/.cpp` | 把元素按区域（正文/条文说明/附录）分组，每组各建一棵树 |
| `tree_builder.h/.cpp` | 核心：走元素流、用栈搭出上下级、标叶子、起名字、做页码映射 |

改动：
- `src/main.cpp`：加 `treecheck <cache.json>` 子命令。
- 两个 `.vcxproj`：登记上面新文件。

测试（建在 `tests/`）：`test_clause_tree.cpp`、`test_format_profile.cpp`、`test_toc_parser.cpp`、`test_region_segmenter.cpp`、`test_tree_builder.cpp`。

---

## 2026-06-08 回归修复任务：caption / 附录 / 表格公式 / 页全文公式 / 条文说明

**背景：** JTC5210 真实 `parse_cache` 抽查确认，模型原始输出里已经包含正式附录、表格 HTML、公式/表达式文本和条文说明边界；其中部分公式主体只存在于 `pages[].text` 页全文，而没有进入 `elements[]` 结构化块。问题出在 M2b/M2c 归一化与建树规则没有完整消费这些信息。

**Bug 与修复：**

- [x] `图中/表中` 说明文本误入 `captions`：在 M2b `is_caption_label` 与 M2c `looks_like_caption` 双层排除，回归测试覆盖旧缓存里 `is_caption=true` 的遗留错误。
- [x] 正式附录不入树：在 Appendix 区域识别 `附录A/B/C` 为 L1 节点，识别 `B.0.1/C.0.1` 等字母条款为附录子节点；附录 A 这类以表格为主体的内容至少形成可挂载节点。
- [x] 表格/显式公式块不落 tree_cache：`TreeNode` 增加 `table_htmls`、`formulas`、`has_formula`；`Table` 保存 `table_html`，`Formula` 保存文本并并入节点 `text`，图片本体仍不进入本阶段缓存。
- [x] `pages[].text` 有独立公式但 `elements[]` 缺公式块：`tree_builder` 从页全文按页内最近条款号回填独立公式行到对应 `TreeNode.text/formulas`，并置 `has_formula=true`；只处理无中文、含等号和数学标记的独立行，避免把普通内联数学表达式都升级成公式。
- [x] `条文说明` 被 appendix 污染：`tag_regions` 处理 Appendix 后独立 `条文说明` 文本；`region_segmenter` 也做同样兜底，以便旧 parse_cache 不重跑 OCR 也能正确分组。

**验证：**

```powershell
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="tree_builder builds formal appendix roots and lettered appendix clauses"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="tree_builder keeps table html and formula text as node attachments"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="tree_builder backfills standalone formula lines from page text when elements omit them"
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="segment_regions promotes appendix-tagged explanation marker to explanation"
.\rag2.0\x64\Debug\rag2.0.exe treecheck data\parse_cache\12215131224082667446.json
```

---

## Task 1: 树的数据结构 + JSON 互转

**大白话:** 先把"一个节点长啥样""整棵树长啥样"定下来，并能存成 JSON、再读回来（往返不丢东西）。这是后面所有任务的地基。

**Files:**
- Create: `src/structure/clause_tree.h`
- Create: `src/structure/clause_tree.cpp`
- Test: `tests/test_clause_tree.cpp`

- [ ] **Step 1: 写数据结构头文件**

写 `src/structure/clause_tree.h`：

```cpp
#pragma once
#include <string>
#include <vector>
#include <map>

// 一个条款树节点。nodes 扁平存，靠 node_id / parent_id / child_ids 串联。
struct TreeNode {
    std::string node_id;     // 路径式唯一名: "<sid>:5/5.2/5.2.6" | "<sid>:4/T0306-1994/2"
    int level = 0;           // 抽象层级 1/2/3...(由 format_profile 给)
    std::string number;      // 原始号: "5.2.6" | "T 0306—1994" | ""(虚拟节点)
    std::string title;       // 标题文本
    std::string text;        // 正文(仅叶子: 本段 + 并入的无号续段/列项)
    int page_start = 0;
    int page_end = 0;
    std::string parent_id;
    std::vector<std::string> child_ids;
    bool is_leaf = false;    // 检索单元(格式定义检索层级, 或分支最深级)
    bool has_table = false;  // 含表格(table_html 留给后续 spec)
    bool has_figure = false; // 含图/图表题; 图片本体路径留给 M7/M8
    std::vector<std::string> captions; // 图/表/图表题附件, M2c-2 并入 retrieval_text
    std::string suspect;     // 透传 M2b 的 suspect + 树级异常("gap")
};

struct ClauseTree {
    std::string standard_id;
    std::string standard_no;
    std::vector<TreeNode> nodes;
    std::map<int, std::vector<std::string>> page_clause_map; // 页号 → 叶子 node_id
    std::string format_profile;   // "A_decimal" | "B_testno"
    int schema_version = 1;       // tree_cache 自身版本
};

// 树 ↔ JSON
std::string clause_tree_to_json(const ClauseTree& t);
ClauseTree  clause_tree_from_json(const std::string& json_text);
// 写盘：目录不存在则创建；打开失败仅 warn 不抛(沿用 parse_cache 风格)
void        write_tree_cache(const std::string& cache_path, const ClauseTree& t);
```

- [ ] **Step 2: 写失败测试**

写 `tests/test_clause_tree.cpp`：

```cpp
#include <doctest/doctest.h>
#include "structure/clause_tree.h"

TEST_CASE("clause_tree JSON 往返不丢字段") {
    ClauseTree t;
    t.standard_id = "sid1";
    t.standard_no = "JTC 5210-2018";
    t.format_profile = "A_decimal";
    TreeNode a; a.node_id = "sid1:5"; a.level = 1; a.number = "5"; a.title = "公路损坏分类";
    a.child_ids = {"sid1:5/5.1"};
    TreeNode b; b.node_id = "sid1:5/5.1"; b.level = 2; b.number = "5.1"; b.title = "路基";
    b.parent_id = "sid1:5"; b.is_leaf = true; b.text = "正文"; b.page_start = 12; b.page_end = 12;
    b.has_figure = true; b.captions = {"图5.1 路基损坏示意"};
    t.nodes = {a, b};
    t.page_clause_map[12] = {"sid1:5/5.1"};

    std::string js = clause_tree_to_json(t);
    ClauseTree r = clause_tree_from_json(js);

    REQUIRE(r.nodes.size() == 2);
    CHECK(r.standard_no == "JTC 5210-2018");
    CHECK(r.format_profile == "A_decimal");
    CHECK(r.nodes[0].child_ids.size() == 1);
    CHECK(r.nodes[1].is_leaf == true);
    CHECK(r.nodes[1].has_figure == true);
    REQUIRE(r.nodes[1].captions.size() == 1);
    CHECK(r.nodes[1].captions[0] == "图5.1 路基损坏示意");
    CHECK(r.nodes[1].page_start == 12);
    CHECK(r.page_clause_map.at(12).size() == 1);
}
```

- [ ] **Step 3: 登记到两个 vcxproj**

在 `rag2.0\rag2.0.vcxproj` 加：
```xml
<ClCompile Include="..\src\structure\clause_tree.cpp" />
```
```xml
<ClInclude Include="..\src\structure\clause_tree.h" />
```
在 `rag2.0.tests\rag2.0.tests.vcxproj` 的 `<ClCompile>` 段加：
```xml
<ClCompile Include="..\src\structure\clause_tree.cpp" />
<ClCompile Include="..\tests\test_clause_tree.cpp" />
```

- [ ] **Step 4: 跑测试确认失败（还没实现 .cpp）**

```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
Expected: 链接错误 `unresolved external symbol clause_tree_to_json`（因为 .cpp 还没写）。

- [ ] **Step 5: 写实现**

写 `src/structure/clause_tree.cpp`：

```cpp
#include "structure/clause_tree.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <spdlog/spdlog.h>

using nlohmann::json;

std::string clause_tree_to_json(const ClauseTree& t) {
    json j;
    j["standard_id"] = t.standard_id;
    j["standard_no"] = t.standard_no;
    j["format_profile"] = t.format_profile;
    j["schema_version"] = t.schema_version;
    j["nodes"] = json::array();
    for (const auto& n : t.nodes) {
        j["nodes"].push_back({
            {"node_id", n.node_id}, {"level", n.level}, {"number", n.number},
            {"title", n.title}, {"text", n.text},
            {"page_start", n.page_start}, {"page_end", n.page_end},
            {"parent_id", n.parent_id}, {"child_ids", n.child_ids},
            {"is_leaf", n.is_leaf}, {"has_table", n.has_table},
            {"has_figure", n.has_figure}, {"captions", n.captions},
            {"suspect", n.suspect}
        });
    }
    j["page_clause_map"] = json::object();
    for (const auto& kv : t.page_clause_map)
        j["page_clause_map"][std::to_string(kv.first)] = kv.second;
    return j.dump(2);
}

ClauseTree clause_tree_from_json(const std::string& json_text) {
    auto j = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    ClauseTree t;
    if (!j.is_object()) return t;
    t.standard_id = j.value("standard_id", "");
    t.standard_no = j.value("standard_no", "");
    t.format_profile = j.value("format_profile", "");
    t.schema_version = j.value("schema_version", 1);
    if (j.contains("nodes") && j["nodes"].is_array())
        for (auto& e : j["nodes"]) {
            TreeNode n;
            n.node_id = e.value("node_id", "");
            n.level = e.value("level", 0);
            n.number = e.value("number", "");
            n.title = e.value("title", "");
            n.text = e.value("text", "");
            n.page_start = e.value("page_start", 0);
            n.page_end = e.value("page_end", 0);
            n.parent_id = e.value("parent_id", "");
            if (e.contains("child_ids") && e["child_ids"].is_array())
                for (auto& c : e["child_ids"]) n.child_ids.push_back(c.get<std::string>());
            n.is_leaf = e.value("is_leaf", false);
            n.has_table = e.value("has_table", false);
            n.has_figure = e.value("has_figure", false);
            if (e.contains("captions") && e["captions"].is_array())
                for (auto& c : e["captions"]) n.captions.push_back(c.get<std::string>());
            n.suspect = e.value("suspect", "");
            t.nodes.push_back(std::move(n));
        }
    if (j.contains("page_clause_map") && j["page_clause_map"].is_object())
        for (auto it = j["page_clause_map"].begin(); it != j["page_clause_map"].end(); ++it) {
            int page = std::stoi(it.key());
            for (auto& id : it.value()) t.page_clause_map[page].push_back(id.get<std::string>());
        }
    return t;
}

void write_tree_cache(const std::string& cache_path, const ClauseTree& t) {
    std::filesystem::path p(cache_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(cache_path, std::ios::binary); // binary：避免 Windows CRLF 改写 UTF-8
    if (!f) { spdlog::warn("tree cache 写入失败: {}", cache_path); return; }
    f << clause_tree_to_json(t);
}
```

- [ ] **Step 6: 跑测试确认通过**

```powershell
& "D:\vs2022\...\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="clause_tree JSON 往返不丢字段"
```
Expected: PASS。

- [ ] **Step 7: 提交**

```powershell
git add src/structure/clause_tree.h src/structure/clause_tree.cpp tests/test_clause_tree.cpp rag2.0/rag2.0.vcxproj rag2.0.tests/rag2.0.tests.vcxproj
git commit -m "feat(m2c1): clause tree data model + JSON round-trip"
```

---

## Task 2: 编号→层级的规则表（format_profile）

**大白话:** 这一块只干一件事：给它一个编号，告诉你它是第几级。普通规范直接数点；合订本里同一个号要看"在不在某个试验里头"。还要能认出试验号 `T 0306—1994`。

**Files:**
- Create: `src/structure/format_profile.h`、`src/structure/format_profile.cpp`
- Test: `tests/test_format_profile.cpp`

- [ ] **Step 1: 写头文件**

`src/structure/format_profile.h`：

```cpp
#pragma once
#include <string>

enum class FormatProfile { A_decimal, B_testno };

// 数十进制编号有几个点: "5"->0, "5.1"->1, "5.1.1"->2。非十进制(如试验号/空)返回 -1。
int decimal_depth(const std::string& number);

// 是否试验号: 形如 "T 0306—1994" / "T0301-2024"(允许空格, 中划线为 - 或 — 或 –)。
bool is_test_number(const std::string& s);

// 把一个号翻译成抽象层级(1 起)。返回 0 表示不是结构号(不建节点)。
//  - A_decimal: "5"->1, "5.1"->2, "5.1.1"->3; "N.0.K"(中段为0,占位)折叠少一级 -> "1.0.1"->2
//  - B_testno : 试验号->2; 根作用域(in_test_scope=false)同 A; 试验内(true): "2"->3,"2.1"->4,"2.1.5"->5
//                tree_builder 会把深于 retrieval_depth 的编号并入当前检索节点正文。
int level_for(FormatProfile profile, const std::string& number, bool in_test_scope);

// 该格式预期检索深度(A=3, B=3)。深于该深度的编号并入当前检索节点正文。
int retrieval_depth(FormatProfile profile);

FormatProfile profile_from_string(const std::string& s);
std::string   profile_to_string(FormatProfile p);
```

- [ ] **Step 2: 写失败测试**

`tests/test_format_profile.cpp`：

```cpp
#include <doctest/doctest.h>
#include "structure/format_profile.h"

TEST_CASE("decimal_depth 数点") {
    CHECK(decimal_depth("5") == 0);
    CHECK(decimal_depth("5.1") == 1);
    CHECK(decimal_depth("5.1.1") == 2);
    CHECK(decimal_depth("T 0306—1994") == -1);
    CHECK(decimal_depth("") == -1);
}

TEST_CASE("is_test_number 认试验号各种写法") {
    CHECK(is_test_number("T 0306—1994"));   // 空格 + 全角破折号
    CHECK(is_test_number("T0301-2024"));    // 无空格 + ASCII 连字符
    CHECK(is_test_number("T 0302–2024"));   // en dash
    CHECK_FALSE(is_test_number("5.1.1"));
    CHECK_FALSE(is_test_number("表4.0.1"));
}

TEST_CASE("level_for A_decimal: 普通十进制 + N.0.K 折叠") {
    auto A = FormatProfile::A_decimal;
    CHECK(level_for(A, "5", false) == 1);
    CHECK(level_for(A, "5.1", false) == 2);
    CHECK(level_for(A, "5.1.1", false) == 3);
    CHECK(level_for(A, "1.0.1", false) == 2);   // 折叠占位节
    CHECK(level_for(A, "2.0.4", false) == 2);
    CHECK(level_for(A, "T0306-2024", false) == 0); // 非结构号
}

TEST_CASE("level_for B_testno: 试验号 + 作用域内重启编号") {
    auto B = FormatProfile::B_testno;
    CHECK(level_for(B, "T 0306—1994", false) == 2);  // 试验号是 2 级
    CHECK(level_for(B, "4", false) == 1);            // 根的章
    CHECK(level_for(B, "2.1", false) == 2);          // 根下真实小节(如术语章)
    CHECK(level_for(B, "2", true) == 3);             // 试验内的节
    CHECK(level_for(B, "2.1", true) == 4);           // 试验内细项, 建树时并入 L3
    CHECK(level_for(B, "2.1.5", true) == 5);         // 更细项, 建树时并入 L3
}

TEST_CASE("retrieval_depth") {
    CHECK(retrieval_depth(FormatProfile::A_decimal) == 3);
    CHECK(retrieval_depth(FormatProfile::B_testno) == 3);
}
```

- [ ] **Step 3: 登记到两个 vcxproj**

主工程加 `format_profile.cpp`(ClCompile)、`format_profile.h`(ClInclude)；测试工程加 `..\src\structure\format_profile.cpp` 和 `..\tests\test_format_profile.cpp`。

- [ ] **Step 4: 跑测试确认失败**

```powershell
& "D:\vs2022\...\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
Expected: 链接错误 `unresolved external symbol level_for` 等。

- [ ] **Step 5: 写实现**

`src/structure/format_profile.cpp`：

```cpp
#include "structure/format_profile.h"
#include <regex>

int decimal_depth(const std::string& number) {
    if (number.empty()) return -1;
    // 必须形如 数字(.数字)*，允许尾部 -后缀
    static const std::regex re(R"(^\d+(?:\.\d+)*(?:-[0-9A-Za-z]+)?$)");
    if (!std::regex_match(number, re)) return -1;
    int dots = 0;
    for (char c : number) { if (c == '.') ++dots; if (c == '-') break; }
    return dots;
}

bool is_test_number(const std::string& s) {
    // T + 可选空格 + 4位 + 连字符(ASCII - / em — E2 80 94 / en – E2 80 93) + 4位
    // 用宽松正则先匹配 "T\s*\d{4}"，再确认后面有连字符 + 4 位(连字符可能是多字节，单独判)
    static const std::regex head(R"(^T\s*\d{4})");
    if (!std::regex_search(s, head)) return false;
    // 找到 4 位数字后，跳过它，看接下来是否为连字符 + 4 位数字
    static const std::regex full(R"(^T\s*\d{4}\s*[-]\s*\d{4})");          // ASCII -
    if (std::regex_search(s, full)) return true;
    // 处理 UTF-8 多字节破折号: 把 — (E2 80 94) 和 – (E2 80 93) 替换成 ASCII - 再判
    std::string norm; norm.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (i + 2 < s.size() && (unsigned char)s[i]==0xE2 && (unsigned char)s[i+1]==0x80 &&
            ((unsigned char)s[i+2]==0x94 || (unsigned char)s[i+2]==0x93)) {
            norm += '-'; i += 3;
        } else { norm += s[i]; ++i; }
    }
    return std::regex_search(norm, full);
}

static bool is_n0k(const std::string& number) {
    // 形如 N.0.K（中段为 0 的占位节）
    static const std::regex re(R"(^\d+\.0\.\d+(?:-[0-9A-Za-z]+)?$)");
    return std::regex_match(number, re);
}

int level_for(FormatProfile profile, const std::string& number, bool in_test_scope) {
    if (profile == FormatProfile::B_testno && is_test_number(number)) return 2;
    int d = decimal_depth(number);
    if (d < 0) return 0;               // 不是结构号
    if (profile == FormatProfile::B_testno && in_test_scope) return d + 3;
    // A_decimal，或 B 的根作用域：与 A 同
    if (is_n0k(number)) return d;       // 折叠占位节: "1.0.1" d=2 -> 2
    return d + 1;                       // "5"->1, "5.1"->2, "5.1.1"->3
}

int retrieval_depth(FormatProfile /*profile*/) {
    return 3; // A_decimal 与 B_testno 当前都以 L3 作为默认检索深度
}

FormatProfile profile_from_string(const std::string& s) {
    return s == "B_testno" ? FormatProfile::B_testno : FormatProfile::A_decimal;
}
std::string profile_to_string(FormatProfile p) {
    return p == FormatProfile::B_testno ? "B_testno" : "A_decimal";
}
```

- [ ] **Step 6: 跑测试确认通过**

```powershell
& "D:\vs2022\...\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe -tc="decimal_depth 数点" -tc="is_test_number 认试验号各种写法" -tc="level_for A_decimal: 普通十进制 + N.0.K 折叠" -tc="level_for B_testno: 试验号 + 作用域内重启编号" -tc="retrieval_depth"
```
Expected: 全 PASS。

- [ ] **Step 7: 提交**

```powershell
git add src/structure/format_profile.* tests/test_format_profile.cpp rag2.0/rag2.0.vcxproj rag2.0.tests/rag2.0.tests.vcxproj
git commit -m "feat(m2c1): format profile — number-to-level mapping for A/B"
```

---

## Task 3: 看目录判断格式（toc_parser）

**大白话:** 拿到一本规范先翻目录。目录里出现 `T 0306` 这种 → 合订本(B)；只有 `5.1` 这种十进制 → 普通规范(A)。目录糊了/没有 → 返回"没认出来"，让上层走兜底（扫正文：正文里出现试验号就 B，否则 A）。

**Files:**
- Create: `src/structure/toc_parser.h`、`src/structure/toc_parser.cpp`
- Test: `tests/test_toc_parser.cpp`

- [ ] **Step 1: 写头文件**

`src/structure/toc_parser.h`：

```cpp
#pragma once
#include "parse/parser.h"            // ParsedDoc
#include "structure/format_profile.h"

struct TocResult {
    bool detected = false;           // 是否从目录认出了格式
    FormatProfile profile = FormatProfile::A_decimal;
};

// 看 region==Toc 的元素文本判断格式; 认不出返回 detected=false。
TocResult detect_format_from_toc(const ParsedDoc& doc);

// 兜底: 扫所有元素的 clause_no/text, 出现试验号则 B, 否则 A。
FormatProfile detect_format_from_body(const ParsedDoc& doc);
```

- [ ] **Step 2: 写失败测试**

`tests/test_toc_parser.cpp`：

```cpp
#include <doctest/doctest.h>
#include "structure/toc_parser.h"

static ParseElement mk(Region r, const std::string& text) {
    ParseElement e; e.region = r; e.text = text; return e;
}

TEST_CASE("目录里有十进制条目 → A_decimal") {
    ParsedDoc d;
    d.elements.push_back(mk(Region::Toc, "目次"));
    d.elements.push_back(mk(Region::Toc,
        "1总则1\n2术语2\n5公路损坏分类6\n5.1路基……6\n5.2沥青路面……7"));
    TocResult r = detect_format_from_toc(d);
    CHECK(r.detected);
    CHECK(r.profile == FormatProfile::A_decimal);
}

TEST_CASE("目录里有试验号 → B_testno") {
    ParsedDoc d;
    d.elements.push_back(mk(Region::Toc,
        "3集料取样方法7\nT 0301—2024 集料取样法……7\n4粗集料试验20\nT 0302—2024 粗集料的筛分试验……20"));
    TocResult r = detect_format_from_toc(d);
    CHECK(r.detected);
    CHECK(r.profile == FormatProfile::B_testno);
}

TEST_CASE("没有目录元素 → detected=false") {
    ParsedDoc d;
    d.elements.push_back(mk(Region::Body, "5.1.1 正文"));
    TocResult r = detect_format_from_toc(d);
    CHECK_FALSE(r.detected);
}

TEST_CASE("兜底: 正文出现试验号 → B, 否则 A") {
    ParsedDoc b;
    b.elements.push_back(mk(Region::Body, "T 0306—1994 粗集料含水率快速试验"));
    CHECK(detect_format_from_body(b) == FormatProfile::B_testno);

    ParsedDoc a;
    a.elements.push_back(mk(Region::Body, "5.1.1 路缘石缺损……"));
    CHECK(detect_format_from_body(a) == FormatProfile::A_decimal);
}
```

- [ ] **Step 3: 登记到两个 vcxproj**

主工程加 `toc_parser.cpp/.h`；测试工程加 `..\src\structure\toc_parser.cpp` 和 `..\tests\test_toc_parser.cpp`。

- [ ] **Step 4: 跑测试确认失败**

Run: 构建。Expected: 链接错误 `unresolved external symbol detect_format_from_toc`。

- [ ] **Step 5: 写实现**

`src/structure/toc_parser.cpp`：

```cpp
#include "structure/toc_parser.h"
#include <regex>

// 文本里是否含试验号子串(用 is_test_number 的宽松版: 在任意位置搜 T....-/—....)
static bool contains_test_number(const std::string& s) {
    // 归一化破折号后用统一正则在任意位置搜
    std::string norm; norm.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (i + 2 < s.size() && (unsigned char)s[i]==0xE2 && (unsigned char)s[i+1]==0x80 &&
            ((unsigned char)s[i+2]==0x94 || (unsigned char)s[i+2]==0x93)) { norm += '-'; i += 3; }
        else { norm += s[i]; ++i; }
    }
    static const std::regex re(R"(T\s*\d{4}\s*-\s*\d{4})");
    return std::regex_search(norm, re);
}

static bool contains_decimal_entry(const std::string& s) {
    static const std::regex re(R"(\d+\.\d+)");
    return std::regex_search(s, re);
}

TocResult detect_format_from_toc(const ParsedDoc& doc) {
    std::string toc;
    for (const auto& e : doc.elements)
        if (e.region == Region::Toc) { toc += e.text; toc += "\n"; }
    TocResult r;
    if (toc.empty()) { r.detected = false; return r; }
    if (contains_test_number(toc)) { r.detected = true; r.profile = FormatProfile::B_testno; return r; }
    if (contains_decimal_entry(toc)) { r.detected = true; r.profile = FormatProfile::A_decimal; return r; }
    r.detected = false; return r;
}

FormatProfile detect_format_from_body(const ParsedDoc& doc) {
    for (const auto& e : doc.elements) {
        if (contains_test_number(e.text) || contains_test_number(e.title) || contains_test_number(e.clause_no))
            return FormatProfile::B_testno;
    }
    return FormatProfile::A_decimal;
}
```

- [ ] **Step 6: 跑测试确认通过**

Run: 构建 + `rag2.0.tests.exe -tc="目录里有十进制条目 → A_decimal" -tc="目录里有试验号 → B_testno" -tc="没有目录元素 → detected=false" -tc="兜底: 正文出现试验号 → B, 否则 A"`
Expected: 全 PASS。

- [ ] **Step 7: 提交**

```powershell
git add src/structure/toc_parser.* tests/test_toc_parser.cpp rag2.0/rag2.0.vcxproj rag2.0.tests/rag2.0.tests.vcxproj
git commit -m "feat(m2c1): TOC-driven format detection + body fallback"
```

---

## Task 4: 区域分组（region_segmenter）

**大白话:** 把元素按区域分堆——正文一堆、条文说明一堆、附录一堆，每堆将来各建一棵树（这样正文和条文说明的相同条款号不会撞）。前置内容和目录这两堆跳过不建树。v1 先用 M2b 已经打好的 `region` 标签分组，接口归我们所有；等以后有了合订本样本再加固内部判定。

**Files:**
- Create: `src/structure/region_segmenter.h`、`src/structure/region_segmenter.cpp`
- Test: `tests/test_region_segmenter.cpp`

- [ ] **Step 1: 写头文件**

`src/structure/region_segmenter.h`：

```cpp
#pragma once
#include <vector>
#include "parse/parser.h"

// 一个内容区一组, 内含指向 doc.elements 的指针(按原阅读顺序)。
struct RegionGroup {
    Region region;
    std::vector<const ParseElement*> elements;
};

// 把 doc.elements 按区域切组。只产出会建树的区: Body / Explanation / Appendix。
// 跳过 FrontMatter / Toc。保持元素原始顺序。
std::vector<RegionGroup> segment_regions(const ParsedDoc& doc);
```

- [ ] **Step 2: 写失败测试**

`tests/test_region_segmenter.cpp`：

```cpp
#include <doctest/doctest.h>
#include "structure/region_segmenter.h"

static ParseElement el(Region r, const std::string& t) {
    ParseElement e; e.region = r; e.text = t; return e;
}

TEST_CASE("segment_regions 跳过前置/目录, 正文与条文说明分开成组") {
    ParsedDoc d;
    d.elements = {
        el(Region::FrontMatter, "封面"),
        el(Region::Toc, "目次"),
        el(Region::Body, "5.1.1 正文一"),
        el(Region::Body, "5.1.2 正文二"),
        el(Region::Explanation, "5.1.1 条文说明"),
        el(Region::Appendix, "附录 A"),
    };
    auto groups = segment_regions(d);
    REQUIRE(groups.size() == 3);                 // Body / Explanation / Appendix
    CHECK(groups[0].region == Region::Body);
    CHECK(groups[0].elements.size() == 2);
    CHECK(groups[1].region == Region::Explanation);
    CHECK(groups[2].region == Region::Appendix);
}
```

- [ ] **Step 3: 登记到两个 vcxproj**

主工程加 `region_segmenter.cpp/.h`；测试工程加 `..\src\structure\region_segmenter.cpp` 和 `..\tests\test_region_segmenter.cpp`。

- [ ] **Step 4: 跑测试确认失败**

Run: 构建。Expected: `unresolved external symbol segment_regions`。

- [ ] **Step 5: 写实现**

`src/structure/region_segmenter.cpp`：

```cpp
#include "structure/region_segmenter.h"

std::vector<RegionGroup> segment_regions(const ParsedDoc& doc) {
    std::vector<RegionGroup> groups;
    auto want = [](Region r) {
        return r == Region::Body || r == Region::Explanation || r == Region::Appendix;
    };
    for (const auto& e : doc.elements) {
        if (!want(e.region)) continue;
        if (groups.empty() || groups.back().region != e.region)
            groups.push_back(RegionGroup{e.region, {}});
        groups.back().elements.push_back(&e);
    }
    return groups;
}
```

> 注意: 该实现把"同一区但被别区打断"的情况会切成多组（如 Body…Explanation…Body 会出两个 Body 组）。v1 可接受——每组独立建树后 node_id 仍唯一。若实测出现交错，留待合订本 fixture 阶段处理。

- [ ] **Step 6: 跑测试确认通过**

Run: 构建 + `rag2.0.tests.exe -tc="segment_regions 跳过前置/目录, 正文与条文说明分开成组"`
Expected: PASS。

- [ ] **Step 7: 提交**

```powershell
git add src/structure/region_segmenter.* tests/test_region_segmenter.cpp rag2.0/rag2.0.vcxproj rag2.0.tests/rag2.0.tests.vcxproj
git commit -m "feat(m2c1): region segmenter (group elements by content region)"
```

---

## Task 5: 建树核心 — 定级 + 搭上下级 + 起名字（格式 A）

**大白话:** 这是核心第一刀。走一遍正文元素，用上一步的规则给每个带编号的元素定级，用一个栈把它们搭成上下级（弹到比自己浅的当父亲），给每个节点起一个带完整路径的唯一名字。先只管"普通规范(A)"，先不管正文聚合/页码/合订本。

**Files:**
- Create: `src/structure/tree_builder.h`、`src/structure/tree_builder.cpp`
- Test: `tests/test_tree_builder.cpp`

- [ ] **Step 1: 写头文件**

`src/structure/tree_builder.h`：

```cpp
#pragma once
#include "parse/parser.h"
#include "structure/clause_tree.h"

// 把一份解析缓存建成条款树。standard_id 用于拼 node_id 前缀。
ClauseTree build_clause_tree(const ParsedDoc& doc, const std::string& standard_id);
```

- [ ] **Step 2: 写失败测试（格式 A 的层级与父子）**

`tests/test_tree_builder.cpp`：

```cpp
#include <doctest/doctest.h>
#include "structure/tree_builder.h"

// 造一个带号的正文元素
static ParseElement body(const std::string& no, const std::string& text, int page = 1) {
    ParseElement e; e.region = Region::Body; e.clause_no = no; e.text = text; e.page_no = page;
    return e;
}
// 按 node_id 找节点
static const TreeNode* find(const ClauseTree& t, const std::string& id) {
    for (const auto& n : t.nodes) if (n.node_id == id) return &n;
    return nullptr;
}

TEST_CASE("格式A: 章/节/条 三级父子 + 路径式 node_id") {
    ParsedDoc d; d.standard_no = "JTC 5210";
    d.elements = {
        body("5", "公路损坏分类"),
        body("5.1", "路基"),
        body("5.1.1", "5.1.1 路肩损坏……"),
        body("5.1.2", "5.1.2 边坡坍塌……"),
    };
    ClauseTree t = build_clause_tree(d, "sid");
    CHECK(t.format_profile == "A_decimal");

    const TreeNode* c5   = find(t, "sid:5");
    const TreeNode* c51  = find(t, "sid:5/5.1");
    const TreeNode* c511 = find(t, "sid:5/5.1/5.1.1");
    REQUIRE(c5);   REQUIRE(c51);   REQUIRE(c511);
    CHECK(c5->level == 1);
    CHECK(c51->level == 2);
    CHECK(c511->level == 3);
    CHECK(c51->parent_id == "sid:5");
    CHECK(c511->parent_id == "sid:5/5.1");
    CHECK(c5->child_ids.size() == 1);     // 5.1
    CHECK(c51->child_ids.size() == 2);    // 5.1.1, 5.1.2
}

TEST_CASE("格式A: 总则 N.0.K 折叠为二级、直接挂在章下") {
    ParsedDoc d;
    d.elements = {
        body("1", "总则"),
        body("1.0.1", "1.0.1 为客观评定……"),
        body("1.0.2", "1.0.2 适用于各等级公路。"),
    };
    ClauseTree t = build_clause_tree(d, "sid");
    const TreeNode* c1    = find(t, "sid:1");
    const TreeNode* c101  = find(t, "sid:1/1.0.1");
    REQUIRE(c1); REQUIRE(c101);
    CHECK(c101->level == 2);
    CHECK(c101->parent_id == "sid:1");    // 直接挂章下, 不造虚拟 1.0
}
```

- [ ] **Step 3: 登记到两个 vcxproj**

主工程加 `tree_builder.cpp/.h`；测试工程加 `..\src\structure\tree_builder.cpp` 和 `..\tests\test_tree_builder.cpp`。

- [ ] **Step 4: 跑测试确认失败**

Run: 构建。Expected: `unresolved external symbol build_clause_tree`。

- [ ] **Step 5: 写实现（本任务只到"定级+父子+名字"，后续任务在此文件上加功能）**

`src/structure/tree_builder.cpp`：

```cpp
#include "structure/tree_builder.h"
#include "structure/format_profile.h"
#include "structure/toc_parser.h"
#include "structure/region_segmenter.h"
#include <vector>

// node_id 用：去空格 + 把 UTF-8 破折号(— E2 80 94 / – E2 80 93)归一化成 ASCII '-'。
// 例: "T 0306—1994" -> "T0306-1994"
static std::string sanitize(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == ' ') { ++i; continue; }
        if (i + 2 < s.size() && (unsigned char)s[i]==0xE2 && (unsigned char)s[i+1]==0x80 &&
            ((unsigned char)s[i+2]==0x94 || (unsigned char)s[i+2]==0x93)) { o += '-'; i += 3; continue; }
        o += s[i]; ++i;
    }
    return o;
}

namespace {
struct Builder {
    ClauseTree& t;
    const std::string& sid;
    FormatProfile profile;

    // 在树里新增一个节点，返回其在 t.nodes 里的下标
    int add(int parent_idx, int level, const std::string& number, const std::string& text) {
        TreeNode n;
        n.level = level;
        n.number = number;
        n.text = text;
        if (parent_idx >= 0) {
            n.parent_id = t.nodes[parent_idx].node_id;
            n.node_id = n.parent_id + "/" + sanitize(number);   // 父路径 + "/" + 本号
        } else {
            n.parent_id = "";
            n.node_id = sid + ":" + sanitize(number);           // 根: "<sid>:<号>"
        }
        int idx = (int)t.nodes.size();
        t.nodes.push_back(std::move(n));
        if (parent_idx >= 0) t.nodes[parent_idx].child_ids.push_back(t.nodes[idx].node_id);
        return idx;
    }
};
}

ClauseTree build_clause_tree(const ParsedDoc& doc, const std::string& standard_id) {
    ClauseTree t;
    t.standard_id = standard_id;
    t.standard_no = doc.standard_no;

    // 1) 定格式: 目录优先, 认不出走兜底
    TocResult toc = detect_format_from_toc(doc);
    FormatProfile profile = toc.detected ? toc.profile : detect_format_from_body(doc);
    t.format_profile = profile_to_string(profile);

    Builder b{t, standard_id, profile};

    // 2) 每个内容区各建一棵(本任务先只处理定级+父子)
    for (const auto& group : segment_regions(doc)) {
        // 作用域栈: 每项 = {nodes 下标, 层级}
        std::vector<std::pair<int,int>> stack;  // (idx, level)
        for (const ParseElement* e : group.elements) {
            std::string number = e->clause_no;
            if (number.empty()) continue;                 // 无号: 本任务先跳过(Task 6 处理)
            int lvl = level_for(profile, number, /*in_test_scope=*/false);
            if (lvl == 0) continue;                        // 非结构号

            // 弹栈到父: 父的 level < lvl
            while (!stack.empty() && stack.back().second >= lvl) stack.pop_back();
            int parent_idx = stack.empty() ? -1 : stack.back().first;
            int idx = b.add(parent_idx, lvl, number, e->text);
            stack.push_back({idx, lvl});
        }
    }
    return t;
}
```

- [ ] **Step 6: 跑测试确认通过**

Run: 构建 + `rag2.0.tests.exe -tc="格式A: 章/节/条 三级父子 + 路径式 node_id" -tc="格式A: 总则 N.0.K 折叠为二级、直接挂在章下"`
Expected: 全 PASS。

- [ ] **Step 7: 提交**

```powershell
git add src/structure/tree_builder.* tests/test_tree_builder.cpp rag2.0/rag2.0.vcxproj rag2.0.tests/rag2.0.tests.vcxproj
git commit -m "feat(m2c1): tree builder core — levels, parent/child, path node_id (format A)"
```

---

## Task 6: 正文聚合 + 挂载图表题 + 标叶子

**大白话:** 上一步只建了带编号的骨架。这一步把"没编号的续行/列项(1轻度/2中度…)"并进它前面那条；遇到图表题(图/表 开头)不建节点，但要挂到当前条的 `captions`，作为后续 `retrieval_text` 和附件回显的素材；遇到表格给当前条打个"含表格"标记；最后把"底下没有子节点"的那些条标成叶子（检索单元），并把编号从正文开头削掉。

**Files:**
- Modify: `src/structure/tree_builder.cpp`
- Test: `tests/test_tree_builder.cpp`(追加用例)

- [ ] **Step 1: 追加失败测试**

在 `tests/test_tree_builder.cpp` 末尾追加：

```cpp
// 无号正文元素
static ParseElement plain(const std::string& text, int page = 1) {
    ParseElement e; e.region = Region::Body; e.text = text; e.page_no = page; return e;
}
static ParseElement caption(const std::string& text, int page = 1) {
    ParseElement e; e.region = Region::Body; e.text = text; e.is_caption = true; e.page_no = page; return e;
}

TEST_CASE("聚合: 无号列项并入前一条, 图表题挂载, 终端条标叶子并削号") {
    ParsedDoc d;
    d.elements = {
        body("5.1", "路基"),
        body("5.1.2", "5.1.2 边坡坍塌应为……损坏程度应按下列标准判断："),
        plain("1轻度应为边坡坍塌长度小于5m。"),
        plain("2中度应为边坡坍塌长度在5～10m之间。"),
        caption("图5.1 示意"),
        body("5.1.3", "5.1.3 水毁冲沟……"),
    };
    ClauseTree t = build_clause_tree(d, "sid");
    const TreeNode* c512 = find(t, "sid:5/5.1/5.1.2");
    const TreeNode* c51  = find(t, "sid:5/5.1");
    REQUIRE(c512);
    // 列项并入 5.1.2
    CHECK(c512->text.find("1轻度") != std::string::npos);
    CHECK(c512->text.find("2中度") != std::string::npos);
    // 图表题没建成节点, 但挂到当前条
    CHECK(find(t, "sid:5/5.1/图5.1") == nullptr);
    REQUIRE(c512->captions.size() == 1);
    CHECK(c512->captions[0] == "图5.1 示意");
    CHECK(c512->has_figure == true);
    // 5.1.2 是终端 → 叶子; 5.1(有子)非叶子
    CHECK(c512->is_leaf == true);
    CHECK(c51->is_leaf == false);
    // 正文开头的号被削掉
    CHECK(c512->text.rfind("5.1.2", 0) != 0);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: 构建 + `rag2.0.tests.exe -tc="聚合: 无号列项并入前一条, 图表题挂载, 终端条标叶子并削号"`
Expected: FAIL（列项没并入、caption 未挂载、号没削、is_leaf 未设）。

- [ ] **Step 3: 改实现**

在 `tree_builder.cpp` 顶部辅助函数区加（`sanitize` 旁边）：

```cpp
// 文本是否像图/表题: 以 图/表/续表/附图/附表 起。UTF-8 字节判断。
static bool looks_like_caption(const ParseElement* e) {
    if (e->is_caption) return true;
    const std::string& s = e->text;
    auto starts = [&](std::initializer_list<unsigned char> bytes) {
        if (s.size() < bytes.size()) return false;
        size_t i = 0; for (unsigned char b : bytes) if ((unsigned char)s[i++] != b) return false;
        return true;
    };
    // 图 E5 9B BE | 表 E8 A1 A8
    return starts({0xE5,0x9B,0xBE}) || starts({0xE8,0xA1,0xA8});
}

static bool looks_like_figure_caption(const ParseElement* e) {
    const std::string& s = e->text;
    return e->raw_label == "figure_title" || e->raw_label == "chart_title" ||
           s.rfind("\xE5\x9B\xBE", 0) == 0 ||       // 图
           s.rfind("\xE9\x99\x84\xE5\x9B\xBE", 0) == 0; // 附图
}

static void attach_caption(ClauseTree& t, int current_leaf, const ParseElement* e) {
    if (current_leaf < 0) return;       // 孤儿 caption: 暂不建节点, 后续 treecheck 可统计
    const std::string text = e->title.empty() ? e->text : e->title;
    if (!text.empty()) t.nodes[current_leaf].captions.push_back(text);
    if (looks_like_figure_caption(e)) t.nodes[current_leaf].has_figure = true;
}

// 若 text 以 number 开头, 削掉它(连同紧随的空白)
static std::string strip_leading_number(const std::string& text, const std::string& number) {
    if (text.rfind(number, 0) == 0) {
        size_t i = number.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        // 全角空格 U+3000 = E3 80 80
        while (i + 2 < text.size() && (unsigned char)text[i]==0xE3 &&
               (unsigned char)text[i+1]==0x80 && (unsigned char)text[i+2]==0x80) i += 3;
        return text.substr(i);
    }
    return text;
}
```

在 `add()` 创建节点处，把 `n.text` 改为削号后的正文：
```cpp
n.text = strip_leading_number(text, number);
```

在区内循环里，处理"无号 / 图表题 / 表格"，并记住"当前条"（用于并入正文）。把 Task 5 的循环体替换为：

```cpp
std::vector<std::pair<int,int>> stack;  // (idx, level)
int current_leaf = -1;                  // 最近建立的节点下标(续行并入它)
for (const ParseElement* e : group.elements) {
    // a) 图表题: 不建节点, 挂到当前条
    if (looks_like_caption(e)) {
        attach_caption(t, current_leaf, e);
        continue;
    }
    // b) 表格/公式: 给当前条打标记
    if (e->type == ElementType::Table || e->type == ElementType::Formula) {
        if (current_leaf >= 0 && e->type == ElementType::Table)
            t.nodes[current_leaf].has_table = true;
        continue;
    }
    std::string number = e->clause_no;
    // c) 无号正文: 并入当前条
    if (number.empty()) {
        if (current_leaf >= 0) {
            if (!t.nodes[current_leaf].text.empty()) t.nodes[current_leaf].text += "\n";
            t.nodes[current_leaf].text += e->text;
        }
        continue;
    }
    int lvl = level_for(profile, number, /*in_test_scope=*/false);
    if (lvl == 0) {                       // 非结构号: 当正文并入
        if (current_leaf >= 0) {
            if (!t.nodes[current_leaf].text.empty()) t.nodes[current_leaf].text += "\n";
            t.nodes[current_leaf].text += e->text;
        }
        continue;
    }
    while (!stack.empty() && stack.back().second >= lvl) stack.pop_back();
    int parent_idx = stack.empty() ? -1 : stack.back().first;
    int idx = b.add(parent_idx, lvl, number, e->text);
    stack.push_back({idx, lvl});
    current_leaf = idx;
}
```

在 `build_clause_tree` 的 `return t;` 之前，加"标叶子"：
```cpp
// 标叶子: 深于检索层级的编号已被折叠后, 没有子节点的就是检索单元
for (auto& n : t.nodes) n.is_leaf = n.child_ids.empty();
```

- [ ] **Step 4: 跑测试确认通过（含之前用例不回归）**

Run: 构建 + `rag2.0.tests.exe -tc="聚合: 无号列项并入前一条, 图表题挂载, 终端条标叶子并削号" -tc="格式A: 章/节/条 三级父子 + 路径式 node_id"`
Expected: 全 PASS。

- [ ] **Step 5: 提交**

```powershell
git add src/structure/tree_builder.cpp tests/test_tree_builder.cpp
git commit -m "feat(m2c1): aggregate body/list-items, attach captions, mark leaf nodes"
```

---

## Task 7: 页码范围 + page_clause_map

**大白话:** 给每个节点记下它从第几页到第几页（续行可能跨页，要把页尾拉大）；再建一张"第几页 → 这页有哪些叶子"的表，给将来定位/溯源用。

**Files:**
- Modify: `src/structure/tree_builder.cpp`
- Test: `tests/test_tree_builder.cpp`(追加)

- [ ] **Step 1: 追加失败测试**

```cpp
TEST_CASE("页码: 节点页范围 + page_clause_map 收录叶子") {
    ParsedDoc d;
    d.elements = {
        body("5.1", "路基", 12),
        body("5.1.6", "5.1.6 路基沉降……", 12),
        plain("判断：", 13),                         // 跨页续行 → 页尾拉到 13
        body("5.1.7", "5.1.7 排水不畅……", 13),
    };
    ClauseTree t = build_clause_tree(d, "sid");
    const TreeNode* c516 = find(t, "sid:5/5.1/5.1.6");
    REQUIRE(c516);
    CHECK(c516->page_start == 12);
    CHECK(c516->page_end == 13);                      // 续行把页尾拉大
    // 12 页有 5.1.6; 13 页有 5.1.6(续) 和 5.1.7
    CHECK(t.page_clause_map.count(12) == 1);
    bool has516_on12 = false;
    for (auto& id : t.page_clause_map.at(12)) if (id == "sid:5/5.1/5.1.6") has516_on12 = true;
    CHECK(has516_on12);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: 构建 + `rag2.0.tests.exe -tc="页码: 节点页范围 + page_clause_map 收录叶子"`
Expected: FAIL（page_start/end 为 0、map 为空）。

- [ ] **Step 3: 改实现**

在 `Builder::add` 里，创建时记起始页（给 add 增一个 page 参数）。把 `add` 签名与实现改为：

```cpp
int add(int parent_idx, int level, const std::string& number, const std::string& text, int page) {
    TreeNode n;
    n.level = level; n.number = number; n.text = strip_leading_number(text, number);
    if (parent_idx >= 0) {
        n.parent_id = t.nodes[parent_idx].node_id;
        n.node_id = n.parent_id + "/" + sanitize(number);
    } else {
        n.parent_id = "";
        n.node_id = sid + ":" + sanitize(number);
    }
    n.page_start = page; n.page_end = page;
    int idx = (int)t.nodes.size();
    t.nodes.push_back(std::move(n));
    if (parent_idx >= 0) t.nodes[parent_idx].child_ids.push_back(t.nodes[idx].node_id);
    return idx;
}
```

循环里建节点处传页号：`int idx = b.add(parent_idx, lvl, number, e->text, e->page_no);`

并入续行/表格时，把当前条页尾拉大：在"无号正文并入"分支里 `current_leaf >= 0` 内追加：
```cpp
if (e->page_no > t.nodes[current_leaf].page_end) t.nodes[current_leaf].page_end = e->page_no;
```
（表格分支若需要也可同样拉大，可选。）

在标叶子之后，加构建 page_clause_map：
```cpp
for (const auto& n : t.nodes) {
    if (!n.is_leaf) continue;
    for (int p = n.page_start; p <= n.page_end && p > 0; ++p)
        t.page_clause_map[p].push_back(n.node_id);
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: 构建 + `rag2.0.tests.exe -tc="页码: 节点页范围 + page_clause_map 收录叶子"`
Expected: PASS。再跑一遍全部 `tree_builder` 用例确认不回归。

- [ ] **Step 5: 提交**

```powershell
git add src/structure/tree_builder.cpp tests/test_tree_builder.cpp
git commit -m "feat(m2c1): node page ranges + page_clause_map"
```

---

## Task 8: 合订本支持（格式 B：试验号 + 内部重启编号）

**大白话:** 现在让建树器认合订本。看到 `T 0306—1994` 就建一个二级节点、并记住"我进了这个试验"；进去之后里头的 `1/2/3` 算三级并作为检索单元，`2.1/2.2` 等更细项只并入当前三级正文；遇到下一个 `T` 号或回到根的章，就切换/退出试验。靠路径式名字保证几十个试验里的 `2` 不撞。

**Files:**
- Modify: `src/structure/tree_builder.cpp`
- Test: `tests/test_tree_builder.cpp`(追加)

- [ ] **Step 1: 追加失败测试（合成一个迷你合订本）**

```cpp
static ParseElement testno(const std::string& no_in_text, int page = 7) {
    // 试验号常无 clause_no, 号在 text 里
    ParseElement e; e.region = Region::Body; e.text = no_in_text; e.page_no = page; return e;
}

TEST_CASE("格式B: 试验号建二级, 内部重启编号, 重号不撞") {
    ParsedDoc d;
    d.elements = {
        body("4", "粗集料试验", 20),                       // 章 L1
        testno("T 0302—2024 粗集料的筛分试验", 20),         // 试验号 L2
        body("2", "仪具与材料", 20),                        // 试验内节 L3
        body("2.1", "2.1 天平：感量……", 20),               // 试验内细项 L4, 并入 L3
        testno("T 0306—1994 粗集料含水率快速试验", 46),      // 另一个试验
        body("2", "仪具与材料", 46),                        // 又一个"2"
        body("2.1", "2.1 天平：感量不大于……", 46),          // 又一个"2.1", 并入另一个 L3
    };
    ClauseTree t = build_clause_tree(d, "sid");
    CHECK(t.format_profile == "B_testno");

    // 试验号节点 number = 抽出的试验号(不含后面的名字); sanitize 去空格+破折号归一化为 ASCII
    const TreeNode* t0302 = find(t, "sid:4/T0302-2024");
    // 路径式名字: 两个试验下的 2 不撞; 2.1 不单独建检索节点
    const TreeNode* a = find(t, "sid:4/T0302-2024/2");
    const TreeNode* b = find(t, "sid:4/T0306-1994/2");
    REQUIRE(t0302); REQUIRE(a); REQUIRE(b);
    CHECK(t0302->level == 2);
    CHECK(a->level == 3);
    CHECK(b->level == 3);
    CHECK(find(t, "sid:4/T0302-2024/2/2.1") == nullptr);
    CHECK(find(t, "sid:4/T0306-1994/2/2.1") == nullptr);
    CHECK(a->node_id != b->node_id);          // 不撞
    CHECK(a->is_leaf == true);
    CHECK(a->text.find("2.1 天平") != std::string::npos);
    CHECK(b->text.find("2.1 天平") != std::string::npos);
}
```

> 注：试验号节点的 `number` 取其文本里的试验号部分（`T 0302—2024`），`sanitize` 去空格后用于 node_id。本任务实现需从元素文本里抽出试验号当 number。

- [ ] **Step 2: 跑测试确认失败**

Run: 构建 + `rag2.0.tests.exe -tc="格式B: 试验号建二级, 内部重启编号, 重号不撞"`
Expected: FAIL。

- [ ] **Step 3: 改实现**

先在 `tree_builder.cpp` 顶部加 `#include <regex>` 与辅助函数"从文本抽试验号"（放在 `strip_leading_number` 旁）：

```cpp
#include <regex>
// 从一段文本里抽出试验号(归一化破折号为 -)，抽不到返回空。例 "T 0302—2024 粗集料..." -> "T 0302-2024"
static std::string extract_test_number(const std::string& s) {
    std::string norm; norm.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (i + 2 < s.size() && (unsigned char)s[i]==0xE2 && (unsigned char)s[i+1]==0x80 &&
            ((unsigned char)s[i+2]==0x94 || (unsigned char)s[i+2]==0x93)) { norm += '-'; i += 3; }
        else { norm += s[i]; ++i; }
    }
    static const std::regex re(R"(T\s*\d{4}\s*-\s*\d{4})");
    std::smatch m;
    if (std::regex_search(norm, m, re)) return m.str(0);
    return "";
}
```

然后把 `build_clause_tree` 里**整个区内 for 循环**替换为下面这版（完整、含试验作用域追踪；这是格式 A/B 通用的最终循环，A 时 `extract_test_number` 抽不到、`in_test_scope` 恒 false，行为同前）：

```cpp
std::vector<std::pair<int,int>> stack;  // (idx, level)
int current_leaf = -1;
bool in_test_scope = false;             // 仅 B 用：是否在某试验内部
for (const ParseElement* e : group.elements) {
    // a) 图表题: 不建节点, 挂到当前条
    if (looks_like_caption(e)) {
        attach_caption(t, current_leaf, e);
        continue;
    }
    // b) 表格/公式: 给当前条打标记
    if (e->type == ElementType::Table || e->type == ElementType::Formula) {
        if (current_leaf >= 0 && e->type == ElementType::Table)
            t.nodes[current_leaf].has_table = true;
        continue;
    }
    // c) 取结构号: 先看是不是试验号(B), 否则用 clause_no
    std::string number = e->clause_no;
    bool this_is_test = false;
    if (profile == FormatProfile::B_testno) {
        std::string tn = extract_test_number(e->clause_no.empty() ? e->text : e->clause_no);
        if (!tn.empty()) { number = tn; this_is_test = true; }
    }
    // d) 无号正文: 并入当前条, 并把页尾拉大
    if (number.empty()) {
        if (current_leaf >= 0) {
            if (!t.nodes[current_leaf].text.empty()) t.nodes[current_leaf].text += "\n";
            t.nodes[current_leaf].text += e->text;
            if (e->page_no > t.nodes[current_leaf].page_end) t.nodes[current_leaf].page_end = e->page_no;
        }
        continue;
    }
    // e) 定级(用 in_test_scope 的旧值: 该号在当前作用域里解释)
    int lvl = level_for(profile, number, in_test_scope);
    if (lvl == 0) {                       // 非结构号: 当正文并入
        if (current_leaf >= 0) {
            if (!t.nodes[current_leaf].text.empty()) t.nodes[current_leaf].text += "\n";
            t.nodes[current_leaf].text += e->text;
            if (e->page_no > t.nodes[current_leaf].page_end) t.nodes[current_leaf].page_end = e->page_no;
        }
        continue;
    }
    if (lvl > retrieval_depth(profile)) { // 深于检索层级: 并入当前检索节点, 不单独建节点
        if (current_leaf >= 0) {
            if (!t.nodes[current_leaf].text.empty()) t.nodes[current_leaf].text += "\n";
            t.nodes[current_leaf].text += e->text;
            if (e->page_no > t.nodes[current_leaf].page_end) t.nodes[current_leaf].page_end = e->page_no;
        }
        continue;
    }
    // f) 弹栈到父
    while (!stack.empty() && stack.back().second >= lvl) stack.pop_back();
    int parent_idx = stack.empty() ? -1 : stack.back().first;
    // g) 建节点
    int idx = b.add(parent_idx, lvl, number, e->text, e->page_no);
    stack.push_back({idx, lvl});
    current_leaf = idx;
    // h) 更新试验作用域(在建完节点之后)
    if (this_is_test) in_test_scope = true;
    else if (profile == FormatProfile::B_testno && lvl <= 1) in_test_scope = false; // 回到根的章
}
```

> 关键顺序：`level_for` 用 `in_test_scope` 的**旧值**，建完节点后再更新。试验号自身 `lvl==2`，挂在根的章下；其后续 `1/2/3` 在 `in_test_scope=true` 下算第 3 级并建检索节点；`2.1` 算第 4 级，但深于 B 的检索深度，所以并入当前 L3 正文。

- [ ] **Step 4: 跑测试确认通过**

Run: 构建 + `rag2.0.tests.exe -tc="格式B: 试验号建二级, 内部重启编号, 重号不撞"`
Expected: PASS。再跑全部 tree_builder 用例确认格式 A 不回归。

- [ ] **Step 5: 提交**

```powershell
git add src/structure/tree_builder.cpp tests/test_tree_builder.cpp
git commit -m "feat(m2c1): format B — test-number scope + restarted numbering"
```

---

## Task 9: 跳级补虚拟节点 + 透传 suspect

**大白话:** 有时引擎把中间一级吞了（比如直接从一级跳到三级，缺了二级）。这时补一个"虚拟"父节点占位，并标记 `suspect=gap` 提示这里可疑。同时把 M2b 已经标过的 `suspect`（跳号/过短）原样带到节点上。

**Files:**
- Modify: `src/structure/tree_builder.cpp`
- Test: `tests/test_tree_builder.cpp`(追加)

- [ ] **Step 1: 追加失败测试**

```cpp
TEST_CASE("跳级: 缺中间级补虚拟节点并标 gap; 透传 M2b suspect") {
    ParsedDoc d;
    ParseElement c5 = body("5", "公路损坏分类");
    ParseElement c511 = body("5.1.1", "5.1.1 路肩……");   // 缺了 5.1
    c511.suspect = "seq";                                  // M2b 标过
    d.elements = { c5, c511 };
    ClauseTree t = build_clause_tree(d, "sid");

    const TreeNode* virt = find(t, "sid:5/5.1");           // 补出来的虚拟二级
    const TreeNode* leaf = find(t, "sid:5/5.1/5.1.1");
    REQUIRE(virt); REQUIRE(leaf);
    CHECK(virt->number == "5.1");
    CHECK(virt->suspect == "gap");
    CHECK(virt->title.empty());                            // 虚拟节点无标题
    CHECK(leaf->suspect == "seq");                         // 透传 M2b
    CHECK(leaf->parent_id == "sid:5/5.1");
}
```

> 说明：虚拟二级的号用"父号 + 缺失段"推。最简策略：若当前号比父深超过 1 级，用当前号去掉最后一段当中间号（`5.1.1` → `5.1`）。只补一层即可覆盖绝大多数；多层缺失递归同理但本 v1 只补到能接上父为止。

- [ ] **Step 2: 跑测试确认失败**

Run: 构建 + `rag2.0.tests.exe -tc="跳级: 缺中间级补虚拟节点并标 gap; 透传 M2b suspect"`
Expected: FAIL。

- [ ] **Step 3: 改实现**

(1) 给 `Builder::add` 增加 `suspect` 参数。把 `add` 整体改为这版（6 参，最终版）：

```cpp
int add(int parent_idx, int level, const std::string& number,
        const std::string& text, int page, const std::string& suspect) {
    TreeNode n;
    n.level = level; n.number = number; n.text = strip_leading_number(text, number);
    n.suspect = suspect;
    if (parent_idx >= 0) {
        n.parent_id = t.nodes[parent_idx].node_id;
        n.node_id = n.parent_id + "/" + sanitize(number);
    } else {
        n.parent_id = "";
        n.node_id = sid + ":" + sanitize(number);
    }
    n.page_start = page; n.page_end = page;
    int idx = (int)t.nodes.size();
    t.nodes.push_back(std::move(n));
    if (parent_idx >= 0) t.nodes[parent_idx].child_ids.push_back(t.nodes[idx].node_id);
    return idx;
}
```

(2) 把 Task 8 循环里的 **f) 弹栈到父 + g) 建节点** 两段（从 `while (!stack.empty()...` 到 `current_leaf = idx;`）替换为下面这版（深于检索层级的并入分支保持在这段之前；h) 更新试验作用域保持不变，仍在其后）：

```cpp
    // f) 弹栈到父
    while (!stack.empty() && stack.back().second >= lvl) stack.pop_back();
    int parent_idx = stack.empty() ? -1 : stack.back().first;
    int parent_level = stack.empty() ? 0 : stack.back().second;

    // f2) 跳级: 父比当前浅超过 1 级 → 补一个虚拟中间节点(只补一层), 标 gap
    if (lvl - parent_level >= 2 && decimal_depth(number) >= 1) {
        std::string mid = number.substr(0, number.find_last_of('.')); // "5.1.1" -> "5.1"
        int vlvl = lvl - 1;
        int vidx = b.add(parent_idx, vlvl, mid, /*text=*/"", e->page_no, /*suspect=*/"gap");
        stack.push_back({vidx, vlvl});
        parent_idx = vidx;
    }

    // g) 建节点(透传 M2b 的 e->suspect)
    int idx = b.add(parent_idx, lvl, number, e->text, e->page_no, e->suspect);
    stack.push_back({idx, lvl});
    current_leaf = idx;
```

注意：虚拟节点 `text`/`title` 为空，`number` 为推出的中间号；`strip_leading_number("", mid)` 返回空，安全。虚拟节点有子节点（真实条），故末尾"标叶子"时 `is_leaf=false`。

- [ ] **Step 4: 跑测试确认通过**

Run: 构建 + `rag2.0.tests.exe -tc="跳级: 缺中间级补虚拟节点并标 gap; 透传 M2b suspect"`
Expected: PASS。再跑全部 tree_builder 用例确认不回归（虚拟节点不应影响正常三级用例，因为正常用例不跳级）。

- [ ] **Step 5: 提交**

```powershell
git add src/structure/tree_builder.cpp tests/test_tree_builder.cpp
git commit -m "feat(m2c1): virtual nodes on level gaps + suspect passthrough"
```

---

## Task 10: treecheck 命令 + 写 tree_cache

**大白话:** 加一个命令 `rag2 treecheck data\parse_cache\<id>.json`：读缓存、建树、打印体检表（格式、节点数、各级数量、叶子数、可疑数、页码覆盖），并把树写到 `data\tree_cache\<id>.json`。这是我们在真实数据上肉眼验收的工具。

**Files:**
- Modify: `src/main.cpp`
- （无新测试；这是 CLI 装配，靠下一任务的真实数据验收）

- [ ] **Step 1: 在 main.cpp 加 treecheck 实现**

在 `src/main.cpp` 顶部已有的 include 旁加：
```cpp
#include "parse/parse_cache.h"
#include "structure/tree_builder.h"
#include "structure/clause_tree.h"
#include "util/path_utf8.h"
#include <filesystem>
#include <fstream>
#include <map>
```

在 `cmd_ocrcheck` 函数后面加：

```cpp
// 体检 + 落盘：读 parse_cache → 建条款树 → 打印统计 → 写 tree_cache。
static int cmd_treecheck(const std::string& cache_path) {
    try {
        if (!std::filesystem::exists(cache_path)) {
            spdlog::error("[FAIL] treecheck: 文件不存在: {}", cache_path); return 1;
        }
        std::ifstream f(cache_path, std::ios::binary);
        std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ParsedDoc doc = parsed_doc_from_json(js);

        std::string sid = path_utf8::stem(cache_path);   // 缓存文件名即 standard_id
        ClauseTree t = build_clause_tree(doc, sid);

        std::map<int,int> level_hist;
        int leaves = 0, suspects = 0;
        for (const auto& n : t.nodes) {
            ++level_hist[n.level];
            if (n.is_leaf) ++leaves;
            if (!n.suspect.empty()) ++suspects;
        }
        spdlog::info("treecheck {} | format={} standard_no={}", sid, t.format_profile, t.standard_no);
        spdlog::info("  节点总数={} 叶子(检索单元)={} 可疑={}", t.nodes.size(), leaves, suspects);
        for (auto& kv : level_hist) spdlog::info("  L{} 数量={}", kv.first, kv.second);
        spdlog::info("  page_clause_map 覆盖页数={}", t.page_clause_map.size());

        std::string out = "data/tree_cache/" + sid + ".json";
        write_tree_cache(out, t);
        spdlog::info("  已写 {}", out);
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] treecheck: {}", e.what()); return 1;
    }
}
```

在 `main()` 的命令分发里，`ocrcheck` 分支后面加：
```cpp
if (cmd == "treecheck") {
    if (argc < 3) { std::cout << "usage: rag2 treecheck <parse_cache.json>\n"; return 1; }
    return cmd_treecheck(argv[2]);
}
```
并把第 184 行的 usage 字符串补上 `treecheck`：
```cpp
std::cout << "usage: rag2 <smoke|ingest|query|dump|ocrcheck|treecheck> [args]\n";
```

- [ ] **Step 2: 构建确认通过**

Run:
```powershell
& "D:\vs2022\...\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
```
Expected: 构建成功，无错误。

- [ ] **Step 3: 提交**

```powershell
git add src/main.cpp
git commit -m "feat(m2c1): treecheck CLI — build tree from cache, print stats, write tree_cache"
```

---

## Task 11: 全量回归 + 真实数据验收（格式 A）

**大白话:** 跑一遍全部单测确认没碰坏别的；再用 `treecheck` 在真实的 JTC5210 缓存上跑一遍，肉眼核对：格式认成 A、章≈7、叶子≈95、页码覆盖全、可疑数合理；并打开生成的 tree_cache 抽查几条树形对不对。

**Files:**
- 无代码改动（验收任务）

- [ ] **Step 1: 跑全部单测**

```powershell
& "D:\vs2022\...\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe
```
Expected: 全绿（M2b 原有 57 用例 + 本轮新增用例）。若有失败，修到全绿再继续。

- [ ] **Step 2: 真实数据体检（格式 A）**

```powershell
chcp 65001
.\rag2.0\x64\Debug\rag2.0.exe treecheck data\parse_cache\12215131224082667446.json
```
Expected（量级核对，不必精确）：
- `format=A_decimal`
- L1 ≈ 7（章）
- 叶子(检索单元) ≈ 95（其中 L2 约 13 个总则类 + L3 约 82 个）
- `page_clause_map` 覆盖页数 > 0 且接近正文页数
- 生成了 `data\tree_cache\12215131224082667446.json`

- [ ] **Step 3: 抽查 tree_cache 树形**

打开 `data\tree_cache\12215131224082667446.json`，抽查：
- `sid:5`(公路损坏分类) 下有 `5.1 5.2 5.3 5.4`；
- `sid:5/5.1` 下有 `5.1.1…5.1.7`，且这些是 `is_leaf=true`、`text` 已削掉开头条款号；
- `sid:1/1.0.1` 直接挂在 `sid:1` 下（折叠生效）。

若量级明显不对（如叶子数 < 50 或 > 200），回到对应任务排查（多半是 `level_for` 折叠或聚合逻辑）。

- [ ] **Step 4: 提交验收产物说明（可选）**

不提交 `data/`（数据产物）。如需记录验收结论，在计划文件勾选完成即可。

---

## ⏭️ 后续（不在本计划内，仅备忘）

- **格式 B 真实验收的前置（task-zero）**：需先 `ingest` 一本汇编规范（如集料试验规程汇编）生成 `parse_cache`，再 `treecheck` 它，核对 `format=B_testno`、L2=试验号、L3=试验内检索单元、`2.1/2.2` 等细项并入 L3 正文、重号不撞。本计划已用合成数据覆盖格式 B 的逻辑单测；真实验收等有缓存后补。
- **M2c-2**（三文本）、**M2c-3**（PG/Milvus + 接进 ingest）、**M2c-4**（§4 引擎垃圾修复）各自另立计划。
