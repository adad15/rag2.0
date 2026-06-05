# M2b 进度与交接（截至 2026-06-04）

> 给接手的新会话:本文汇总 M2b「解析质量」的完成情况、端到端验证结果、现场修的坑、**在真实扫描件上发现的 PP-Structure 三类系统性失败**,以及下一步(**先做 MinerU A/B**)。分支 **`V2.0`**(已推 origin)。
>
> 配套文档:spec `docs/superpowers/specs/2026-06-03-m2b-ocr-quality-design.md`;plan `docs/superpowers/plans/2026-06-03-m2b-ocr-quality.md`;上一轮 `docs/superpowers/2026-06-02-m2a-progress-and-handoff.md`。

---

## 0. 一句话现状

- **M2b（解析质量）:完成并在真实扫描件(JTC 5210-2018)上端到端验证通过。** 条款号填充率 **0% → 100%**,图表题泄漏 0,水印/页眉已去除。单测 **57 用例 / 221 断言全绿**。
- **但发现真正的天花板在 PP-StructureV3 的版面模型**:同一份文档暴露了**三类系统性失败**(阅读顺序揉块 / 页面接缝丢内容 / 区域检测乱码),且 PP-Structure **不提供逐块置信度**,连"哪些块可疑"都没法自动标。
- **下一步最高杠杆 = 试 MinerU**(M2a 预留的可插拔后端),做同文档 A/B 对比,再决定主引擎与是否进 M2c。

---

## 1. M2b 做了什么(回顾)

**定位**:M2 第二刀。**不建树、不碰 PG/Milvus、不改检索**;只把 `data/parse_cache/<id>.json` 的 OCR 产物**清洗成干净、带结构标签的富 IR**,给 M2c 当原料。接缝 = parse_cache。

**完整流水线**(一次扫描件 ingest):

```
app.py(Python OCR 服务)
  ① 渲染 DPI 250
  ② 去水印:亮度>180 的浅灰像素置白(RAG_WM_THRESHOLD 可调,0关闭)
  ③ PP-StructureV3 版面分析+OCR
  ④ _to_elements 如实上报:透传原始 block_label→raw_label、真实置信度、逐页日志
        ↓ HTTP
C++ ppstructure_backend
  ⑤ 每批 8 页/POST、读超时 1200s、逐批 spdlog 日志
  ⑥ parse_ppstructure_json: JSON→ParseElement(含 raw_label)
        ↓
HybridParser::parse → merge_doc → normalize_parsed_doc(M2b 主菜,4 步):
  1) 丢弃:页眉/页脚/页码(raw_label∈{header,footer,number}) + 独立英文糊块
  2) apply_clause_extraction: caption 过滤(raw_label table_title/figure_title 或 图/表 前缀) + parse_clause_no 抠号
  3) tag_regions: 状态机分区 front_matter/toc/body/appendix/explanation
  4) flag_anomalies: suspect=seq(跳号)/short(正文过短)
  → schema_version=2
        ↓
write_parse_cache → data/parse_cache/<id>.json
        ↓
ocrcheck CLI: 读缓存出"OCR 体检表"
```

**parse_clause_no(共享两档文法,poppler/OCR 前端共用)**:多级号 `5.2.1`(须含点)+ 单级章号 `1总则`(仅 Heading+号后跟汉字);守卫挡 `0.5%`/`5.2%`/`3.5mm`/`200kN`。

**新增/改动文件**:`clause_no.{h,cpp}`、`ocr_normalize.{h,cpp}`、`ocr_metrics.{h,cpp}`(新);`parser.h`(IR 加 raw_label/region/is_caption/suspect + schema_version + Region 枚举)、`parse_cache.cpp`、`ppstructure_backend.cpp`、`hybrid_parser.cpp`、`main.cpp`(ocrcheck)、`services/ppstructure/app.py`。7 个 test_*.cpp。

