# M0 + M1 Walking Skeleton 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 打通规范 RAG 系统第一条端到端线——把 1 份原生 PDF 解析入库，问一个问题，得到带标准号/条款号溯源的 DeepSeek 回答；同时建立可复用的环境与接口骨架。

**Architecture:** C++ 主程序（确定性编排）通过 HTTP 调用三个外部服务（Milvus REST v2、云端 embedding API、DeepSeek API），用 PostgreSQL 作结构化底座、poppler 解析原生 PDF。从第一天就钉死 5 个接口契约（解析器 / Embedding / 检索器 / 候选归一化 / 生成上下文），让后续里程碑替换内部实现而不返工。Milvus 只负责召回，权威字段一律用 `clause_id` 回查 PG。

**Tech Stack:** C++17、VS2022 / MSBuild（`.sln`+`.vcxproj`）、vcpkg 清单模式（根目录 `vcpkg.json` + builtin-baseline 锁版本，`D:\vcpkg` 提供工具与缓存）、poppler-cpp、libpqxx（PostgreSQL）、cpp-httplib[openssl]（统一 HTTP 客户端）、nlohmann/json、spdlog、doctest（测试）。外部：PostgreSQL、Milvus 2.5.x Standalone（Docker）、DashScope/OpenAI 兼容 embedding API、DeepSeek API。

> **构建/测试命令约定（贯穿全计划，取代各任务里的 cmake/ctest 写法）：**
> - 依赖（清单模式）：根目录 `vcpkg.json` 声明依赖并用 `builtin-baseline` 锁版本；`D:\vcpkg\vcpkg integrate install`（一次，提供 MSBuild 集成）后，两 `.vcxproj` 已开 `VcpkgEnableManifest=true` 且 `VcpkgInstalledDir=D:\vcpkg-installed\rag2.0\`，**构建时自动按清单还原**到 `D:\vcpkg-installed\rag2.0\x64-windows\`（复用 `D:\vcpkg` 二进制缓存）。无需手动 `vcpkg install`。
> - 构建：`& "d:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m`（凡步骤写 `cmake --build build --config Debug` 均指此命令；也可在 VS2022 IDE 里 Ctrl+Shift+B）。
> - 跑测试：运行 `rag2.0.tests\x64\Debug\rag2.0.tests.exe`（凡步骤写 `ctest ...` 均指运行此测试 exe）。
> - 跑应用：运行 `rag2.0\x64\Debug\rag2.0.exe`（凡步骤写 `.\build\Debug\rag2.exe` 均指此 exe）。
> - **新增 .cpp 无需改工程文件**：两个 `.vcxproj` 用通配符 `..\src\**\*.cpp` / `..\tests\**\*.cpp` 收录，所以各任务里"登记 `CMakeLists.txt`"步骤一律跳过。

---

## 关键工程决策（执行前若不同意请先改）

1. **Milvus 走 REST API v2（HTTP），不用 gRPC C++ SDK。** 文档 §4.1 允许 REST；Windows 上构建 milvus C++ gRPC SDK 风险高。一个 cpp-httplib 客户端同时打通 Milvus / embedding / DeepSeek。
2. **构建用 VS2022 / MSBuild（不用 CMake）。** 沿用现有 `rag2.0.sln`/`rag2.0.vcxproj`，新增 `rag2.0.tests.vcxproj` 作测试目标；两工程用通配符收录 `src`/`tests`，独立 IntDir 避免 obj 冲突。vcpkg 用**清单模式**：根目录 `vcpkg.json`（builtin-baseline 锁版本），两 `.vcxproj` 开 `VcpkgEnableManifest=true`，配合 `integrate install` 在构建时自动还原依赖、自动链接、自动拷 DLL。poppler 头按 vcpkg 布局用 `#include <poppler/cpp/poppler-document.h>`。
3. **TDD 策略（C++ 基础设施现实做法）：** 纯逻辑（条款切分、prompt 组装、配置解析、请求体/响应体 JSON 编解码、向量解析）走"先写失败测试"的严格 TDD；I/O 连通性（PG / Milvus / DeepSeek / poppler 真实文件）通过 `smoke` / `ingest` / `query` 子命令手动验证，并对其中纯逻辑部分做单测。每个任务都标注了用哪种。
4. **所有外部配置走环境变量**，无硬编码密钥/地址。换云厂商只改 env。

## 接口契约（5 个，从本计划起钉死，后续里程碑只换实现）

| # | 契约 | 头文件 | 说明 |
|---|---|---|---|
| ① | 解析器 `Parser` → `ParsedDoc` 统一中间格式 | `src/parse/parser.h` | poppler（本计划）/ MinerU（M2）实现同一接口 |
| ② | `EmbeddingClient::Embed(text)` → `vector<float>` | `src/embedding/embedding_client.h` | 云 API（本计划）/ Qwen3-Embedding-8B（M5） |
| ③ | `Retriever::Retrieve(query, filter)` → `vector<Candidate>` | `src/retrieve/retriever.h` | dense（本计划）/ +BM25/+PG 精确（M3） |
| ④ | `Candidate{standard_id, clause_id, score, ...}` 归一化键 | `src/retrieve/candidate.h` | 文本/视觉/引用扩展候选统一去重键 |
| ⑤ | `ContextFragment`（§11.3 schema） | `src/generate/context.h` | 注入 DeepSeek 的固定结构 |

---

## 文件结构（本计划创建的文件及其单一职责）

```
rag2.0/
  CMakeLists.txt                 顶层构建：rag_core 静态库 + rag2.0 应用 + rag_tests 测试
  CMakePresets.json              vcpkg 工具链 + VS2022 预设
  vcpkg.json                     依赖清单（manifest 模式）
  .env.example                   环境变量样例（不含真实密钥）
  src/
    main.cpp                     入口：smoke / ingest / query 子命令分发
    config.h / config.cpp        从环境变量加载配置（纯逻辑可测）
    logging.h / logging.cpp      spdlog 初始化
    http/http_client.h/.cpp      cpp-httplib 封装：post_json / get_json（HTTPS）
    db/schema.sql                最小 PG schema：standards + clause_nodes 子集
    db/pg_client.h/.cpp          libpqxx 封装：连接、建表、写入、按 id 回查
    milvus/milvus_rest.h/.cpp    Milvus REST v2：建集合 / 插入 / 向量检索
    embedding/embedding_client.h 契约②（抽象基类）
    embedding/cloud_embedding.h/.cpp  云 API 实现（OpenAI 兼容 /embeddings）
    parse/parser.h               契约①：Parser 抽象 + ParsedDoc / ParsedClause IR
    parse/poppler_parser.h/.cpp  poppler 实现
    ingest/clause_splitter.h/.cpp 朴素条款号正则切分（纯逻辑可测）
    ingest/ingest_pipeline.h/.cpp 编排：parse→split→PG→embed→Milvus
    retrieve/candidate.h         契约④：Candidate 结构
    retrieve/retriever.h         契约③：Retriever 抽象
    retrieve/dense_retriever.h/.cpp  Milvus 检索 → PG 回查 → Candidate
    generate/context.h           契约⑤：ContextFragment + 序列化
    generate/prompt_builder.h/.cpp  ContextFragment[] → DeepSeek 消息（纯逻辑可测）
    generate/deepseek_client.h/.cpp DeepSeek chat 调用
    generate/answer_pipeline.h/.cpp 编排：query→embed→retrieve→context→DeepSeek
  tests/
    test_main.cpp                doctest 入口
    test_config.cpp
    test_clause_splitter.cpp
    test_context.cpp
    test_prompt_builder.cpp
    test_embedding_parse.cpp
    test_milvus_body.cpp
```

---

# 阶段 M0：环境与依赖就绪

### Task 0: 构建骨架 + 测试基础设施（MSBuild + vcpkg + doctest）

> **本任务已改为 MSBuild + vcpkg 清单模式（取代下方 CMake 步骤 1–8，仅作历史参考）。** 实际执行：
> 1. 依赖：根目录 `vcpkg.json`（已建，含 builtin-baseline）；`D:\vcpkg\vcpkg integrate install`（一次）。两 `.vcxproj` 已开 `VcpkgEnableManifest=true` 且 `VcpkgInstalledDir=D:\vcpkg-installed\rag2.0\`，构建时自动还原到 `D:\vcpkg-installed\rag2.0\x64-windows\`。
> 2. 工程文件（已建）：`rag2.0\rag2.0.vcxproj`（应用，glob `..\src\**\*.cpp`，含目录 `..\src`，C++17/x64，VcpkgEnableManifest）、`rag2.0\rag2.0.tests.vcxproj`（测试，glob `..\src` 排除 `main.cpp` + `..\tests`，链 doctest）、`rag2.0.sln`（含两工程，x64 Debug/Release）。
> 3. 源文件（已建）：`src\main.cpp`（占位入口）、`tests\test_main.cpp`（doctest 入口）、`.gitignore`（含 `vcpkg_installed/`）、`.env.example`、`vcpkg.json`。
> 4. 构建：见顶部「构建/测试命令约定」的 MSBuild 命令（首次构建会触发清单还原，较久）；跑 `rag2.0.tests\x64\Debug\rag2.0.tests.exe` 应输出 doctest 全绿。
> 5. 提交。
>
> 下面的 CMake 步骤 1–9 保留备查，**执行时跳过**。

**Files（CMake 历史方案，已废弃）:**
- Create: `vcpkg.json`
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `src/main.cpp`
- Create: `tests/test_main.cpp`
- Create: `.env.example`
- Create: `.gitignore`

- [ ] **Step 1: 写依赖清单 `vcpkg.json`**

```json
{
  "name": "rag2",
  "version-string": "0.1.0",
  "dependencies": [
    "nlohmann-json",
    "spdlog",
    "libpqxx",
    "poppler",
    { "name": "cpp-httplib", "features": ["openssl"] },
    "doctest"
  ]
}
```

- [ ] **Step 2: 写 `CMakePresets.json`（指向 vcpkg 工具链）**

把 `<VCPKG_ROOT>` 换成你的 vcpkg 安装路径（如 `C:/vcpkg`）。

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "default",
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": {
        "CMAKE_TOOLCHAIN_FILE": "<VCPKG_ROOT>/scripts/buildsystems/vcpkg.cmake",
        "VCPKG_TARGET_TRIPLET": "x64-windows"
      }
    }
  ]
}
```

