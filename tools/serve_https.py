#!/usr/bin/env python3
"""
自签 HTTPS 静态服务器：给 tools/webhid-test.html 提供 secure context。

WebHID / Web Serial 只在 secure context（https / localhost / file://）可用，
局域网 IP 的 HTTP 不行。本脚本用 openssl 生成自签证书后以 TLS 提供 tools/
目录；首次访问浏览器会警告"不安全"，点"高级 → 继续前往"即可（自签证书
只用于本地测试）。

用法：
    python3 tools/serve_https.py [端口]     # 默认 8443
    然后打开 https://<本机IP>:8443/webhid-test.html
"""

import functools
import http.server
import os
import ssl
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CERT = os.path.join(HERE, "serve_https.pem")
KEY = os.path.join(HERE, "serve_https-key.pem")


def ensure_cert():
    if os.path.exists(CERT) and os.path.exists(KEY):
        return
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", KEY, "-out", CERT, "-days", "3650",
         "-subj", "/CN=pico-io-local"],
        check=True,
    )
    print(f"已生成自签证书: {CERT}")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8443
    ensure_cert()

    handler = functools.partial(
        http.server.SimpleHTTPRequestHandler, directory=HERE)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    print(f"serving {HERE} at https://0.0.0.0:{port}/  (Ctrl+C 退出)")
    print(f"打开 https://localhost:{port}/webhid-test.html 或 https://<本机IP>:{port}/webhid-test.html")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