**关键提交**(分支 V2.0,HEAD=43f4b65):
- 计划任务 T1–T9 + 集成测试:`8c40433`(IR字段)→…→`a20bdd6`(集成测试)。
- 现场运维修复:`22b6134`(DPI/批/超时+日志)、`39288fd`(置信度诚实显示)、`2d4a5fd`(去水印)、`43f4b65`(去页眉页脚)。

---

## 2. 端到端验证结果(真实数据)

对 JTC 5210-2018 扫描件重跑 ingest 后 `ocrcheck`:

```
schema_version=2 | pages=56 elements=522
正文条款候选 : 45    其中抠到号: 45  (填充率 100.0%)
图表题泄漏   : 0
可疑条款     : 4   (seq/short)
TOC 元素     : 6
置信度       : 不可用（服务未提供逐块分数）
区域分布     : front_matter / toc / body / appendix / explanation 五区齐全
水印碎片(浏览/专用/信息公开): 0   (改造前 14)
表格 table_html: 26 保留(留给 M5)
```

**对比改造前**:填充率 ~0%→100%、水印 14→0、页眉 53→0、置信度 假1.0→诚实"不可用"。条文说明(explanation)与正文(body)已正确分开 → 解决了"撞号覆盖"隐患。

---

## 3. 现场修的坑(都已提交)

| 坑 | 现象 | 修复 |
|----|------|------|
| OCR 撞读超时 | 300 DPI 下 16 页一批跑不完 600s,ingest 在 592s abort、缓存没写 | DPI→250、批 16→8、超时 600→1200s(`22b6134`) |
| 两窗口静默 | OCR 期间看不到进度 | app.py 逐页日志 + C++ 逐批 spdlog(`22b6134`) |
| 置信度假象 | PP-Structure 不给逐块分数,app.py 兜底全 0.0,旧表显示误导性 0.000 | 体检表显示"不可用",加 conf_available(`39288fd`) |
| **预览水印** | 浅灰斜纹"交通运输部信息公开/浏览专用"混入正文 | 渲染后亮度>180 置白(`2d4a5fd`)。**注:名为 `Watermark` 的 OCG 层是幌子,关层对像素零影响——水印是印进图像的光栅** |
| **页眉页脚** | 每页"公路技术状况评定标准（JTG 5210—2018）"×53 等混入 | 按 raw_label∈{header,footer,number} 丢弃(`43f4b65`)。**列项"1/2/3"是 raw_label=text,不受影响,已验证安全** |

---

## 4. ⚠️ 核心发现:PP-StructureV3 在本类文档上的三类系统性失败

**这些都不是 M2b 的 bug**——M2b 已把"引擎给的东西"洗干净;问题在引擎吐出来的东西本身就缺/错/乱。**M2b/M2c 能标记,但救不回引擎从没正确吐出的内容。**

### ① 阅读顺序错乱 / 块切分揉块
版面模型把"上一条尾巴 + 页眉/水印碎片 + 下一条正文"揉进一个块,且顺序排错。
- 例(p7):`行有关标准的规定。交通运输1.0.4公路技术状况的检测评定…尚应符合国家和行业现浏览专`
  = [1.0.3 尾巴] + [页眉"交通运输"] + [1.0.4 正文,号埋在中间] + [水印"浏览专"]。
- 例(p15):`度（1.0m）换算成损坏面积。损坏程度应按下列标准判断。5.3.4错台应为接缝两边出现的高差…检测结果应用影响宽`
  = [上一条尾巴(应接"影响宽**度**")] + [5.3.4 正文(又在"影响宽"处截断)],顺序颠倒。
- 后果:号埋在块中部 → `parse_clause_no`(只认块首)抠不到 → 约 **77 个块**条款号没抠出(其中含图表题/续文,真正丢的条款是少数)。

### ② 页面接缝丢内容
跨页骑缝的条款,其列项落在页底/页顶版面检测盲区,**两页都没吐出来**。
- 例:**5.1.6 路基沉降** 跨 p12→p13。p12 末尾停在"…损坏程度应按下列标准"(连"判断："都没),p13 开头是"判断："+"3重度应为路基沉降长度大于10m。"。**列项 1、2 整段消失。**
- 证据:同节 5.1.1~5.1.5 列项 1/2/3 全完整,**唯独跨页的 5.1.6 丢**。