- [ ] **Step 3: 写顶层 `CMakeLists.txt`（三目标：core 库 / app / tests）**

```cmake
cmake_minimum_required(VERSION 3.21)
project(rag2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(unofficial-poppler CONFIG REQUIRED)

# ---- core static library (所有逻辑都在这里，app 和 tests 共用) ----
add_library(rag_core STATIC
  src/config.cpp
  src/logging.cpp
  src/http/http_client.cpp
  src/db/pg_client.cpp
  src/milvus/milvus_rest.cpp
  src/embedding/cloud_embedding.cpp
  src/parse/poppler_parser.cpp
  src/ingest/clause_splitter.cpp
  src/ingest/ingest_pipeline.cpp
  src/retrieve/dense_retriever.cpp
  src/generate/prompt_builder.cpp
  src/generate/deepseek_client.cpp
  src/generate/answer_pipeline.cpp
)
target_include_directories(rag_core PUBLIC src)
target_link_libraries(rag_core PUBLIC
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  libpqxx::pqxx
  httplib::httplib
  unofficial::poppler::poppler-cpp
)

# ---- application ----
add_executable(rag2 src/main.cpp)
target_link_libraries(rag2 PRIVATE rag_core)

# ---- tests ----
enable_testing()
add_executable(rag_tests
  tests/test_main.cpp
  tests/test_config.cpp
  tests/test_clause_splitter.cpp
  tests/test_context.cpp
  tests/test_prompt_builder.cpp
  tests/test_embedding_parse.cpp
  tests/test_milvus_body.cpp
)
target_link_libraries(rag_tests PRIVATE rag_core doctest::doctest)
add_test(NAME rag_tests COMMAND rag_tests)
```

> 注：vcpkg 的 poppler 端口导出目标名为 `unofficial::poppler::poppler-cpp`、包名 `unofficial-poppler`。若 `find_package` 名称在你的 vcpkg 版本不同，按 `vcpkg` 安装日志末尾打印的 "Usage" 提示修正这两行。

- [ ] **Step 4: 写 doctest 入口 `tests/test_main.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("test harness is alive") {
    CHECK(1 + 1 == 2);
}
```

- [ ] **Step 5: 写最小 `src/main.cpp`（占位入口，后续任务填子命令）**

```cpp
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "usage: rag2 <smoke|ingest|query> [args]\n";
        return 1;
    }
    std::string cmd = argv[1];
    std::cout << "command: " << cmd << " (not implemented yet)\n";
    return 0;
}
```

- [ ] **Step 6: 写 `.gitignore` 与 `.env.example`**

`.gitignore`:
```
/build/
.env
*.user
.vs/
```

`.env.example`（执行 M0 时复制为本机环境变量，勿提交真实密钥）:
```
RAG_PG_CONNINFO=host=localhost port=5432 dbname=rag user=postgres password=postgres
RAG_MILVUS_BASE_URL=http://localhost:19530
RAG_MILVUS_TOKEN=root:Milvus
RAG_MILVUS_COLLECTION=clause_text
RAG_EMBED_BASE_URL=https://dashscope.aliyuncs.com
RAG_EMBED_PATH=/compatible-mode/v1/embeddings
RAG_EMBED_MODEL=text-embedding-v3
RAG_EMBED_DIM=1024
RAG_EMBED_KEY=
RAG_DEEPSEEK_BASE_URL=https://api.deepseek.com
RAG_DEEPSEEK_PATH=/chat/completions
RAG_DEEPSEEK_MODEL=deepseek-chat
RAG_DEEPSEEK_KEY=
RAG_DOC_PATH=
```

- [ ] **Step 7: 配置并构建**

Run（PowerShell，在项目根目录）:
```
cmake --preset default
cmake --build build --config Debug
```
Expected: 配置阶段 vcpkg 自动安装依赖（首次较慢），构建成功生成 `build/Debug/rag2.exe` 与 `build/Debug/rag_tests.exe`。

- [ ] **Step 8: 跑测试验证测试基础设施可用**

Run:
```
ctest --test-dir build -C Debug --output-on-failure
```
Expected: PASS（`test harness is alive`）。

- [ ] **Step 9: Commit**

```
git add vcpkg.json CMakeLists.txt CMakePresets.json src/main.cpp tests/test_main.cpp .env.example .gitignore
git commit -m "build: bootstrap CMake+vcpkg project with doctest harness"
```

---

### Task 1: 配置加载（从环境变量）

**Files:**
- Create: `src/config.h`, `src/config.cpp`
- Test: `tests/test_config.cpp`

- [ ] **Step 1: 写失败测试 `tests/test_config.cpp`**

`Config::from_map` 是纯函数（输入 key→value 的 map），便于测试，不依赖真实环境变量。

```cpp
#include <doctest/doctest.h>
#include "config.h"

TEST_CASE("Config::from_map fills fields and applies defaults") {
    std::map<std::string, std::string> env = {
        {"RAG_PG_CONNINFO", "host=localhost dbname=rag"},
        {"RAG_EMBED_KEY", "sk-embed"},
        {"RAG_DEEPSEEK_KEY", "sk-deep"},
        {"RAG_DOC_PATH", "C:/docs/a.pdf"},
    };
    Config c = Config::from_map(env);
    CHECK(c.pg_conninfo == "host=localhost dbname=rag");
    CHECK(c.embed_key == "sk-embed");
    CHECK(c.deepseek_key == "sk-deep");
    CHECK(c.doc_path == "C:/docs/a.pdf");
    // 默认值
    CHECK(c.milvus_base_url == "http://localhost:19530");
    CHECK(c.embed_model == "text-embedding-v3");
    CHECK(c.embed_dim == 1024);
    CHECK(c.deepseek_model == "deepseek-chat");
    CHECK(c.milvus_collection == "clause_text");
}

TEST_CASE("Config::from_map reports missing required keys") {
    std::map<std::string, std::string> env;
    Config c = Config::from_map(env);
    auto missing = c.missing_required();
    CHECK(std::find(missing.begin(), missing.end(), "RAG_PG_CONNINFO") != missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "RAG_EMBED_KEY") != missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "RAG_DEEPSEEK_KEY") != missing.end());
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug`
Expected: 编译失败（`config.h` 不存在）。

- [ ] **Step 3: 写 `src/config.h`**

```cpp
#pragma once
#include <string>
#include <map>
#include <vector>
#include <algorithm>

struct Config {
    std::string pg_conninfo;
    std::string milvus_base_url;
    std::string milvus_token;
    std::string milvus_collection;
    std::string embed_base_url;
    std::string embed_path;
    std::string embed_model;
    int         embed_dim = 1024;
    std::string embed_key;
    std::string deepseek_base_url;
    std::string deepseek_path;
    std::string deepseek_model;
    std::string deepseek_key;
    std::string doc_path;

    // 从任意 key->value map 构建（测试友好）
    static Config from_map(const std::map<std::string, std::string>& env);
    // 从真实进程环境变量构建
    static Config from_env();
    // 返回缺失的必填 key 名列表
    std::vector<std::string> missing_required() const;
};
```

- [ ] **Step 4: 写 `src/config.cpp`**

