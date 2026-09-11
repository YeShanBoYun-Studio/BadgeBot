#!/usr/bin/env python3
"""BadgeBot V2 语音后端 —— 跑在电脑上,配合工牌的「语音」页使用。

流程:工牌按键说话 → PCM 经局域网明文 HTTP 直传本脚本 → 交给识别引擎 →
把识别出的文字直接打进电脑当前聚焦的窗口。徽章侧零依赖、无需 TLS。

用法:
    python tools/badge_voice.py --engine echo
    python tools/badge_voice.py --engine openai --api-key SK-xxx \
        --base-url https://api.openai.com/v1 --model whisper-1 --lang zh
    python tools/badge_voice.py --engine faster-whisper --model small --lang zh

之后在配网门户「语音」里填 http://<这台电脑的IP>:8722 即可。

依赖:
    - pynput(注入键盘;--no-type 可关)
    - openai 引擎:仅 Python 标准库(urllib 手工 multipart),兼容任何
      OpenAI /audio/transcriptions 形状的服务(OpenAI、Groq、硅基流动等)
    - faster-whisper 引擎:pip install faster-whisper
"""

import argparse
import io
import json
import sys
import urllib.request
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ARGS = None          # 由 main() 填充
TYPE_FN = None       # inject(text) 或 None
TRANSCRIBE_FN = None # transcribe(pcm_bytes, rate, bits, ch) -> str


# ---------------- 文本注入 ----------------

def make_typer(no_type: bool):
    if no_type:
        return None
    try:
        from pynput.keyboard import Controller
        kb = Controller()

        def inject(text: str):
            kb.type(text)
        return inject
    except Exception as e:  # pynput 缺失/无桌面环境:只打印不注入
        print(f"[warn] 键盘注入不可用({e}),只显示识别结果", file=sys.stderr)
        return None


# ---------------- 识别引擎 ----------------

def _validated_base_url(raw: str) -> str:
    """命令行给的服务地址只放行 http(s) + 主机名形式。

    本工具在用户自己的电脑上、指向用户自己选择的 ASR 服务(含局域网自建,
    如 whisper.cpp 服务器),因此不阻断私网地址;但协议白名单、禁 userinfo、
    禁重定向可以挡住 file:// 等奇怪 scheme 与跳转劫持。
    """
    u = urlparse(raw)
    if u.scheme not in ("http", "https") or not u.hostname:
        raise SystemExit(f"[error] --base-url 必须是 http(s)://host[:port] 形式:{raw}")
    if u.username or u.password:
        raise SystemExit("[error] --base-url 不允许携带用户名/密码")
    return raw


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **kw):
        return None


def _post_form(url: str, body: bytes, content_type: str, api_key: str) -> dict:
    opener = urllib.request.build_opener(_NoRedirect)
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": content_type,
        },
    )
    with opener.open(req, timeout=60) as resp:
        return json.loads(resp.read().decode("utf-8"))