### ③ 区域检测局部失败 → 乱码
给清晰黑字画错识别框,吐出乱码。
- 例(p15):`2中度应为主要裂缝宽度在3~10mm之间。`(原图又黑又清晰,我渲染验证过)被 OCR 读成 **`2EM 添司`**;同段 1、3 行完美,唯独 2 行乱码。还有 `[司ab]1`、`2添司`。
- 已排除:不是水印(当前缓存水印碎片=0)、不是阈值擦字(阈值后图我看过,该行完整)。**纯 PP-Structure 失败。**

### 致命叠加:没有逐块置信度
PP-StructureV3 的 `parsing_res_list` **不含逐块分数**(app.py 兜底全 0.0)。所以上面 ③ 这种乱码**无法靠置信度自动标红**——这是质检的一大缺口。

### ④ M2b 清洗规则过拟合 JTC 5210，换标准族就崩(重要,直接定义 M2c 需求)
实测 JTG 3432《集料试验规程》(已 ingest,parse_cache `12104388589027850934.json`,4551 元素):
- **region 灾难**:body=60 / **explanation=4455** / front_matter=28 / toc=4 / appendix=4，即 **99% 被误判为"条文说明"**。根因:第 92 个元素是 `raw_label=text`、内容恰为"条文说明"的块 → `tag_regions` 的 explanation 锁**一触即锁、永不切回** → 其后所有正文(`3集料取样方法`、`T 0301—2024集料取样法`、`3.1 皮带运输机上取样`…)全错位进 explanation。
- **clause_no 不认 T 方法号**:试验规程的"条款"是 `T 0301—2024` 形式;`parse_clause_no` 只认点分号(`5.2.1`)+单级章号(`1总则`)→ T 号 clause_no 全空。
- **结论**:`parse_clause_no` 编号文法 + `tag_regions` region 规则都是**按 JTC 5210 结构调的,不跨标准族泛化**。不是 bug,是 M2b 只在一份文档验过的**适用范围局限**。光加 T 号正则无用——这些方法此刻被埋在 explanation 区,body 填充率统计不到;**编号 + region 必须一起在 M2c 治**。

---

## 5. 下一步(按杠杆排序)

### 5.1 【最高杠杆】先做 MinerU A/B(建议立刻)
- **为什么**:同一份文档,PP-Structure 暴露了 §4 三类系统性版面问题,且不给置信度。**继续在它上面修是修不动的(garbage in)**;M2c/M3 在烂输入上做再多也白搭。
- **做什么**:实现 `MineruBackend`(M2a 已留 `OcrBackend` 接口),走独立 Python 服务;在 JTC 5210(+ JTG 3432 文字版)上与 PP-Structure **A/B 对比**,用 `ocrcheck` 体检表量化:填充率、图表题泄漏、乱码率(人工抽样)、跨页缺失率。胜者定为默认引擎。
- **流程**:brainstorm → spec → plan → 实现。
- **注意**:MinerU 需 GPU(1660Ti 6GB),环境/显存是风险点(参考 M2a 装 PP-Structure 的踩坑经验)。