```cpp
#include "config.h"
#include <cstdlib>

static std::string get(const std::map<std::string, std::string>& e,
                       const std::string& k, const std::string& def = "") {
    auto it = e.find(k);
    return it != e.end() && !it->second.empty() ? it->second : def;
}

Config Config::from_map(const std::map<std::string, std::string>& e) {
    Config c;
    c.pg_conninfo      = get(e, "RAG_PG_CONNINFO");
    c.milvus_base_url  = get(e, "RAG_MILVUS_BASE_URL", "http://localhost:19530");
    c.milvus_token     = get(e, "RAG_MILVUS_TOKEN", "root:Milvus");
    c.milvus_collection= get(e, "RAG_MILVUS_COLLECTION", "clause_text");
    c.embed_base_url   = get(e, "RAG_EMBED_BASE_URL", "https://dashscope.aliyuncs.com");
    c.embed_path       = get(e, "RAG_EMBED_PATH", "/compatible-mode/v1/embeddings");
    c.embed_model      = get(e, "RAG_EMBED_MODEL", "text-embedding-v3");
    c.embed_dim        = std::stoi(get(e, "RAG_EMBED_DIM", "1024"));
    c.embed_key        = get(e, "RAG_EMBED_KEY");
    c.deepseek_base_url= get(e, "RAG_DEEPSEEK_BASE_URL", "https://api.deepseek.com");
    c.deepseek_path    = get(e, "RAG_DEEPSEEK_PATH", "/chat/completions");
    c.deepseek_model   = get(e, "RAG_DEEPSEEK_MODEL", "deepseek-chat");
    c.deepseek_key     = get(e, "RAG_DEEPSEEK_KEY");
    c.doc_path         = get(e, "RAG_DOC_PATH");
    return c;
}

Config Config::from_env() {
    std::map<std::string, std::string> e;
    const char* keys[] = {
        "RAG_PG_CONNINFO","RAG_MILVUS_BASE_URL","RAG_MILVUS_TOKEN","RAG_MILVUS_COLLECTION",
        "RAG_EMBED_BASE_URL","RAG_EMBED_PATH","RAG_EMBED_MODEL","RAG_EMBED_DIM","RAG_EMBED_KEY",
        "RAG_DEEPSEEK_BASE_URL","RAG_DEEPSEEK_PATH","RAG_DEEPSEEK_MODEL","RAG_DEEPSEEK_KEY","RAG_DOC_PATH"
    };
    for (const char* k : keys) {
        const char* v = std::getenv(k);
        if (v) e[k] = v;
    }
    return from_map(e);
}

std::vector<std::string> Config::missing_required() const {
    std::vector<std::string> m;
    if (pg_conninfo.empty()) m.push_back("RAG_PG_CONNINFO");
    if (embed_key.empty())   m.push_back("RAG_EMBED_KEY");
    if (deepseek_key.empty())m.push_back("RAG_DEEPSEEK_KEY");
    return m;
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 6: Commit**

```
git add src/config.h src/config.cpp tests/test_config.cpp CMakeLists.txt
git commit -m "feat: env-based Config loader with defaults and required-key check"
```

---

### Task 2: 日志初始化（spdlog）

**Files:**
- Create: `src/logging.h`, `src/logging.cpp`

无独立单测（纯封装）；后续任务通过运行时输出验证。

- [ ] **Step 1: 写 `src/logging.h`**

```cpp
#pragma once
namespace logging {
// 初始化全局 logger（控制台 + 时间戳），可重复调用安全。
void init();
}
```

- [ ] **Step 2: 写 `src/logging.cpp`**

```cpp
#include "logging.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <atomic>

namespace logging {
void init() {
    static std::atomic<bool> done{false};
    bool expected = false;
    if (!done.compare_exchange_strong(expected, true)) return;
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);
}
}
```

- [ ] **Step 3: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 4: Commit**

```
git add src/logging.h src/logging.cpp CMakeLists.txt
git commit -m "feat: spdlog logging init"
```

---

### Task 3: HTTP 客户端封装（cpp-httplib，统一 HTTPS POST/GET JSON）

**Files:**
- Create: `src/http/http_client.h`, `src/http/http_client.cpp`

I/O 封装，纯逻辑（URL 拆分）做单测，真实请求在后续 smoke 验证。

- [ ] **Step 1: 写 `src/http/http_client.h`**

```cpp
#pragma once
#include <string>
#include <map>

namespace http {

struct Response {
    int status = 0;            // 0 表示连接失败
    std::string body;
    std::string error;         // 连接层错误描述
    bool ok() const { return status >= 200 && status < 300; }
};

// 把 "https://host:port" 或 "http://host" 拆成 scheme+host(:port) 基址与剩余无关。
// cpp-httplib 的 Client 可直接吃 "https://host:port" 形式的基址字符串。
// 这里仅做最小封装：POST/GET 一个 JSON 字符串。
Response post_json(const std::string& base_url, const std::string& path,
                   const std::string& json_body,
                   const std::map<std::string, std::string>& headers);

Response get_json(const std::string& base_url, const std::string& path,
                  const std::map<std::string, std::string>& headers);

}  // namespace http
```

- [ ] **Step 2: 写 `src/http/http_client.cpp`**

```cpp
#include "http/http_client.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace http {

static httplib::Headers to_headers(const std::map<std::string, std::string>& h) {
    httplib::Headers out;
    for (auto& kv : h) out.emplace(kv.first, kv.second);
    return out;
}

Response post_json(const std::string& base_url, const std::string& path,
                   const std::string& body,
                   const std::map<std::string, std::string>& headers) {
    httplib::Client cli(base_url.c_str());
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(120, 0);
    cli.enable_server_certificate_verification(false);  // M1 阶段简化；生产应开启
    auto res = cli.Post(path.c_str(), to_headers(headers), body, "application/json");
    Response r;
    if (!res) { r.status = 0; r.error = httplib::to_string(res.error()); return r; }
    r.status = res->status; r.body = res->body; return r;
}

Response get_json(const std::string& base_url, const std::string& path,
                  const std::map<std::string, std::string>& headers) {
    httplib::Client cli(base_url.c_str());
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(60, 0);
    cli.enable_server_certificate_verification(false);
    auto res = cli.Get(path.c_str(), to_headers(headers));
    Response r;
    if (!res) { r.status = 0; r.error = httplib::to_string(res.error()); return r; }
    r.status = res->status; r.body = res->body; return r;
}

}  // namespace http
```

- [ ] **Step 3: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功（确认 cpp-httplib 的 OpenSSL 特性已生效，无链接错误）。

- [ ] **Step 4: Commit**

```
git add src/http/http_client.h src/http/http_client.cpp CMakeLists.txt
git commit -m "feat: cpp-httplib HTTPS json client wrapper"
```

---

### Task 4: PostgreSQL 客户端 + 最小 schema

**Files:**
- Create: `src/db/schema.sql`
- Create: `src/db/pg_client.h`, `src/db/pg_client.cpp`

I/O 任务：连通性由 smoke 子命令（Task 8）验证。

- [ ] **Step 1: 写最小 schema `src/db/schema.sql`**

仅 standards + clause_nodes 的 M1 子集（完整版在 M2）。

```sql
CREATE TABLE IF NOT EXISTS standards (
    standard_id   TEXT PRIMARY KEY,
    standard_no   TEXT,
    standard_name TEXT,
    status        TEXT DEFAULT '现行',
    file_path     TEXT,
    created_at    TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS clause_nodes (
    node_id     TEXT PRIMARY KEY,
    standard_id TEXT REFERENCES standards(standard_id),
    clause_no   TEXT,
    title       TEXT,
    path        TEXT,
    text        TEXT,
    page_start  INT
);

CREATE INDEX IF NOT EXISTS idx_clause_standard ON clause_nodes(standard_id);
```

- [ ] **Step 2: 写 `src/db/pg_client.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <optional>

struct ClauseRow {
    std::string node_id;
    std::string standard_id;
    std::string clause_no;
    std::string title;
    std::string path;
    std::string text;
    int page_start = 0;
};

struct StandardRow {
    std::string standard_id;
    std::string standard_no;
    std::string standard_name;
    std::string status;
    std::string file_path;
};

class PgClient {
public:
    explicit PgClient(std::string conninfo);
    // 执行 schema.sql 内容建表
    void apply_schema(const std::string& schema_sql);
    // 连通性检查：SELECT 1，成功返回 true
    bool ping();
    void upsert_standard(const StandardRow& s);
    void insert_clause(const ClauseRow& c);
    // 按 node_id 回查（契约：Milvus 命中后以 PG 为权威源）
    std::optional<ClauseRow> get_clause(const std::string& node_id);
    std::optional<StandardRow> get_standard(const std::string& standard_id);
private:
    std::string conninfo_;
};
```

- [ ] **Step 3: 写 `src/db/pg_client.cpp`**

```cpp
#include "db/pg_client.h"
#include <pqxx/pqxx>

PgClient::PgClient(std::string conninfo) : conninfo_(std::move(conninfo)) {}

void PgClient::apply_schema(const std::string& schema_sql) {
    pqxx::connection c(conninfo_);
    pqxx::work tx(c);
    tx.exec(schema_sql);
    tx.commit();
}

bool PgClient::ping() {
    try {
        pqxx::connection c(conninfo_);
        pqxx::work tx(c);
        auto r = tx.exec("SELECT 1");
        return r.size() == 1;
    } catch (const std::exception&) {
        return false;
    }
}

void PgClient::upsert_standard(const StandardRow& s) {
    pqxx::connection c(conninfo_);
    pqxx::work tx(c);
    tx.exec_params(
        "INSERT INTO standards(standard_id,standard_no,standard_name,status,file_path) "
        "VALUES($1,$2,$3,$4,$5) ON CONFLICT (standard_id) DO UPDATE SET "
        "standard_no=EXCLUDED.standard_no, standard_name=EXCLUDED.standard_name, "
        "status=EXCLUDED.status, file_path=EXCLUDED.file_path",
        s.standard_id, s.standard_no, s.standard_name, s.status, s.file_path);
    tx.commit();
}

void PgClient::insert_clause(const ClauseRow& c) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    tx.exec_params(
        "INSERT INTO clause_nodes(node_id,standard_id,clause_no,title,path,text,page_start) "
        "VALUES($1,$2,$3,$4,$5,$6,$7) ON CONFLICT (node_id) DO UPDATE SET "
        "text=EXCLUDED.text, clause_no=EXCLUDED.clause_no, path=EXCLUDED.path",
        c.node_id, c.standard_id, c.clause_no, c.title, c.path, c.text, c.page_start);
    tx.commit();
}

