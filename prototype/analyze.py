#!/usr/bin/env python3
"""Switch 游戏截图英语分析器 —— 阶段一原型"""

import argparse
import base64
import os
import sys
from pathlib import Path

import anthropic

SUPPORTED_EXTENSIONS = {
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".webp": "image/webp",
    ".gif": "image/gif",
}

SYSTEM_PROMPT = """你是一位游戏翻译助手。

用户会发送 Switch 游戏截图。请识别截图中所有可见的英文文本（对话框、UI 文字、标识、字幕等），并直接输出自然流畅的中文翻译。

如果截图中没有英文文本，直接回复："截图中未检测到英文文本。"

只输出翻译内容，不需要其他说明。"""


def get_media_type(path: Path) -> str:
    ext = path.suffix.lower()
    media_type = SUPPORTED_EXTENSIONS.get(ext)
    if not media_type:
        supported = "、".join(SUPPORTED_EXTENSIONS.keys())
        print(f"错误：不支持的格式 '{ext}'，支持：{supported}", file=sys.stderr)
        sys.exit(1)
    return media_type


def analyze(image_path: str, model: str) -> None:
    path = Path(image_path)
    if not path.exists():
        print(f"错误：文件不存在：{image_path}", file=sys.stderr)
        sys.exit(1)

    if not os.environ.get("ANTHROPIC_API_KEY"):
        print("错误：请先设置环境变量 ANTHROPIC_API_KEY", file=sys.stderr)
        sys.exit(1)

    media_type = get_media_type(path)
    image_data = base64.standard_b64encode(path.read_bytes()).decode("utf-8")

    client = anthropic.Anthropic()

    print(f"分析中：{path.name}  (模型: {model})\n")

    response = client.messages.create(
        model=model,
        max_tokens=2048,
        system=SYSTEM_PROMPT,
        messages=[
            {
                "role": "user",
                "content": [
                    {
                        "type": "image",
                        "source": {
                            "type": "base64",
                            "media_type": media_type,
                            "data": image_data,
                        },
                    },
                    {"type": "text", "text": "请分析这张游戏截图中的英文内容。"},
                ],
            }
        ],
    )

    for block in response.content:
        if block.type == "text":
            print(block.text)

    print()
    print(
        f"Token 用量：输入 {response.usage.input_tokens} / "
        f"输出 {response.usage.output_tokens}"
    )


def main():
    parser = argparse.ArgumentParser(
        description="分析 Switch 游戏截图中的英文内容，输出翻译和词汇解析"
    )
    parser.add_argument("image", help="游戏截图路径（png/jpg/webp）")
    parser.add_argument(
        "--model",
        default="claude-sonnet-4-6",
        help="Claude 模型（默认：claude-sonnet-4-6，也可用 claude-opus-4-6 获得更高质量）",
    )
    args = parser.parse_args()
    analyze(args.image, args.model)


if __name__ == "__main__":
    main()
