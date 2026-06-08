"""PP-StructureV3(PaddleOCR 3.x) FastAPI 服务：把扫描页 OCR 成归一化元素流，供 rag2.0 C++ 端调用。

契约（与 C++ 端 ppstructure_backend.cpp / parse_ppstructure_json 对齐）：
  POST /parse_pages {"file_path": "...", "pages": [1-based 页号...]}  -> {"elements": [...]}
  POST /parse       {"file_path": "..."}                              -> {"elements": [...]}  (全篇)
  GET  /health                                                        -> {"status":"ok","engine":"v3"}
元素字段：type ∈ {Heading,Text,Table,Formula,Figure}(区分大小写)、page_no、raw_label、level、
  clause_no、title、text、table_html、caption、ocr_confidence。失败用 {"error": "..."}。

原始料缓存（唯一 OCR 缓存，C++ 端已不再自建缓存）：
  每页把模型“原始结果”(res0.json) 存到 data/ocr_raw_cache/<key>/p<page>.json；
  下次命中则跳过渲染+GPU，直接对原始料重跑 _to_elements。
  key 只由“渲染参数”决定(DPI / 去水印阈值 / paddleocr 版本)——改 _to_elements 的挑字段逻辑
  不会使缓存失效（这正是目的：调提取代码不必重跑 GPU）；改 DPI/阈值/版本会自动换 key 重跑。
  ⚠️ 改了 _engine 的子模块开关(use_xxx) 等不进 key 的参数后，需手动删 data/ocr_raw_cache/ 才会重 OCR。

依赖：paddleocr>=3.0 + paddlex[ocr] + paddlepaddle(-gpu)。构造 PPStructureV3() 首次会下载模型到 ~/.paddlex。
"""
import os
import time
import json
import hashlib
from fastapi import FastAPI
from pydantic import BaseModel
import fitz                      # PyMuPDF：渲染指定页为图（免系统 poppler）
import numpy as np
from PIL import Image
import paddleocr
from paddleocr import PPStructureV3

app = FastAPI()
_engine = PPStructureV3(         # 首次构造会下载模型，较慢
    use_doc_orientation_classify=False,
    use_doc_unwarping=False,
    use_textline_orientation=False,
    use_seal_recognition=False,
    use_chart_recognition=False,
)

class PagesReq(BaseModel):
    file_path: str
    pages: list[int] = []        # 1-based；空表示全篇

class FileReq(BaseModel):
    file_path: str

# 渲染分辨率。进缓存 key（改了要重 OCR）。
_DPI = int(os.environ.get("RAG_DPI", "250"))
# 去预览水印：水印为浅灰斜纹(亮度高)，正文为黑。亮度 > 阈值的像素置白。
# 0 = 关闭；环境变量 RAG_WM_THRESHOLD 可覆盖（这类规范常见水印实测 170~190 干净）。进缓存 key。
_WM_THRESHOLD = int(os.environ.get("RAG_WM_THRESHOLD", "180"))
_PADDLE_VER = getattr(paddleocr, "__version__", "unknown")

# 原始料缓存根目录：定位到项目根 data/（本文件在 <root>/services/ppstructure/app.py）。
_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_RAW_CACHE_ROOT = os.path.join(_PROJECT_ROOT, "data", "ocr_raw_cache")

def _raw_cache_dir(file_path):
    # key 只含“渲染参数”：同一文件、同 DPI/阈值/版本 → 同目录；改提取逻辑不影响 key。
    key = f"{file_path}|dpi={_DPI}|wm={_WM_THRESHOLD}|ppocr={_PADDLE_VER}"
    h = hashlib.sha1(key.encode("utf-8")).hexdigest()[:16]
    return os.path.join(_RAW_CACHE_ROOT, h)

def _raw_cache_path(cache_dir, page):
    return os.path.join(cache_dir, f"p{page}.json")

def _json_default(o):
    # 保险：.json 一般已是纯 Python 类型；万一夹了 numpy 标量/数组也能落盘。
    if isinstance(o, np.ndarray):
        return o.tolist()
    if isinstance(o, np.integer):
        return int(o)
    if isinstance(o, np.floating):
        return float(o)
    raise TypeError(f"not JSON serializable: {type(o)}")

def _render_page(doc, page_index_0based, dpi=_DPI):
    page = doc[page_index_0based]
    pix = page.get_pixmap(dpi=dpi)
    arr = np.array(Image.frombytes("RGB", [pix.width, pix.height], pix.samples))
    if _WM_THRESHOLD > 0:
        lum = arr @ np.array([0.299, 0.587, 0.114])   # 亮度
        arr[lum > _WM_THRESHOLD] = 255                # 浅灰水印/背景 -> 纯白，保留深色正文
    return arr

