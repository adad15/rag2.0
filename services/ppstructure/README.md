# PP-Structure OCR 服务（uv 管理）

rag2.0 的扫描件 OCR 后端：PaddleOCR(PP-StructureV3) + FastAPI。C++ 端通过 HTTP 调用。

## 安装与运行（uv）

```powershell
cd services\ppstructure

# 1. 建虚拟环境并装 pyproject 里的依赖（fastapi/uvicorn/paddleocr/pymupdf/...）
uv sync

# 2. 装 paddlepaddle 本体（二选一；不放 pyproject 因 GPU wheel 需专用 index）
#    —— CPU 版（稳、装得上、慢，先验证链路用它最省事）：
uv pip install paddlepaddle
#    —— GPU 版（GTX 1660 Ti，按你的 CUDA 版本选 index，例：CUDA 11.8）：
# uv pip install paddlepaddle-gpu -i https://www.paddlepaddle.org.cn/packages/stable/cu118/
#    其它 CUDA 版把 cu118 换成 cu126 等；确切命令以飞桨官网安装选择器为准。

# 3. 起服务（从本目录跑，app:app 指 app.py 里的 app）
uv run uvicorn app:app --host 0.0.0.0 --port 8001
```

健康检查：`curl http://localhost:8001/health` → `{"status":"ok"}`。

> 注：`uv sync` 可能因 paddleocr 的依赖声明顺带拉一个 CPU 版 paddlepaddle；要用 GPU 就在第 2 步用 GPU wheel 覆盖。首次构造 `PPStructureV3()` 会下载模型，较慢。

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