### 5.2 M2c 结构化层(在胜出的引擎上做)
消费干净缓存,建条款树/三文本/写 PG+Milvus。结合 §4 的发现,M2c 切分器应:
1. **块中部拆分**:遇块内 `N.N.N` 把揉块拆开(缓解①);
2. **续接归位**:把"上一条尾巴"接回上一条;
3. **质检门禁**:加"**列项应从 1 开始**""**条款号连续性**"检查,把②③这类缺失/乱码**自动标红**(弥补无置信度的缺口);
4. 条款层级树 + atomic/retrieval/context 三文本 + page_clause_map + PG schema 扩展 + Milvus 标量字段。
5. **跨标准族鲁棒(见 §4④,JTG 3432 的真实反例)**:
   - **可扩展编号文法**:点分号(`5.2.1`)/ T 方法号(`T 0301-2024`)/ 附录字母号(`A.1`)… 按标准类型可插拔,别写死一种;
   - **鲁棒 region 检测**:"条文说明/附录"必须是**独立章级标题**才切换(不能被正文里含该词的 `text` 块触发),修掉"一触即锁、永不切回"的脆弱锁;并能处理试验规程"每个 T 方法下还有子条款(适用范围/仪具/方法…)"的嵌套结构。
   - 设计时拿 **JTC 5210(点分) + JTG 3432(T 号)** 两份做对照,确保不再过拟合单一文档。

### 5.3 M3 三路召回
dense + BM25 + PG 精确 + RRF + 查询理解。检索质量主要靠这步。

---

## 6. 怎么运行(当前状态)

**① 起 OCR 服务**(改了 app.py 必须重启才生效):
```powershell
cd "D:\vs2022 code\rag2.0\services\ppstructure"
uv run --no-sync uvicorn app:app --host 0.0.0.0 --port 8001
# 可选:$env:RAG_WM_THRESHOLD=180  (去水印阈值,0 关闭)
```

**② ingest / 体检**(项目根):
```powershell
chcp 65001
.\rag2.0\x64\Debug\rag2.0.exe ingest
.\rag2.0\x64\Debug\rag2.0.exe ocrcheck data\parse_cache\<id>.json
```
- 进度:OCR 服务窗口逐页日志(带每页耗时)、ingest 窗口逐批日志。
- 56 页约 7 批;每页 ≤ 几十秒即远在 1200s 超时内。

**构建/测试**:
```powershell
& "D:\vs2022\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" rag2.0.sln /p:Configuration=Debug /p:Platform=x64 /m
.\rag2.0.tests\x64\Debug\rag2.0.tests.exe        # 57 用例 / 221 断言
```

---

## 7. 待办清单(给下一轮)

- [ ] **MinerU A/B**(§5.1,最高优先):实现 MineruBackend + 同文档对比 + 用体检表量化,定主引擎。
- [ ] **M2c 结构化层**(§5.2):块中部拆分、续接归位、列项/条款号连续性质检门禁、条款树、三文本、PG/Milvus schema;**+ 跨标准族鲁棒(可扩展编号文法 T号/字母号 + 鲁棒 region 检测,拿 JTC 5210 与 JTG 3432 两份对照,见 §4④)**。
- [ ] **(可选速度优化,见对话)** app.py 关掉 PP-Structure 无用子模型(`use_doc_unwarping`/`use_doc_orientation_classify`/`use_textline_orientation`/`use_seal_recognition`/`use_chart_recognition`=False),纯提速不掉精度;改后需清 `data/ocr_cache/<id>/` 重跑。
- [ ] M2b 收尾:本分支 V2.0 已含全部 M2b 代码且验证通过,可按 finishing-a-development-branch 决定合并/PR。
- [ ] (低优)若坚持用 PP-Structure:研究能否从底层 OCR 文本行分数聚合出逐块置信度,以恢复"乱码自动标红"能力。

---

## 8. 关键提醒(容易忘)

- 改 `app.py` **必须重启 OCR 服务**才生效;改 C++ 必须重新 build。
- `ocrcheck` 读的是缓存,**清洗在 ingest 时做**——想看新效果必须先重新 ingest。
- M2b 的 100% 填充率落在**缓存的富 elements** 里,**给 M2c 消费**;**当前 query 检索仍走 M1 老路**(split_clauses 切页文本),M2b 没改检索。别误以为"检索已变好"。
- 用户语料**多为带水印的扫描规范**;水印是**光栅印入**,靠亮度阈值去(非 OCG 层)。
- PP-Structure 三类失败见 §4;**别再逐例排查**,直接上 MinerU A/B 量化对比。