std::optional<ClauseRow> PgClient::get_clause(const std::string& node_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec_params(
        "SELECT node_id,standard_id,clause_no,title,path,text,COALESCE(page_start,0) "
        "FROM clause_nodes WHERE node_id=$1", node_id);
    if (r.empty()) return std::nullopt;
    auto row = r[0];
    ClauseRow c;
    c.node_id = row[0].c_str(); c.standard_id = row[1].c_str();
    c.clause_no = row[2].c_str(); c.title = row[3].c_str();
    c.path = row[4].c_str(); c.text = row[5].c_str();
    c.page_start = row[6].as<int>();
    return c;
}

std::optional<StandardRow> PgClient::get_standard(const std::string& standard_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec_params(
        "SELECT standard_id,standard_no,standard_name,status,COALESCE(file_path,'') "
        "FROM standards WHERE standard_id=$1", standard_id);
    if (r.empty()) return std::nullopt;
    auto row = r[0];
    StandardRow s;
    s.standard_id = row[0].c_str(); s.standard_no = row[1].c_str();
    s.standard_name = row[2].c_str(); s.status = row[3].c_str();
    s.file_path = row[4].c_str();
    return s;
}
```

- [ ] **Step 4: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功（确认 libpqxx 链接正常）。

- [ ] **Step 5: Commit**

```
git add src/db/schema.sql src/db/pg_client.h src/db/pg_client.cpp CMakeLists.txt
git commit -m "feat: PostgreSQL client (libpqxx) with minimal schema and authoritative get_clause"
```

---

### Task 5: Milvus REST v2 客户端（建集合 / 插入 / 检索）

**Files:**
- Create: `src/milvus/milvus_rest.h`, `src/milvus/milvus_rest.cpp`
- Test: `tests/test_milvus_body.cpp`（纯逻辑：请求体 JSON 构造）

- [ ] **Step 1: 写失败测试 `tests/test_milvus_body.cpp`**

把"构造 search 请求体"做成纯函数以便测试。

```cpp
#include <doctest/doctest.h>
#include "milvus/milvus_rest.h"
#include <nlohmann/json.hpp>

TEST_CASE("build_search_body produces valid Milvus REST v2 search payload") {
    std::vector<float> vec = {0.1f, 0.2f, 0.3f};
    std::string body = milvus::build_search_body("clause_text", vec, 5, {"node_id", "standard_id"});
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["limit"] == 5);
    CHECK(j["data"][0].size() == 3);
    CHECK(j["data"][0][0] == doctest::Approx(0.1f));
    CHECK(j["outputFields"][0] == "node_id");
    CHECK(j["annsField"] == "dense");
}

TEST_CASE("build_insert_body wraps one row with dense vector") {
    std::vector<float> vec = {1.0f, 2.0f};
    std::string body = milvus::build_insert_body("clause_text", "n1", "s1", vec);
    auto j = nlohmann::json::parse(body);
    CHECK(j["collectionName"] == "clause_text");
    CHECK(j["data"][0]["node_id"] == "n1");
    CHECK(j["data"][0]["standard_id"] == "s1");
    CHECK(j["data"][0]["dense"].size() == 2);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（`milvus_rest.h` 不存在）。

- [ ] **Step 3: 写 `src/milvus/milvus_rest.h`**

```cpp
#pragma once
#include <string>
#include <vector>

namespace milvus {

struct Hit {
    std::string node_id;
    std::string standard_id;
    float score = 0.0f;
};

// 纯函数：构造 REST v2 请求体（可单测）
std::string build_insert_body(const std::string& collection,
                              const std::string& node_id,
                              const std::string& standard_id,
                              const std::vector<float>& dense);
std::string build_search_body(const std::string& collection,
                              const std::vector<float>& query,
                              int top_k,
                              const std::vector<std::string>& output_fields);

class MilvusRest {
public:
    MilvusRest(std::string base_url, std::string token);
    bool ping();                                  // GET /v2/vectordb/collections/list
    // 若集合不存在则创建（dense 维度 = dim）
    void ensure_collection(const std::string& collection, int dim);
    void insert(const std::string& collection, const std::string& node_id,
                const std::string& standard_id, const std::vector<float>& dense);
    std::vector<Hit> search(const std::string& collection,
                            const std::vector<float>& query, int top_k);
private:
    std::string base_url_;
    std::string token_;
};

}  // namespace milvus
```

- [ ] **Step 4: 写 `src/milvus/milvus_rest.cpp`**

```cpp
#include "milvus/milvus_rest.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace milvus {

std::string build_insert_body(const std::string& collection, const std::string& node_id,
                              const std::string& standard_id, const std::vector<float>& dense) {
    json row;
    row["node_id"] = node_id;
    row["standard_id"] = standard_id;
    row["dense"] = dense;
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({row});
    return body.dump();
}

std::string build_search_body(const std::string& collection, const std::vector<float>& query,
                              int top_k, const std::vector<std::string>& output_fields) {
    json body;
    body["collectionName"] = collection;
    body["data"] = json::array({query});
    body["annsField"] = "dense";
    body["limit"] = top_k;
    body["outputFields"] = output_fields;
    return body.dump();
}

MilvusRest::MilvusRest(std::string base_url, std::string token)
    : base_url_(std::move(base_url)), token_(std::move(token)) {}

static std::map<std::string, std::string> auth_headers(const std::string& token) {
    return { {"Authorization", "Bearer " + token} };
}

bool MilvusRest::ping() {
    auto res = http::post_json(base_url_, "/v2/vectordb/collections/list", "{}",
                               auth_headers(token_));
    return res.ok();
}

void MilvusRest::ensure_collection(const std::string& collection, int dim) {
    // 已存在则直接返回
    {
        json q; q["collectionName"] = collection;
        auto has = http::post_json(base_url_, "/v2/vectordb/collections/has", q.dump(),
                                   auth_headers(token_));
        if (has.ok()) {
            auto j = json::parse(has.body, nullptr, false);
            if (!j.is_discarded() && j.contains("data") &&
                j["data"].contains("has") && j["data"]["has"].get<bool>())
                return;
        }
    }
    // 用快速建集合 + 自定义 schema（主键 node_id varchar，dense 向量）
    json schema;
    schema["autoID"] = false;
    schema["fields"] = json::array({
        { {"fieldName","node_id"}, {"dataType","VarChar"}, {"isPrimary",true},
          {"elementTypeParams", { {"max_length", 256} }} },
        { {"fieldName","standard_id"}, {"dataType","VarChar"},
          {"elementTypeParams", { {"max_length", 128} }} },
        { {"fieldName","dense"}, {"dataType","FloatVector"},
          {"elementTypeParams", { {"dim", dim} }} }
    });
    json index;
    index = json::array({
        { {"fieldName","dense"}, {"indexName","dense_idx"}, {"metricType","COSINE"} }
    });
    json body;
    body["collectionName"] = collection;
    body["schema"] = schema;
    body["indexParams"] = index;
    auto res = http::post_json(base_url_, "/v2/vectordb/collections/create", body.dump(),
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus create collection failed: " + res.body + res.error);
}

void MilvusRest::insert(const std::string& collection, const std::string& node_id,
                        const std::string& standard_id, const std::vector<float>& dense) {
    auto body = build_insert_body(collection, node_id, standard_id, dense);
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/insert", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus insert failed: " + res.body + res.error);
}

std::vector<Hit> MilvusRest::search(const std::string& collection,
                                    const std::vector<float>& query, int top_k) {
    auto body = build_search_body(collection, query, top_k, {"node_id", "standard_id"});
    auto res = http::post_json(base_url_, "/v2/vectordb/entities/search", body,
                               auth_headers(token_));
    if (!res.ok())
        throw std::runtime_error("milvus search failed: " + res.body + res.error);
    auto j = json::parse(res.body);
    std::vector<Hit> hits;
    for (auto& item : j["data"]) {
        Hit h;
        h.node_id = item.value("node_id", "");
        h.standard_id = item.value("standard_id", "");
        h.score = item.value("distance", 0.0f);
        hits.push_back(h);
    }
    return hits;
}

}  // namespace milvus
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS（两个 build_*_body 测试）。

- [ ] **Step 6: Commit**

```
git add src/milvus/milvus_rest.h src/milvus/milvus_rest.cpp tests/test_milvus_body.cpp CMakeLists.txt
git commit -m "feat: Milvus REST v2 client (create/insert/search) with testable body builders"
```

---

### Task 6: DeepSeek 客户端

**Files:**
- Create: `src/generate/deepseek_client.h`, `src/generate/deepseek_client.cpp`

- [ ] **Step 1: 写 `src/generate/deepseek_client.h`**

```cpp
#pragma once
#include <string>

namespace deepseek {
struct Message { std::string role; std::string content; };

class DeepSeekClient {
public:
    DeepSeekClient(std::string base_url, std::string path,
                   std::string model, std::string api_key);
    // 发送 system+user 两条消息，返回回答文本；失败抛异常
    std::string chat(const std::string& system, const std::string& user);
private:
    std::string base_url_, path_, model_, api_key_;
};
}
```

- [ ] **Step 2: 写 `src/generate/deepseek_client.cpp`**

```cpp
#include "generate/deepseek_client.h"
#include "http/http_client.h"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace deepseek {

DeepSeekClient::DeepSeekClient(std::string base_url, std::string path,
                               std::string model, std::string api_key)
    : base_url_(std::move(base_url)), path_(std::move(path)),
      model_(std::move(model)), api_key_(std::move(api_key)) {}

std::string DeepSeekClient::chat(const std::string& system, const std::string& user) {
    json body;
    body["model"] = model_;
    body["messages"] = json::array({
        { {"role","system"}, {"content", system} },
        { {"role","user"},   {"content", user} }
    });
    body["temperature"] = 0.0;
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_}
    };
    auto res = http::post_json(base_url_, path_, body.dump(), headers);
    if (!res.ok())
        throw std::runtime_error("deepseek chat failed: " + res.body + res.error);
    auto j = json::parse(res.body);
    return j["choices"][0]["message"]["content"].get<std::string>();
}

}  // namespace deepseek
```

- [ ] **Step 3: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 4: Commit**

```
git add src/generate/deepseek_client.h src/generate/deepseek_client.cpp CMakeLists.txt
git commit -m "feat: DeepSeek chat client (OpenAI-compatible)"
```

---

### Task 7: 解析器契约① + poppler 实现

**Files:**
- Create: `src/parse/parser.h`（契约①：Parser 抽象 + ParsedDoc IR）
- Create: `src/parse/poppler_parser.h`, `src/parse/poppler_parser.cpp`

- [ ] **Step 1: 写契约① `src/parse/parser.h`**

```cpp
#pragma once
#include <string>
#include <vector>