def _raw_from_result(result):
    """从 predict() 返回值取“原始结果” dict（res0.json）。这就是要缓存的东西。"""
    res0 = result[0] if isinstance(result, (list, tuple)) and result else result
    raw = getattr(res0, "json", None)
    return raw if isinstance(raw, dict) else {}

def _to_elements(raw, page_no):
    """把 PP-StructureV3 单页“原始结果” dict 映射为 IR 元素。
       如实上报：透传原始 block_label 到 raw_label、透传真实置信度；语义判断交 C++。
       入参 raw 为 res0.json（含 res 包裹）——既来自实时 predict 也来自原始料缓存，路径一致。"""
    if not isinstance(raw, dict):
        return []
    d = raw.get("res", raw)
    blocks = d.get("parsing_res_list") or []
    out = []
    for b in blocks:
        if not isinstance(b, dict):
            continue
        rawlbl = (b.get("block_label") or "text")
        label = rawlbl.lower()
        content = b.get("block_content", "") or ""
        # 真实置信度：优先块级分数，缺失/非数（含数字字符串）则置 0.0（而非伪 1.0），便于 C++ 端识别"无分数"
        conf = b.get("block_score", b.get("score", None))
        try:
            conf = float(conf)
        except (TypeError, ValueError):
            conf = 0.0
        base = {"page_no": page_no, "raw_label": rawlbl, "ocr_confidence": conf}
        if "table" in label:
            out.append({**base, "type": "Table", "table_html": content, "caption": "", "text": ""})
        elif "formula" in label:
            out.append({**base, "type": "Formula", "text": content})
        elif "title" in label:     # doc_title/paragraph_title/table_title/figure_title...
            out.append({**base, "type": "Heading", "level": 1, "title": content, "text": content})
        else:
            if content.strip():
                out.append({**base, "type": "Text", "text": content})
    return out

def _ocr_page_raw(doc, pno, cache_dir):
    """返回某页的“原始结果” dict：命中缓存直接读（不渲染、不跑 GPU）；否则跑模型并落盘。
       返回 (raw_dict, from_cache)。"""
    path = _raw_cache_path(cache_dir, pno)
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f), True
    img = _render_page(doc, pno - 1)
    raw = _raw_from_result(_engine.predict(input=img))
    tmp = path + ".tmp"          # 临时文件 + 改名，防半截文件
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(raw, f, ensure_ascii=False, default=_json_default)
    os.replace(tmp, path)
    return raw, False

def _parse(file_path, pages):
    if not os.path.exists(file_path):
        return {"error": f"file not found: {file_path}"}
    doc = fitz.open(file_path)
    page_nums = pages if pages else list(range(1, doc.page_count + 1))
    total = len(page_nums)
    cache_dir = _raw_cache_dir(file_path)
    os.makedirs(cache_dir, exist_ok=True)
    t_batch = time.time()
    print(f"[ocr] 收到请求：{total} 页（{page_nums[0]}~{page_nums[-1]}），dpi={_DPI}", flush=True)
    all_elems = []
    for idx, pno in enumerate(page_nums, 1):
        if pno < 1 or pno > doc.page_count:
            continue
        t0 = time.time()
        try:
            raw, from_cache = _ocr_page_raw(doc, pno, cache_dir)
            els = _to_elements(raw, pno)
            all_elems.extend(els)
            tag = "缓存命中" if from_cache else "OCR"
            print(f"[ocr] 第 {pno} 页 ({idx}/{total}) {tag}  {len(els)} 元素  用时 {time.time()-t0:.2f}s",
                  flush=True)
        except Exception as ex:    # 单页失败不终止全篇
            print(f"[ocr] 第 {pno} 页 ({idx}/{total}) 失败：{ex}", flush=True)
            all_elems.append({"type": "Text", "page_no": pno, "text": "",
                              "caption": f"[page {pno} ocr failed: {ex}]",
                              "ocr_confidence": 0.0})
    print(f"[ocr] 本批完成：{total} 页 -> {len(all_elems)} 元素，总用时 {time.time()-t_batch:.1f}s",
          flush=True)
    return {"elements": all_elems}

@app.post("/parse_pages")
def parse_pages(req: PagesReq):
    return _parse(req.file_path, req.pages)

@app.post("/parse")
def parse(req: FileReq):
    return _parse(req.file_path, [])

@app.get("/health")
def health():
    return {"status": "ok", "engine": "v3"}
