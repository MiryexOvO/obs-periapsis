"""Create and read a versioned PERI publisher invitation."""
import argparse
import hashlib
from urllib.parse import urlsplit
import base64
import binascii
import ipaddress
import json
import os
import sys
from pathlib import Path

PREFIX = "peri://v1/"


def short_settings(server, key):
    # Keep TLS trust separate from the short server URL.
    # 短服务器地址与 TLS 信任证书分开配置。
    url = urlsplit(server.strip())
    if (url.scheme != "peri" or not url.hostname or url.path not in ("", "/")
            or url.username is not None or url.query or url.fragment):
        raise ValueError("服务器地址格式为 peri://IP 或 peri://IP:端口")
    host = ipaddress.ip_address(url.hostname)
    if host.is_unspecified:
        raise ValueError("请把监听地址换成 OBS 可访问的服务器 IP；同机可填 127.0.0.1")
    port = 4444 if url.port is None else url.port
    if port == 0:
        raise ValueError("服务器端口范围为 1..65535")
    if not 1 <= len(key.encode("utf8")) <= 128 or not key.isprintable():
        raise ValueError("推流码应为 1 到 128 字节的可打印文本")
    address = f"[{host}]:{port}" if host.version == 6 else f"{host}:{port}"
    return address, hashlib.sha256(b"peri-stream-key-v1\0" + key.encode("utf8")).hexdigest()


def parse(link):
    try:
        if len(link) > 32768 or not link.startswith(PREFIX):
            raise ValueError
        body = link[len(PREFIX):]
        raw = base64.b64decode(body + "=" * (-len(body) % 4), altchars=b"-_", validate=True)
        data = json.loads(raw)
        address, name, resource, token, cert = (data[k] for k in
                                               ("address", "name", "resource", "token", "cert"))
        host, port = address.rsplit(":", 1)
        ip = ipaddress.ip_address(host.strip("[]"))
        if host != (f"[{ip}]" if ip.version == 6 else str(ip)) or not 1 <= int(port) <= 65535:
            raise ValueError
        for field, limit in ((name, 253), (resource, 256), (token, 128)):
            if not isinstance(field, str) or not 1 <= len(field) <= limit or not all("!" <= c <= "~" for c in field):
                raise ValueError
        der = base64.b64decode(cert, validate=True)
        if len(token) < 32 or not der or len(der) > 16384:
            raise ValueError
        return address, name, resource, token, der
    except (KeyError, TypeError, ValueError, AttributeError, binascii.Error):
        # 解析错误不可回显令牌。 / Never echo credentials in parser errors.
        raise ValueError("推流码无效，请重新生成完整的 peri://v1/ 地址") from None


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate a private PERI publishing link")
    parser.add_argument("address")
    parser.add_argument("server_name")
    parser.add_argument("cert", type=Path)
    parser.add_argument("resource")
    args = parser.parse_args()
    data = dict(address=args.address, name=args.server_name, resource=args.resource,
                cert=base64.b64encode(args.cert.read_bytes()).decode(),
                token=os.environ.get("PERI_PUBLISH_TOKEN", ""))
    link = PREFIX + base64.urlsafe_b64encode(json.dumps(data).encode()).decode().rstrip("=")
    parse(link)
    sys.stdout.write(link + "\n")