// 统一中间格式（IR）——poppler（M1）与 MinerU（M2）都产出它。
// M1 仅需文本 + 页码；表格/公式/block 字段为 M2 预留，先留空。
struct ParsedPage {
    int page_no = 0;       // 从 1 开始
    std::string text;      // 该页全文
};

struct ParsedDoc {
    std::string source_path;
    std::string title;            // M1 可为文件名
    std::vector<ParsedPage> pages;
};

class Parser {
public:
    virtual ~Parser() = default;
    virtual ParsedDoc parse(const std::string& file_path) = 0;
};
```

- [ ] **Step 2: 写 `src/parse/poppler_parser.h`**

```cpp
#pragma once
#include "parse/parser.h"

class PopplerParser : public Parser {
public:
    ParsedDoc parse(const std::string& file_path) override;
};
```

- [ ] **Step 3: 写 `src/parse/poppler_parser.cpp`**

```cpp
#include "parse/poppler_parser.h"
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <memory>
#include <stdexcept>
#include <filesystem>

ParsedDoc PopplerParser::parse(const std::string& file_path) {
    std::unique_ptr<poppler::document> doc(
        poppler::document::load_from_file(file_path));
    if (!doc)
        throw std::runtime_error("poppler: cannot open " + file_path);

    ParsedDoc out;
    out.source_path = file_path;
    out.title = std::filesystem::path(file_path).filename().string();

    int n = doc->pages();
    for (int i = 0; i < n; ++i) {
        std::unique_ptr<poppler::page> pg(doc->create_page(i));
        ParsedPage p;
        p.page_no = i + 1;
        if (pg) {
            poppler::byte_array ba = pg->text().to_utf8();
            p.text.assign(ba.begin(), ba.end());
        }
        out.pages.push_back(std::move(p));
    }
    return out;
}
```

- [ ] **Step 4: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功（确认 poppler-cpp 头与库链接正常）。

- [ ] **Step 5: Commit**

```
git add src/parse/parser.h src/parse/poppler_parser.h src/parse/poppler_parser.cpp CMakeLists.txt
git commit -m "feat: Parser contract (ParsedDoc IR) + poppler implementation"
```

---

### Task 8: `smoke` 子命令（M0 验收：四个连通性冒烟测试）

**Files:**
- Modify: `src/main.cpp`

把四项连通性检查接进 `rag2 smoke`。这是 **M0 的验收闸**。

- [ ] **Step 1: 改写 `src/main.cpp`，加入 smoke 子命令**

```cpp
#include <iostream>
#include <string>
#include "config.h"
#include "logging.h"
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "generate/deepseek_client.h"
#include "parse/poppler_parser.h"
#include <spdlog/spdlog.h>

static int cmd_smoke(const Config& cfg) {
    int failures = 0;

    // 1. PostgreSQL
    try {
        PgClient pg(cfg.pg_conninfo);
        if (pg.ping()) spdlog::info("[OK] PostgreSQL 连接成功");
        else { spdlog::error("[FAIL] PostgreSQL ping 失败"); ++failures; }
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] PostgreSQL: {}", e.what()); ++failures;
    }

    // 2. Milvus
    try {
        milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
        if (mv.ping()) spdlog::info("[OK] Milvus 连接成功");
        else { spdlog::error("[FAIL] Milvus ping 失败"); ++failures; }
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] Milvus: {}", e.what()); ++failures;
    }

    // 3. DeepSeek（真实调用一次）
    try {
        deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                    cfg.deepseek_model, cfg.deepseek_key);
        std::string ans = ds.chat("你是测试助手，只回复 OK。", "请回复 OK");
        spdlog::info("[OK] DeepSeek 响应: {}", ans);
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] DeepSeek: {}", e.what()); ++failures;
    }

    // 4. poppler 打开 PDF
    try {
        if (cfg.doc_path.empty()) {
            spdlog::error("[FAIL] poppler: 未设置 RAG_DOC_PATH"); ++failures;
        } else {
            PopplerParser parser;
            ParsedDoc d = parser.parse(cfg.doc_path);
            spdlog::info("[OK] poppler 解析 {} 页，首页前 40 字: {}",
                         d.pages.size(),
                         d.pages.empty() ? "" : d.pages[0].text.substr(0, 40));
        }
    } catch (const std::exception& e) {
        spdlog::error("[FAIL] poppler: {}", e.what()); ++failures;
    }

    if (failures == 0) { spdlog::info("==== M0 冒烟全部通过 ===="); return 0; }
    spdlog::error("==== M0 冒烟失败 {} 项 ====", failures);
    return 1;
}

int main(int argc, char** argv) {
    logging::init();
    if (argc < 2) {
        std::cout << "usage: rag2 <smoke|ingest|query> [args]\n";
        return 1;
    }
    Config cfg = Config::from_env();
    std::string cmd = argv[1];

    if (cmd == "smoke") {
        auto missing = cfg.missing_required();
        if (!missing.empty()) {
            for (auto& m : missing) spdlog::error("缺少必填环境变量: {}", m);
            return 1;
        }
        return cmd_smoke(cfg);
    }
    std::cout << "command: " << cmd << " (not implemented yet)\n";
    return 0;
}
```

- [ ] **Step 2: 启动外部依赖（Milvus via Docker）**

Run（PowerShell，需已装 Docker Desktop）:
```
Invoke-WebRequest https://github.com/milvus-io/milvus/releases/download/v2.5.4/milvus-standalone-docker-compose.yml -OutFile docker-compose.yml
docker compose up -d
```
Expected: milvus-standalone / etcd / minio 三个容器 Up；`http://localhost:19530` 可访问。
（PostgreSQL 若未装：可 `docker run -d --name rag-pg -e POSTGRES_PASSWORD=postgres -e POSTGRES_DB=rag -p 5432:5432 postgres:16`。）

- [ ] **Step 3: 设置本机环境变量并填入真实值**

Run（PowerShell，把空值换成你的真实密钥与 PDF 路径）:
```
$env:RAG_PG_CONNINFO="host=localhost port=5432 dbname=rag user=postgres password=postgres"
$env:RAG_MILVUS_BASE_URL="http://localhost:19530"
$env:RAG_MILVUS_TOKEN="root:Milvus"
$env:RAG_EMBED_KEY="<你的 embedding API key>"
$env:RAG_DEEPSEEK_KEY="<你的 DeepSeek key>"
$env:RAG_DOC_PATH="<你的原生 PDF 绝对路径>"
```

- [ ] **Step 4: 跑 smoke，验证 M0 验收**

Run:
```
cmake --build build --config Debug
.\build\Debug\rag2.exe smoke
```
Expected: 四行 `[OK]` + `==== M0 冒烟全部通过 ====`，退出码 0。
（若某项 FAIL：先用 §6 风险表对应排查——Milvus 版本/容器、key、PDF 路径。）

- [ ] **Step 5: Commit**

```
git add src/main.cpp
git commit -m "feat: smoke subcommand wiring four connectivity checks (M0 acceptance)"
```

> **M0 完成判据：** `rag2 smoke` 退出码 0，四项全 OK。

---

# 阶段 M1：端到端最小骨架

### Task 9: 候选契约④ + 上下文契约⑤

**Files:**
- Create: `src/retrieve/candidate.h`（契约④）
- Create: `src/generate/context.h`（契约⑤）
- Test: `tests/test_context.cpp`

- [ ] **Step 1: 写失败测试 `tests/test_context.cpp`**

```cpp
#include <doctest/doctest.h>
#include "generate/context.h"
#include <nlohmann/json.hpp>

TEST_CASE("ContextFragment serializes to the §11.3 schema") {
    ContextFragment f;
    f.source_id = "S1";
    f.standard_no = "JTG D60-2015";
    f.standard_name = "公路桥涵设计通用规范";
    f.status = "现行";
    f.clause_no = "4.2.1";
    f.path = "第4章 / 4.2 / 4.2.1";
    f.is_mandatory = true;
    f.text = "条款原文……";
    auto j = to_json(f);
    CHECK(j["source_id"] == "S1");
    CHECK(j["standard_no"] == "JTG D60-2015");
    CHECK(j["status"] == "现行");
    CHECK(j["clause_no"] == "4.2.1");
    CHECK(j["is_mandatory"] == true);
    CHECK(j.contains("tables"));
    CHECK(j.contains("formulas"));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（`context.h` 不存在）。

- [ ] **Step 3: 写契约④ `src/retrieve/candidate.h`**

```cpp
#pragma once
#include <string>

