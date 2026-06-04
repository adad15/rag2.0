"""PP-StructureV3(PaddleOCR 3.x) FastAPI 服务：把扫描页 OCR 成归一化元素流，供 rag2.0 C++ 端调用。

契约（与 C++ 端 ppstructure_backend.cpp / parse_ppstructure_json 对齐）：
  POST /parse_pages {"file_path": "...", "pages": [1-based 页号...]}  -> {"elements": [...]}
  POST /parse       {"file_path": "..."}                              -> {"elements": [...]}  (全篇)
  GET  /health                                                        -> {"status":"ok","engine":"v3"}
元素字段：type ∈ {Heading,Text,Table,Formula,Figure}(区分大小写)、page_no、raw_label、level、
  clause_no、title、text、table_html、caption、ocr_confidence。失败用 {"error": "..."}。

依赖：paddleocr>=3.0 + paddlex[ocr] + paddlepaddle(-gpu)。构造 PPStructureV3() 首次会下载模型到 ~/.paddlex。
"""
import os
import time
from fastapi import FastAPI
from pydantic import BaseModel
import fitz                      # PyMuPDF：渲染指定页为图（免系统 poppler）
import numpy as np
from PIL import Image
from paddleocr import PPStructureV3

app = FastAPI()
_engine = PPStructureV3()        # 首次构造会下载模型，较慢

class PagesReq(BaseModel):
    file_path: str
    pages: list[int] = []        # 1-based；空表示全篇

class FileReq(BaseModel):
    file_path: str

# 去预览水印：水印为浅灰斜纹(亮度高)，正文为黑。亮度 > 阈值的像素置白。
# 0 = 关闭；环境变量 RAG_WM_THRESHOLD 可覆盖（这类规范常见水印实测 170~190 干净）。
_WM_THRESHOLD = int(os.environ.get("RAG_WM_THRESHOLD", "180"))

def _render_page(doc, page_index_0based, dpi=250):
    page = doc[page_index_0based]
    pix = page.get_pixmap(dpi=dpi)
    arr = np.array(Image.frombytes("RGB", [pix.width, pix.height], pix.samples))
    if _WM_THRESHOLD > 0:
        lum = arr @ np.array([0.299, 0.587, 0.114])   # 亮度
        arr[lum > _WM_THRESHOLD] = 255                # 浅灰水印/背景 -> 纯白，保留深色正文
    return arr

def _to_elements(result, page_no):
    """把 PP-StructureV3 单页结果映射为 IR 元素。
       如实上报：透传原始 block_label 到 raw_label、透传真实置信度；语义判断交 C++。"""
    res0 = result[0] if isinstance(result, (list, tuple)) and result else result
    d = getattr(res0, "json", None)
    if not isinstance(d, dict):
        return []
    d = d.get("res", d)
    blocks = d.get("parsing_res_list") or []
    out = []
    for b in blocks:
        if not isinstance(b, dict):
            continue
        raw = (b.get("block_label") or "text")
        label = raw.lower()
        content = b.get("block_content", "") or ""
        # 真实置信度：优先块级分数，缺失/非数（含数字字符串）则置 0.0（而非伪 1.0），便于 C++ 端识别"无分数"
        conf = b.get("block_score", b.get("score", None))
        try:
            conf = float(conf)
        except (TypeError, ValueError):
            conf = 0.0
        base = {"page_no": page_no, "raw_label": raw, "ocr_confidence": conf}
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

def _parse(file_path, pages):
    if not os.path.exists(file_path):
        return {"error": f"file not found: {file_path}"}
    doc = fitz.open(file_path)
    page_nums = pages if pages else list(range(1, doc.page_count + 1))
    total = len(page_nums)
    t_batch = time.time()
    print(f"[ocr] 收到请求：{total} 页（{page_nums[0]}~{page_nums[-1]}），dpi=250", flush=True)
    all_elems = []
    for idx, pno in enumerate(page_nums, 1):
        if pno < 1 or pno > doc.page_count:
            continue
        t0 = time.time()
        try:
            img = _render_page(doc, pno - 1)
            els = _to_elements(_engine.predict(input=img), pno)
            all_elems.extend(els)
            print(f"[ocr] 第 {pno} 页 ({idx}/{total}) OK  {len(els)} 元素  用时 {time.time()-t0:.1f}s",
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
