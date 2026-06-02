# PP-Structure OCR 服务（uv 管理）

rag2.0 的扫描件 OCR 后端：PaddleOCR(PP-StructureV3) + FastAPI。C++ 端通过 HTTP 调用。

## 安装与运行（uv）

> ⚠️ **重要：paddlepaddle 本体（尤其 GPU 版）不在 pyproject 里**——它是手动 `uv pip install` 的。
> 因此**不要用普通 `uv run` 或 `uv sync`**（它们会按 pyproject 做精确同步，把手动装的 paddle 卸掉）。
> 装依赖用 `uv sync --inexact`（保留手动装的包），跑服务用 `uv run --no-sync`。

```powershell
cd services\ppstructure

# 1. 装 pyproject 依赖（含 paddleocr>=3.0）；--inexact 不删手动装的 paddle
uv sync --inexact

# 2. 装 paddlepaddle 本体（二选一）
#    —— GPU 版（GTX 1660 Ti，按 CUDA 版本选 index，例 CUDA 11.8）：
uv pip install paddlepaddle-gpu -i https://www.paddlepaddle.org.cn/packages/stable/cu118/
#    —— CPU 版（慢但稳）：
# uv pip install paddlepaddle

# 3. 起服务（--no-sync：不重新同步、不卸掉手动装的 GPU paddle）
uv run --no-sync uvicorn app:app --host 0.0.0.0 --port 8001
```

健康检查：`curl http://localhost:8001/health` → `{"status":"ok","engine":"v3"}`（v3=PPStructureV3）。

> 注：首次构造 `PPStructureV3()` 会下载 3.x 模型（PP-OCRv5 等），存到 `C:\Users\<你>\.paddlex\`（3.x 用 paddlex，路径不是 `.paddleocr`），较慢。
> 升级/重装 paddleocr 后想验证，**别用 `uv run`**（会回退到 lock），用 venv python 直接验：`.venv\Scripts\python.exe -c "from paddleocr import PPStructureV3; import paddleocr; print(paddleocr.__version__)"`。

## 联调（验证字段映射 —— 唯一需按版本微调处）

服务起来后，用一份扫描 PDF 的某有内容页测试：
```powershell
curl -s -X POST http://localhost:8001/parse_pages -H "Content-Type: application/json" -d "{\"file_path\":\"D:/path/to/xxx.pdf\",\"pages\":[91]}"
```
应返回 `{"elements":[...]}`。若 `elements` 为空或字段不对，按 PP-StructureV3 的**实际返回结构**改 `app.py` 里的 `_to_elements`（不同 PaddleOCR 版本的 block 字段名不一样）。

## 与 C++ 端的契约

- 端点：`POST /parse_pages {file_path, pages:[1-based]}`、`POST /parse {file_path}`、`GET /health`
- 元素 `type` 区分大小写：`Heading` / `Text` / `Table` / `Formula` / `Figure`
- 失败：`{"error": "..."}`
- 详见 C++ 端 `src/parse/ppstructure_backend.cpp` 的 `parse_ppstructure_json`。