// 所有召回候选统一归一到 standard_id + clause_id（node_id），作为去重键。
struct Candidate {
    std::string standard_id;
    std::string clause_id;   // == clause_nodes.node_id
    float score = 0.0f;
    std::string source;      // "dense" | "bm25" | "pg_exact" | "visual" ...
};
```

- [ ] **Step 4: 写契约⑤ `src/generate/context.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// §11.3 注入 DeepSeek 的固定结构化片段。
struct ContextFragment {
    std::string source_id;     // S1, S2 ...
    std::string standard_no;
    std::string standard_name;
    std::string status;
    std::string clause_no;
    std::string path;
    bool is_mandatory = false;
    std::string text;
    std::vector<std::string> tables;    // M1 留空，M5 填表格片段
    std::vector<std::string> formulas;  // M1 留空
};

inline nlohmann::json to_json(const ContextFragment& f) {
    return {
        {"source_id", f.source_id},
        {"standard_no", f.standard_no},
        {"standard_name", f.standard_name},
        {"status", f.status},
        {"clause_no", f.clause_no},
        {"path", f.path},
        {"is_mandatory", f.is_mandatory},
        {"text", f.text},
        {"tables", f.tables},
        {"formulas", f.formulas}
    };
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 6: Commit**

```
git add src/retrieve/candidate.h src/generate/context.h tests/test_context.cpp CMakeLists.txt
git commit -m "feat: Candidate (contract 4) and ContextFragment (contract 5, §11.3 schema)"
```

---

### Task 10: 朴素条款切分器（纯逻辑 TDD）

**Files:**
- Create: `src/ingest/clause_splitter.h`, `src/ingest/clause_splitter.cpp`
- Test: `tests/test_clause_splitter.cpp`

- [ ] **Step 1: 写失败测试 `tests/test_clause_splitter.cpp`**

```cpp
#include <doctest/doctest.h>
#include "ingest/clause_splitter.h"

TEST_CASE("split_clauses extracts clause_no and text by numeric headers") {
    std::string page =
        "4.2.1 桥涵设计应符合本规范的规定。\n"
        "4.2.2 设计洪水频率应按表4.2.2取值。\n"
        "4.3.1 荷载组合应符合下列要求。\n";
    auto clauses = split_clauses(page, /*page_no=*/12);

    REQUIRE(clauses.size() == 3);
    CHECK(clauses[0].clause_no == "4.2.1");
    CHECK(clauses[0].text.find("桥涵设计应符合") != std::string::npos);
    CHECK(clauses[0].page_start == 12);
    CHECK(clauses[1].clause_no == "4.2.2");
    CHECK(clauses[2].clause_no == "4.3.1");
}

TEST_CASE("split_clauses ignores lines without a leading clause number") {
    std::string page = "前言\n本规范由交通运输部提出。\n1.0.1 为规范设计，制定本规范。\n";
    auto clauses = split_clauses(page, 1);
    REQUIRE(clauses.size() == 1);
    CHECK(clauses[0].clause_no == "1.0.1");
}

TEST_CASE("split_clauses appends continuation lines to current clause") {
    std::string page = "4.2.1 第一行。\n续行内容仍属于4.2.1。\n4.2.2 下一条。\n";
    auto clauses = split_clauses(page, 5);
    REQUIRE(clauses.size() == 2);
    CHECK(clauses[0].text.find("续行内容") != std::string::npos);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（`clause_splitter.h` 不存在）。

- [ ] **Step 3: 写 `src/ingest/clause_splitter.h`**

```cpp
#pragma once
#include <string>
#include <vector>

struct SplitClause {
    std::string clause_no;
    std::string text;
    int page_start = 0;
};

// 朴素切分：按行扫描，行首匹配 形如 N.N.N / N.N / N.N.N-x 的条款号即起新条款，
// 否则把该行追加到当前条款正文。M1 临时实现，M2 由层级树正规化替换。
std::vector<SplitClause> split_clauses(const std::string& page_text, int page_no);
```

- [ ] **Step 4: 写 `src/ingest/clause_splitter.cpp`**

```cpp
#include "ingest/clause_splitter.h"
#include <regex>
#include <sstream>

std::vector<SplitClause> split_clauses(const std::string& page_text, int page_no) {
    // 行首条款号：数字段 + 至少两段 .数字（覆盖 1.0.1 / 4.2.1 / 4.2.1-1 / 4.2.1-a）
    static const std::regex head(R"(^\s*(\d+\.\d+(?:\.\d+)?(?:-[0-9a-zA-Z]+)?)\s+(.*)$)");

    std::vector<SplitClause> out;
    std::istringstream iss(page_text);
    std::string line;
    while (std::getline(iss, line)) {
        std::smatch m;
        if (std::regex_match(line, m, head)) {
            SplitClause c;
            c.clause_no = m[1].str();
            c.text = m[2].str();
            c.page_start = page_no;
            out.push_back(std::move(c));
        } else if (!out.empty()) {
            // 续行：去掉首尾空白后并入当前条款
            std::string trimmed = line;
            size_t a = trimmed.find_first_not_of(" \t\r");
            if (a != std::string::npos) {
                out.back().text += trimmed.substr(a);
            }
        }
    }
    return out;
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS（3 个用例）。

- [ ] **Step 6: Commit**

```
git add src/ingest/clause_splitter.h src/ingest/clause_splitter.cpp tests/test_clause_splitter.cpp CMakeLists.txt
git commit -m "feat: naive clause splitter (M1 temporary) with TDD"
```

---

### Task 11: Embedding 契约② + 云实现 + 响应解析测试

**Files:**
- Create: `src/embedding/embedding_client.h`（契约②）
- Create: `src/embedding/cloud_embedding.h`, `src/embedding/cloud_embedding.cpp`
- Test: `tests/test_embedding_parse.cpp`

- [ ] **Step 1: 写失败测试 `tests/test_embedding_parse.cpp`**

把"解析 OpenAI 兼容 embedding 响应"做成纯函数测试（不触网）。

```cpp
#include <doctest/doctest.h>
#include "embedding/cloud_embedding.h"

TEST_CASE("parse_embedding_response extracts the first vector") {
    std::string resp = R"({
      "data": [ { "embedding": [0.1, 0.2, 0.3], "index": 0 } ],
      "model": "text-embedding-v3"
    })";
    std::vector<float> v = parse_embedding_response(resp);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == doctest::Approx(0.1f));
    CHECK(v[2] == doctest::Approx(0.3f));
}

TEST_CASE("build_embedding_request_body wraps input and model") {
    std::string body = build_embedding_request_body("text-embedding-v3", "压实度限值");
    auto j = nlohmann::json::parse(body);
    CHECK(j["model"] == "text-embedding-v3");
    CHECK(j["input"] == "压实度限值");
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写契约② `src/embedding/embedding_client.h`**

```cpp
#pragma once
#include <string>
#include <vector>

// 契约②：文本 -> 稠密向量。M1 用云 API；M5 换 Qwen3-Embedding-8B 只换实现。
class EmbeddingClient {
public:
    virtual ~EmbeddingClient() = default;
    virtual std::vector<float> embed(const std::string& text) = 0;
    virtual int dim() const = 0;
};
```

- [ ] **Step 4: 写 `src/embedding/cloud_embedding.h`**

```cpp
#pragma once
#include "embedding/embedding_client.h"
#include <nlohmann/json.hpp>

// 纯函数（可单测）
std::string build_embedding_request_body(const std::string& model, const std::string& input);
std::vector<float> parse_embedding_response(const std::string& json_body);

class CloudEmbedding : public EmbeddingClient {
public:
    CloudEmbedding(std::string base_url, std::string path, std::string model,
                   std::string api_key, int dim);
    std::vector<float> embed(const std::string& text) override;
    int dim() const override { return dim_; }
private:
    std::string base_url_, path_, model_, api_key_;
    int dim_;
};
```

- [ ] **Step 5: 写 `src/embedding/cloud_embedding.cpp`**

```cpp
#include "embedding/cloud_embedding.h"
#include "http/http_client.h"
#include <stdexcept>

using nlohmann::json;

std::string build_embedding_request_body(const std::string& model, const std::string& input) {
    json body;
    body["model"] = model;
    body["input"] = input;   // OpenAI 兼容：单条字符串
    return body.dump();
}

std::vector<float> parse_embedding_response(const std::string& json_body) {
    auto j = json::parse(json_body);
    std::vector<float> v;
    for (auto& x : j["data"][0]["embedding"]) v.push_back(x.get<float>());
    return v;
}

CloudEmbedding::CloudEmbedding(std::string base_url, std::string path, std::string model,
                               std::string api_key, int dim)
    : base_url_(std::move(base_url)), path_(std::move(path)), model_(std::move(model)),
      api_key_(std::move(api_key)), dim_(dim) {}

std::vector<float> CloudEmbedding::embed(const std::string& text) {
    auto body = build_embedding_request_body(model_, text);
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_}
    };
    auto res = http::post_json(base_url_, path_, body, headers);
    if (!res.ok())
        throw std::runtime_error("embedding api failed: " + res.body + res.error);
    auto v = parse_embedding_response(res.body);
    if ((int)v.size() != dim_)
        throw std::runtime_error("embedding dim mismatch: got " + std::to_string(v.size()));
    return v;
}
```

- [ ] **Step 6: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 7: Commit**

```
git add src/embedding/embedding_client.h src/embedding/cloud_embedding.h src/embedding/cloud_embedding.cpp tests/test_embedding_parse.cpp CMakeLists.txt
git commit -m "feat: EmbeddingClient contract + cloud (OpenAI-compatible) impl with testable parse"
```

---

### Task 12: 入库管道（parse → split → PG → embed → Milvus）

**Files:**
- Create: `src/ingest/ingest_pipeline.h`, `src/ingest/ingest_pipeline.cpp`

编排任务；端到端在 Task 15 的 `ingest` 子命令验证。

- [ ] **Step 1: 写 `src/ingest/ingest_pipeline.h`**

```cpp
#pragma once
#include <string>
#include "db/pg_client.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"
#include "parse/parser.h"

