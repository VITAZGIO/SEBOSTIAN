#!/usr/bin/env python3
"""Макет сервера Себастьяна для теста HTTP-клиента (seb_hc) на ПК.
Те же эндпоинты, что §6 ТЗ: /health, /stt, /ask_stream, /ask_text_stream.
Режим потока задаётся в пути запроса: ?mode=chunked|close|length.
"""
import base64
import io
import json
import socket
import struct
import sys
import threading
import time
import math


def wav24k(ms, hz=440):
    n = 24000 * ms // 1000
    pcm = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * hz * i / 24000))) for i in range(n))
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 24000, 48000, 2, 16)
    return hdr + b"data" + struct.pack("<I", len(pcm)) + pcm


def events():
    yield {"type": "stt", "text": "Себастьян, как дела?", "t": 0.9}
    yield {"type": "text", "text": "Всё отлично, сэр.", "t": 2.1, "chunks": 2}
    for ms in (1500, 700):
        yield {"type": "audio", "audio_b64": base64.b64encode(wav24k(ms)).decode(), "text": "Всё отлично, сэр.", "t_audio": 1.4}
    yield {"type": "done", "total": 6.0}


def handle(c):
    f = c.makefile("rb")
    line = f.readline().decode()
    method, path, _ = line.split(" ", 2)
    hdrs = {}
    while True:
        l = f.readline().decode().strip()
        if not l:
            break
        k, v = l.split(":", 1)
        hdrs[k.lower()] = v.strip()
    body = f.read(int(hdrs.get("content-length", "0")))
    mode = path.split("mode=")[1] if "mode=" in path else "chunked"
    p = path.split("?")[0]
    if p == "/health":
        b = json.dumps({"ok": True, "model": "qwen2.5:7b"}).encode()
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(b) + b)
    elif p == "/stt":
        ok = b"--" in body and b"RIFF" in body          # пришёл multipart с wav
        b = ('"Себастьян, включи свет"' if mode == "quoted"
             else json.dumps({"text": "Себастьян, включи свет" if ok else "НЕТ WAV"})).encode()
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(b) + b)
    elif p in ("/ask_stream", "/ask_text_stream"):
        time.sleep(1.5)                                  # «думает»
        data = b"".join(b"data: " + json.dumps(e, ensure_ascii=True).encode() + b"\n\n" for e in events())
        if mode == "chunked":
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n")
            evs = [b"data: " + json.dumps(e, ensure_ascii=True).encode() + b"\n\n" for e in events()]
            for i, ev in enumerate(evs):
                for k in range(0, len(ev), 7777):           # куски не совпадают с границами строк
                    part = ev[k:k + 7777]
                    c.sendall(b"%x\r\n" % len(part) + part + b"\r\n")
                if i == 1:
                    time.sleep(1.2)                         # пауза между событиями > 1 с
            c.sendall(b"0\r\n\r\n")
        elif mode == "close":
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + data)
        else:
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(data) + data)
    elif p == "/slow":
        time.sleep(30)
    c.close()


def main():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 18099))
    s.listen(8)
    print("ready", flush=True)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
