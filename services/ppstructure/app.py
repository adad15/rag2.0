"""PP-Structure(PaddleOCR) FastAPI 服务：把扫描页 OCR 成归一化元素流，供 rag2.0 C++ 端调用。

契约（与 C++ 端 ppstructure_backend.cpp / parse_ppstructure_json 对齐）：
  POST /parse_pages {"file_path": "...", "pages": [1-based 页号...]}  -> {"elements": [...]}
  POST /parse       {"file_path": "..."}                              -> {"elements": [...]}  (全篇)
  GET  /health                                                        -> {"status": "ok"}
元素字段：type ∈ {Heading,Text,Table,Formula,Figure}(区分大小写)、page_no、level、
  clause_no、title、text、table_html、caption、ocr_confidence。失败用 {"error": "..."}。
"""
import os
from fastapi import FastAPI
from pydantic import BaseModel
import fitz                      # PyMuPDF：渲染指定页为图（免系统 poppler）
import numpy as np
from PIL import Image
from paddleocr import PPStructureV3

app = FastAPI()
_pipeline = PPStructureV3()      # 首次构造会下载/加载模型，较慢

class PagesReq(BaseModel):
    file_path: str
    pages: list[int] = []        # 1-based；空表示全篇

class FileReq(BaseModel):
    file_path: str

def _render_page(doc, page_index_0based, dpi=200):
    page = doc[page_index_0based]
    pix = page.get_pixmap(dpi=dpi)
    img = Image.frombytes("RGB", [pix.width, pix.height], pix.samples)
    return np.array(img)

def _to_elements(result, page_no):
    """把 PP-StructureV3 单页结果映射为 IR 元素。字段名随 PaddleOCR 版本不同需微调。"""
    elements = []
    blocks = []
    if isinstance(result, dict):
        blocks = result.get("parsing_res_list") or result.get("layout") or []
    elif isinstance(result, list):
        blocks = result
    for b in blocks:
        btype = (b.get("type") or b.get("label") or "text").lower()
        if "table" in btype:
            elements.append({"type": "Table", "page_no": page_no,
                             "table_html": b.get("html", b.get("res", "")) or "",
                             "caption": "", "text": "", "ocr_confidence": 1.0})
        elif "title" in btype or "header" in btype:
            txt = b.get("text", "") or ""
            elements.append({"type": "Heading", "page_no": page_no, "level": 1,
                             "title": txt, "text": txt, "ocr_confidence": 1.0})
        else:
            txt = b.get("text", "") or ""
            if txt.strip():
                elements.append({"type": "Text", "page_no": page_no,
                                 "text": txt, "ocr_confidence": 1.0})
    return elements

def _parse(file_path, pages):
    if not os.path.exists(file_path):
        return {"error": f"file not found: {file_path}"}
    doc = fitz.open(file_path)
    page_nums = pages if pages else list(range(1, doc.page_count + 1))
    all_elems = []
    for pno in page_nums:
        if pno < 1 or pno > doc.page_count:
            continue
        try:
            img = _render_page(doc, pno - 1)
            res = _pipeline.predict(input=img)
            res0 = res[0] if isinstance(res, list) and res else res
            all_elems.extend(_to_elements(res0, pno))
        except Exception as ex:    # 单页失败不终止全篇
            all_elems.append({"type": "Text", "page_no": pno, "text": "",
                              "caption": f"[page {pno} ocr failed: {ex}]",
                              "ocr_confidence": 0.0})
    return {"elements": all_elems}

@app.post("/parse_pages")
def parse_pages(req: PagesReq):
    return _parse(req.file_path, req.pages)

@app.post("/parse")
def parse(req: FileReq):
    return _parse(req.file_path, [])

@app.get("/health")
def health():
    return {"status": "ok"}