struct IngestResult {
    std::string standard_id;
    int clause_count = 0;
};

// 解析 1 份文件 → 切分条款 → 写 PG → 生成向量 → 写 Milvus。
// standard_no/standard_name 在 M1 用文件名占位（M2 由元数据抽取替换）。
IngestResult ingest_file(const std::string& file_path,
                         Parser& parser,
                         PgClient& pg,
                         milvus::MilvusRest& mv,
                         EmbeddingClient& embed,
                         const std::string& collection);
```

- [ ] **Step 2: 写 `src/ingest/ingest_pipeline.cpp`**

```cpp
#include "ingest/ingest_pipeline.h"
#include "ingest/clause_splitter.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <functional>

static std::string make_id(const std::string& s) {
    return std::to_string(std::hash<std::string>{}(s));
}

IngestResult ingest_file(const std::string& file_path, Parser& parser, PgClient& pg,
                         milvus::MilvusRest& mv, EmbeddingClient& embed,
                         const std::string& collection) {
    ParsedDoc doc = parser.parse(file_path);

    std::string fname = std::filesystem::path(file_path).filename().string();
    std::string standard_id = make_id(file_path);

    StandardRow s;
    s.standard_id = standard_id;
    s.standard_no = fname;          // M1 占位
    s.standard_name = doc.title;    // M1 占位
    s.status = "现行";
    s.file_path = file_path;
    pg.upsert_standard(s);

    mv.ensure_collection(collection, embed.dim());

    IngestResult result;
    result.standard_id = standard_id;

    for (auto& page : doc.pages) {
        auto clauses = split_clauses(page.text, page.page_no);
        for (auto& c : clauses) {
            std::string node_id = standard_id + ":" + c.clause_no;

            ClauseRow row;
            row.node_id = node_id;
            row.standard_id = standard_id;
            row.clause_no = c.clause_no;
            row.title = "";
            row.path = c.clause_no;     // M1 占位，M2 用层级路径
            row.text = c.text;
            row.page_start = c.page_start;
            pg.insert_clause(row);

            // 检索文本：M1 简单拼标准号 + 条款号 + 正文（retrieval_text 雏形）
            std::string retrieval_text = s.standard_no + " " + c.clause_no + " " + c.text;
            std::vector<float> vec = embed.embed(retrieval_text);
            mv.insert(collection, node_id, standard_id, vec);

            ++result.clause_count;
        }
    }
    spdlog::info("入库完成: {} 条条款 (standard_id={})", result.clause_count, standard_id);
    return result;
}
```

- [ ] **Step 3: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 4: Commit**

```
git add src/ingest/ingest_pipeline.h src/ingest/ingest_pipeline.cpp CMakeLists.txt
git commit -m "feat: ingest pipeline (parse->split->PG->embed->Milvus)"
```

---

### Task 13: 检索契约③ + dense 检索器（Milvus 命中 → PG 回查）

**Files:**
- Create: `src/retrieve/retriever.h`（契约③）
- Create: `src/retrieve/dense_retriever.h`, `src/retrieve/dense_retriever.cpp`

体现底座原则：Milvus 只给 node_id + 分数，权威字段一律回查 PG。

- [ ] **Step 1: 写契约③ `src/retrieve/retriever.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "retrieve/candidate.h"

// 契约③：query → 候选列表。M1 仅 dense；M3 注册 BM25 / PG 精确，融合不改此接口。
class Retriever {
public:
    virtual ~Retriever() = default;
    virtual std::vector<Candidate> retrieve(const std::string& query, int top_k) = 0;
};
```

- [ ] **Step 2: 写 `src/retrieve/dense_retriever.h`**

```cpp
#pragma once
#include "retrieve/retriever.h"
#include "milvus/milvus_rest.h"
#include "embedding/embedding_client.h"

class DenseRetriever : public Retriever {
public:
    DenseRetriever(milvus::MilvusRest& mv, EmbeddingClient& embed, std::string collection);
    std::vector<Candidate> retrieve(const std::string& query, int top_k) override;
private:
    milvus::MilvusRest& mv_;
    EmbeddingClient& embed_;
    std::string collection_;
};
```

- [ ] **Step 3: 写 `src/retrieve/dense_retriever.cpp`**

```cpp
#include "retrieve/dense_retriever.h"

DenseRetriever::DenseRetriever(milvus::MilvusRest& mv, EmbeddingClient& embed,
                               std::string collection)
    : mv_(mv), embed_(embed), collection_(std::move(collection)) {}

std::vector<Candidate> DenseRetriever::retrieve(const std::string& query, int top_k) {
    std::vector<float> qv = embed_.embed(query);
    auto hits = mv_.search(collection_, qv, top_k);
    std::vector<Candidate> out;
    for (auto& h : hits) {
        Candidate c;
        c.standard_id = h.standard_id;
        c.clause_id = h.node_id;   // 归一化键
        c.score = h.score;
        c.source = "dense";
        out.push_back(c);
    }
    return out;
}
```

- [ ] **Step 4: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 5: Commit**

```
git add src/retrieve/retriever.h src/retrieve/dense_retriever.h src/retrieve/dense_retriever.cpp CMakeLists.txt
git commit -m "feat: Retriever contract + dense retriever (Milvus hit -> Candidate)"
```

---

### Task 14: Prompt 组装器（纯逻辑 TDD）

**Files:**
- Create: `src/generate/prompt_builder.h`, `src/generate/prompt_builder.cpp`
- Test: `tests/test_prompt_builder.cpp`

- [ ] **Step 1: 写失败测试 `tests/test_prompt_builder.cpp`**

```cpp
#include <doctest/doctest.h>
#include "generate/prompt_builder.h"

TEST_CASE("build_system_prompt enforces sourcing and refusal rules") {
    std::string sys = build_system_prompt();
    CHECK(sys.find("标准号") != std::string::npos);
    CHECK(sys.find("条款号") != std::string::npos);
    CHECK(sys.find("拒答") != std::string::npos);
}

TEST_CASE("build_user_prompt embeds question and JSON context fragments") {
    ContextFragment f;
    f.source_id = "S1";
    f.standard_no = "JTG D60-2015";
    f.clause_no = "4.2.1";
    f.status = "现行";
    f.text = "桥涵设计应符合规定。";
    std::string user = build_user_prompt("桥涵设计有什么要求？", {f});
    CHECK(user.find("桥涵设计有什么要求？") != std::string::npos);
    CHECK(user.find("S1") != std::string::npos);
    CHECK(user.find("4.2.1") != std::string::npos);
    CHECK(user.find("JTG D60-2015") != std::string::npos);
}

