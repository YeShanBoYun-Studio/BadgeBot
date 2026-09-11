"""badge_voice.py 回环冒烟测试(echo 引擎 + chunked 上传)。

手动运行,不进 validate.sh 门禁(会绑端口、起进程):
    python tests/test_badge_voice_loopback.py

验证后端的 chunked 解码路径与应答格式,模拟工牌的分块上传行为。
"""
import json
import struct
import subprocess
import sys
import time
from urllib.parse import urlparse

PORT = 18799


def chunked_request(url: str, chunks: list[bytes]):
    """urllib 不支持 chunked 上传,用 http.client 手工分块。"""
    import http.client
    u = urlparse(url)
    conn = http.client.HTTPConnection(u.hostname, u.port, timeout=10)
    conn.putrequest("POST", u.path + ("?" + u.query if u.query else ""))
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Transfer-Encoding", "chunked")
    conn.endheaders()
    for c in chunks:
        conn.send(b"%x\r\n" % len(c) + c + b"\r\n")
    conn.send(b"0\r\n\r\n")
    resp = conn.getresponse()
    body = resp.read()
    conn.close()
    return resp.status, json.loads(body.decode("utf-8"))


def main() -> int:
    proc = subprocess.Popen(
        [sys.executable, "tools/badge_voice.py", "--engine", "echo",
         "--port", str(PORT)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    time.sleep(2.0)
    ok = True
    try:
        # 模拟工牌:两块 PCM chunk 按 chunked 编码上传(1s 的 16k/16bit/单声道)
        pcm = struct.pack("<h", 0) * 16000
        status, reply = chunked_request(
            f"http://127.0.0.1:{PORT}/voice?rate=16000&bits=16&ch=1",
            [pcm[:16000], pcm[16000:]],
        )
        print("status:", status, "reply:", reply)
        ok &= status == 200
        ok &= reply.get("text") == "[echo] 32000B 16000Hz/16bit x1"
        ok &= abs(reply.get("seconds", 0) - 1.0) < 0.01

        # 空体 → 400
        status2, _ = chunked_request(f"http://127.0.0.1:{PORT}/voice", [b""])
        print("empty status:", status2)
        ok &= status2 == 400
    finally:
        proc.terminate()
    print("ECHO-LOOPBACK", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