def make_openai_asr(args):
    base = _validated_base_url(args.base_url).rstrip("/")

    def transcribe(pcm: bytes, rate: int, bits: int, ch: int) -> str:
        wav_buf = io.BytesIO()
        with wave.open(wav_buf, "wb") as w:
            w.setnchannels(ch)
            w.setsampwidth(bits // 8)
            w.setframerate(rate)
            w.writeframes(pcm)
        wav_bytes = wav_buf.getvalue()

        boundary = "BadgeBotFormBoundary7d1a2c"
        parts = [
            ("model", None, None, ARGS.model.encode()),
            ("language", None, None, ARGS.lang.encode() if ARGS.lang else None),
            ("response_format", None, None, b"json"),
            ("file", "audio.wav", "audio/wav", wav_bytes),
        ]
        body = io.BytesIO()
        for name, filename, ctype, data in parts:
            if data is None:
                continue
            body.write(f"--{boundary}\r\n".encode())
            if filename:
                body.write(
                    f'Content-Disposition: form-data; name="{name}"; '
                    f'filename="{filename}"\r\nContent-Type: {ctype}\r\n\r\n'.encode()
                )
            else:
                body.write(
                    f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode()
                )
            body.write(data)
            body.write(b"\r\n")
        body.write(f"--{boundary}--\r\n".encode())

        result = _post_form(
            base + "/audio/transcriptions",
            body.getvalue(),
            f"multipart/form-data; boundary={boundary}",
            ARGS.api_key,
        )
        return result.get("text", "")

    return transcribe


def make_faster_whisper_asr(args):
    try:
        import numpy as np
        from faster_whisper import WhisperModel
    except ImportError as e:
        sys.exit(f"[error] faster-whisper 未安装:pip install faster-whisper({e})")

    print(f"[info] 加载 faster-whisper 模型 {args.model}(首次会下载)…")
    model = WhisperModel(args.model, device="auto", compute_type="auto")

    def transcribe(pcm: bytes, rate: int, bits: int, ch: int) -> str:
        assert bits == 16, "faster-whisper 引擎目前只支持 16bit 采样"
        arr = np.frombuffer(pcm, dtype=np.int16).astype("float32") / 32768.0
        if ch > 1:
            arr = arr.reshape(-1, ch).mean(axis=1)
        segments, _info = model.transcribe(
            arr, language=args.lang or None, beam_size=1
        )
        return "".join(s.text for s in segments)

    return transcribe


def make_echo_asr(args):
    def transcribe(pcm: bytes, rate: int, bits: int, ch: int) -> str:
        return f"[echo] {len(pcm)}B {rate}Hz/{bits}bit x{ch}"
    return transcribe


ENGINES = {
    "echo": make_echo_asr,
    "openai": make_openai_asr,
    "faster-whisper": make_faster_whisper_asr,
}


# ---------------- HTTP 服务 ----------------

def read_body(handler) -> bytes:
    """支持 chunked(工牌边录边传)与 Content-Length 两种编码。"""
    te = (handler.headers.get("Transfer-Encoding") or "").lower()
    if "chunked" in te:
        out = bytearray()
        while True:
            line = handler.rfile.readline(65536).strip()
            if b";" in line:
                line = line.split(b";", 1)[0]
            size = int(line, 16)
            if size == 0:
                handler.rfile.readline()  # 结尾 CRLF
                break
            out += handler.rfile.read(size)
            handler.rfile.read(2)
        return bytes(out)
    length = int(handler.headers.get("Content-Length") or 0)
    return handler.rfile.read(length) if length > 0 else b""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _reply(self, code: int, obj: dict):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self._reply(200, {"service": "badge-voice", "engine": ARGS.engine})

    def do_POST(self):
        u = urlparse(self.path)
        if u.path != "/voice":
            self._reply(404, {"error": "not found"})
            return
        q = parse_qs(u.query)
        rate = int((q.get("rate") or ["16000"])[0])
        bits = int((q.get("bits") or ["16"])[0])
        ch = int((q.get("ch") or ["1"])[0])

        try:
            pcm = read_body(self)
        except Exception as e:
            self._reply(400, {"error": f"bad body: {e}"})
            return
        if not pcm:
            self._reply(400, {"error": "empty body"})
            return

        dur = len(pcm) / (rate * (bits // 8) * ch)
        print(f"[voice] 收到 {len(pcm)} 字节 ≈ {dur:.1f}s "
              f"({rate}Hz/{bits}bit x{ch}),识别中…")
        try:
            text = (TRANSCRIBE_FN or (lambda *a: ""))(pcm, rate, bits, ch).strip()
        except Exception as e:
            print(f"[error] 识别失败:{e}", file=sys.stderr)
            self._reply(500, {"error": str(e)})
            return

        typed = False
        if text and TYPE_FN:
            try:
                TYPE_FN(text)
                typed = True
            except Exception as e:
                print(f"[warn] 注入失败:{e}", file=sys.stderr)
        print(f"[voice] 文本:{text!r}" + ("(已打字)" if typed else ""))

        self._reply(200, {"text": text, "typed": typed, "seconds": round(dur, 1)})

    def log_message(self, fmt, *args):  # 静默默认访问日志
        pass


def main():
    global ARGS, TYPE_FN, TRANSCRIBE_FN
    p = argparse.ArgumentParser(description="BadgeBot voice backend")
    p.add_argument("--port", type=int, default=8722)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--engine", choices=list(ENGINES), default="echo")
    p.add_argument("--api-key", default="", help="openai 引擎的 API key")
    p.add_argument("--base-url", default="https://api.openai.com/v1",
                   help="openai 引擎的服务地址(可指向任意兼容服务)")
    p.add_argument("--model", default="whisper-1",
                   help="openai: 模型名;faster-whisper: 模型规格(small/medium…)")
    p.add_argument("--lang", default="zh", help="识别语言(zh/en/…;空 = 自动)")
    p.add_argument("--no-type", action="store_true", help="只识别,不注入键盘")
    ARGS = p.parse_args()

    if ARGS.engine == "openai" and not ARGS.api_key:
        sys.exit("[error] openai 引擎需要 --api-key")

    TYPE_FN = make_typer(ARGS.no_type)
    TRANSCRIBE_FN = ENGINES[ARGS.engine](ARGS)

    server = ThreadingHTTPServer((ARGS.host, ARGS.port), Handler)
    print(f"[info] BadgeBot 语音后端 http://0.0.0.0:{ARGS.port} 引擎={ARGS.engine} "
          f"注入={'关' if ARGS.no_type else ('开' if TYPE_FN else '不可用')}")
    print(f"[info] 在配网门户「语音」里填 http://<本机IP>:{ARGS.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[info] 退出")


if __name__ == "__main__":
    main()
