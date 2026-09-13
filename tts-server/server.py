#!/usr/bin/env python3
"""本地 TTS 服务：游戏截图 → Vision OCR → say → wav。

给 OBS 插件的 F8 用。零 API 成本，Windows 端通过局域网共享同一个服务。
"""
import hashlib
import json
import os
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import Vision
from Foundation import NSData

PORT = int(os.environ.get("TTS_PORT", "8765"))
VOICE = os.environ.get("TTS_VOICE", "Zoe")
RATE = int(os.environ.get("TTS_RATE", "140"))
# 剧情对白在 Vision 里稳定是 conf 1.00 且成句；UI 标签词数不够，
# 被英文模型误认的日文竖排置信度只有 0.3~0.5。两条一起卡就够干净。
MIN_CONF = float(os.environ.get("TTS_MIN_CONF", "0.9"))
MIN_WORDS = int(os.environ.get("TTS_MIN_WORDS", "4"))
CACHE_DIR = os.path.expanduser("~/Library/Caches/obs-game-translator-tts")


def ocr(jpeg):
    data = NSData.dataWithBytes_length_(jpeg, len(jpeg))
    handler = Vision.VNImageRequestHandler.alloc().initWithData_options_(data, None)
    req = Vision.VNRecognizeTextRequest.alloc().init()
    req.setRecognitionLevel_(Vision.VNRequestTextRecognitionLevelAccurate)
    req.setRecognitionLanguages_(["en-US"])
    req.setUsesLanguageCorrection_(True)
    ok, _ = handler.performRequests_error_([req], None)
    if not ok:
        return []
    rows = []
    for o in req.results():
        c = o.topCandidates_(1)[0]
        bb = o.boundingBox()
        rows.append({
            "text": c.string(),
            "conf": float(c.confidence()),
            "y": 1.0 - float(bb.origin.y) - float(bb.size.height),
        })
    return rows


def pick_dialog(rows):
    keep = [r for r in rows
            if r["conf"] >= MIN_CONF and len(r["text"].split()) >= MIN_WORDS]
    keep.sort(key=lambda r: r["y"])
    return " ".join(r["text"] for r in keep)


def synth(text, voice, rate):
    os.makedirs(CACHE_DIR, exist_ok=True)
    key = hashlib.sha256(f"{voice}|{rate}|{text}".encode()).hexdigest()
    path = os.path.join(CACHE_DIR, key + ".wav")
    cached = os.path.exists(path)
    if not cached:
        subprocess.run(
            ["say", "-v", voice, "-r", str(rate), "-o", path,
             "--file-format=WAVE", "--data-format=LEI16@22050", text],
            check=True)
    with open(path, "rb") as f:
        return f.read(), cached


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, body=b"", ctype="application/octet-stream"):
        self.send_response(code)
        if body:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _params(self, q):
        return q.get("voice", [VOICE])[0], int(q.get("rate", [RATE])[0])

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        voice, rate = self._params(q)
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))

        if u.path == "/f8":
            rows = ocr(body)
            text = pick_dialog(rows)
            if not text:
                self.log_message("no dialog (%d raw rows)", len(rows))
                return self._send(204)
            wav, cached = synth(text, voice, rate)
            self.log_message("%s | %s", "HIT " if cached else "MISS", text)
            return self._send(200, wav, "audio/wav")

        if u.path == "/f8/debug":
            rows = ocr(body)
            out = {
                "all": sorted(rows, key=lambda r: r["y"]),
                "kept": pick_dialog(rows),
                "thresholds": {"min_conf": MIN_CONF, "min_words": MIN_WORDS},
            }
            return self._send(200, json.dumps(out, ensure_ascii=False, indent=2).encode(),
                              "application/json")

        if u.path == "/speak":
            text = body.decode("utf-8").strip()
            if not text:
                return self._send(400)
            wav, cached = synth(text, voice, rate)
            self.log_message("%s | %s", "HIT " if cached else "MISS", text)
            return self._send(200, wav, "audio/wav")

        self._send(404)

    def do_GET(self):
        if urlparse(self.path).path == "/health":
            return self._send(200, b'{"ok":true}', "application/json")
        self._send(404)


if __name__ == "__main__":
    print(f"TTS server on 0.0.0.0:{PORT}  voice={VOICE} rate={RATE} "
          f"conf>={MIN_CONF} words>={MIN_WORDS}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