TEST_CASE("build_user_prompt with empty context still asks model to refuse") {
    std::string user = build_user_prompt("不在库的问题", {});
    CHECK(user.find("不在库的问题") != std::string::npos);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: 编译失败（头不存在）。

- [ ] **Step 3: 写 `src/generate/prompt_builder.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "generate/context.h"

// §11.2 约束 -> system prompt
std::string build_system_prompt();
// 问题 + §11.3 上下文片段 -> user prompt
std::string build_user_prompt(const std::string& question,
                              const std::vector<ContextFragment>& fragments);
```

- [ ] **Step 4: 写 `src/generate/prompt_builder.cpp`**

```cpp
#include "generate/prompt_builder.h"
#include <nlohmann/json.hpp>

std::string build_system_prompt() {
    return
        "你是规范条文检索助手。严格遵守：\n"
        "1. 只能依据提供的检索上下文回答，不得编造；\n"
        "2. 回答必须标明标准号、标准名称、条款号；\n"
        "3. 涉及强制性条文必须提示；\n"
        "4. 涉及废止/非现行标准必须提示；\n"
        "5. 若上下文为空或不足以支撑，必须明确拒答，提示未检索到依据；\n"
        "6. 不得编造条款号、限值、单位。\n"
        "引用格式示例：依据《标准名称》（标准号）第 X.X.X 条……";
}

std::string build_user_prompt(const std::string& question,
                              const std::vector<ContextFragment>& fragments) {
    nlohmann::json ctx = nlohmann::json::array();
    for (auto& f : fragments) ctx.push_back(to_json(f));
    std::string s = "【用户问题】\n" + question + "\n\n【检索上下文（JSON 数组）】\n";
    s += ctx.dump(2);
    s += "\n\n请依据上述上下文作答，并按要求标注来源；若不足以作答请拒答。";
    return s;
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cmake --build build --config Debug` 后 `ctest --test-dir build -C Debug --output-on-failure`
Expected: PASS。

- [ ] **Step 6: Commit**

```
git add src/generate/prompt_builder.h src/generate/prompt_builder.cpp tests/test_prompt_builder.cpp CMakeLists.txt
git commit -m "feat: prompt builder enforcing sourcing/refusal rules (§11.2/§11.3)"
```

---

### Task 15: 问答管道（query → embed → retrieve → 回查 PG → context → DeepSeek）

**Files:**
- Create: `src/generate/answer_pipeline.h`, `src/generate/answer_pipeline.cpp`

- [ ] **Step 1: 写 `src/generate/answer_pipeline.h`**

```cpp
#pragma once
#include <string>
#include "retrieve/retriever.h"
#include "db/pg_client.h"
#include "generate/deepseek_client.h"

// query → 检索候选 → 用 clause_id 回查 PG 拿权威字段 → 组装 ContextFragment
// → DeepSeek 生成带溯源回答。候选为空时直接返回拒答提示（不调用模型）。
std::string answer_query(const std::string& question,
                         Retriever& retriever,
                         PgClient& pg,
                         deepseek::DeepSeekClient& ds,
                         int top_k);
```

- [ ] **Step 2: 写 `src/generate/answer_pipeline.cpp`**

```cpp
#include "generate/answer_pipeline.h"
#include "generate/context.h"
#include "generate/prompt_builder.h"
#include <vector>

std::string answer_query(const std::string& question, Retriever& retriever, PgClient& pg,
                         deepseek::DeepSeekClient& ds, int top_k) {
    auto candidates = retriever.retrieve(question, top_k);
    if (candidates.empty())
        return "未检索到相关规范依据，无法作答。";

    std::vector<ContextFragment> fragments;
    int idx = 1;
    for (auto& c : candidates) {
        // 底座原则：以 PG 回查为权威源
        auto clause = pg.get_clause(c.clause_id);
        if (!clause) continue;
        auto std_row = pg.get_standard(clause->standard_id);

        ContextFragment f;
        f.source_id = "S" + std::to_string(idx++);
        f.standard_no = std_row ? std_row->standard_no : "";
        f.standard_name = std_row ? std_row->standard_name : "";
        f.status = std_row ? std_row->status : "";
        f.clause_no = clause->clause_no;
        f.path = clause->path;
        f.is_mandatory = false;   // M1 未识别强制性，M2 补
        f.text = clause->text;
        fragments.push_back(std::move(f));
    }
    if (fragments.empty())
        return "检索命中但回查规范原文为空，无法作答。";

    std::string sys = build_system_prompt();
    std::string user = build_user_prompt(question, fragments);
    return ds.chat(sys, user);
}
```

- [ ] **Step 3: 构建验证**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 4: Commit**

```
git add src/generate/answer_pipeline.h src/generate/answer_pipeline.cpp CMakeLists.txt
git commit -m "feat: answer pipeline (retrieve -> PG authoritative lookup -> DeepSeek)"
```

---

### Task 16: `ingest` 与 `query` 子命令（M1 验收：端到端）

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: 在 `main.cpp` 顶部补充 include 与 schema 加载辅助**

在现有 include 区追加：
```cpp
#include "embedding/cloud_embedding.h"
#include "ingest/ingest_pipeline.h"
#include "retrieve/dense_retriever.h"
#include "generate/answer_pipeline.h"
#include <fstream>
#include <sstream>
```

在 `cmd_smoke` 上方加入辅助函数：
```cpp
static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
```

- [ ] **Step 2: 在 `main.cpp` 加入 `cmd_ingest` 与 `cmd_query`**

```cpp
static int cmd_ingest(const Config& cfg) {
    if (cfg.doc_path.empty()) { spdlog::error("未设置 RAG_DOC_PATH"); return 1; }
    PgClient pg(cfg.pg_conninfo);
    pg.apply_schema(read_file("src/db/schema.sql"));   // 幂等建表

    milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
    CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                         cfg.embed_key, cfg.embed_dim);
    PopplerParser parser;

    auto r = ingest_file(cfg.doc_path, parser, pg, mv, embed, cfg.milvus_collection);
    spdlog::info("ingest 完成: standard_id={}, clauses={}", r.standard_id, r.clause_count);
    return 0;
}

static int cmd_query(const Config& cfg, const std::string& question) {
    PgClient pg(cfg.pg_conninfo);
    milvus::MilvusRest mv(cfg.milvus_base_url, cfg.milvus_token);
    CloudEmbedding embed(cfg.embed_base_url, cfg.embed_path, cfg.embed_model,
                         cfg.embed_key, cfg.embed_dim);
    DenseRetriever retriever(mv, embed, cfg.milvus_collection);
    deepseek::DeepSeekClient ds(cfg.deepseek_base_url, cfg.deepseek_path,
                                cfg.deepseek_model, cfg.deepseek_key);

    std::string ans = answer_query(question, retriever, pg, ds, /*top_k=*/5);
    std::cout << "\n===== 回答 =====\n" << ans << "\n";
    return 0;
}
```

- [ ] **Step 3: 在 `main` 的命令分发里接上**

把 `main` 末尾的占位分支替换为：
```cpp
    if (cmd == "ingest") {
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("缺少 {}", m); return 1; }
        return cmd_ingest(cfg);
    }
    if (cmd == "query") {
        if (argc < 3) { std::cout << "usage: rag2 query \"你的问题\"\n"; return 1; }
        auto missing = cfg.missing_required();
        if (!missing.empty()) { for (auto& m : missing) spdlog::error("缺少 {}", m); return 1; }
        return cmd_query(cfg, argv[2]);
    }
    std::cout << "unknown command: " << cmd << "\n";
    return 1;
```

- [ ] **Step 4: 构建**

Run: `cmake --build build --config Debug`
Expected: 成功。

- [ ] **Step 5: 端到端入库（用你的真实 PDF）**

Run（沿用 Task 8 Step 3 的环境变量；从项目根目录运行以便相对路径 `src/db/schema.sql` 可见）:
```
.\build\Debug\rag2.exe ingest
```
Expected: 日志显示 `ingest 完成: ... clauses=N`（N>0）；PG `clause_nodes` 有 N 行，Milvus `clause_text` 集合 entity 数 = N。

- [ ] **Step 6: 端到端提问（M1 验收）**

Run（问一个该 PDF 中条款相关的问题）:
```
.\build\Debug\rag2.exe query "你这份规范里关于<某主题>的要求是什么？"
```
Expected: 输出一段回答，且**引用到该文档中真实存在的条款号**（与 PG 中 `clause_no` 对得上）。

- [ ] **Step 7: Commit**

```
git add src/main.cpp
git commit -m "feat: ingest+query subcommands — end-to-end walking skeleton (M1 acceptance)"
```

> **M1 完成判据：** `rag2 ingest` 成功写入 N>0 条条款；`rag2 query "..."` 返回的回答能正确引用该文档中真实存在的条款号。

---

## 自检结果（Spec 覆盖核对）

对照来源 spec 的 M0/M1 范围与 5 个接口契约：

- **M0 四项连通性冒烟** → Task 8 `smoke` 子命令（PG/Milvus/DeepSeek/poppler）✅
- **M0 C++ 依赖链编译通过** → Task 0 vcpkg manifest + CMake（poppler/libpqxx/cpp-httplib/nlohmann-json/spdlog）✅
- **M1 poppler 抽取** → Task 7 ✅
- **M1 朴素条款号正则切分** → Task 10 ✅
- **M1 最小 PG 表 standards+clause_nodes 子集** → Task 4 schema + Task 12 写入 ✅
- **M1 临时 embedding 生成 dense 向量写 Milvus** → Task 11 + Task 12 ✅
- **M1 单路 dense 召回 top-k** → Task 13 ✅
- **M1 组装上下文 + DeepSeek 带溯源回答** → Task 14/15/16 ✅
- **契约①解析器** → Task 7 `parser.h` ✅
- **契约②Embedding** → Task 11 `embedding_client.h` ✅
- **契约③检索器** → Task 13 `retriever.h` ✅
- **契约④候选归一化 standard_id+clause_id** → Task 9 `candidate.h`，Task 13 填充 ✅
- **契约⑤生成上下文 §11.3 schema** → Task 9 `context.h`，Task 14/15 使用 ✅
- **Milvus REST（非 gRPC）决策** → Task 5 ✅
- **底座原则：Milvus 命中后回查 PG 为权威** → Task 15 `answer_query` ✅

无占位符遗留；类型/签名跨任务一致（`split_clauses`/`Candidate.clause_id`/`ContextFragment`/`MilvusRest::search` 等命名前后一致）。

## 开放项（执行时需你提供，非计划缺陷）

1. `RAG_EMBED_KEY`、`RAG_DEEPSEEK_KEY` 真实密钥（Task 8 Step 3）。
2. `RAG_DOC_PATH` 你的原生 PDF 路径（Task 8 / Task 16）。
3. vcpkg 安装路径填入 `CMakePresets.json`（Task 0 Step 2）。
4. 若你的云 embedding 不是 DashScope：改 `RAG_EMBED_BASE_URL/PATH/MODEL/DIM` 四个 env（接口②已隔离，代码不动）。
5. 确认 Milvus 版本号（计划用 2.5.4；只要 ≥2.5 即可，REST v2 与全文检索特性可用）。
