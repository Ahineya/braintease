#!/usr/bin/env python3
"""Local, read-only terminal preview. No Telegram protocol runs here."""
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import secrets
import time
from urllib.parse import urlsplit
from live import PRIVATE, request


def main():
    token=secrets.token_urlsafe(24)
    base='/'+token
    document=('''<!doctype html><meta charset="utf-8"><title>Telegram · Fuse-X</title>
<style>body{background:#15191f;color:#eee;font:18px system-ui;text-align:center}
img{width:800px;max-width:95vw;image-rendering:pixelated}p{margin:14px auto}</style>
<h2>Telegram login</h2><p>Telegram → Settings → Devices → Link Desktop Device</p>
<img id="screen" alt="Live guest terminal"><p>The code refreshes automatically.</p>
<script>const picture=document.getElementById('screen');
function refresh(){picture.src=location.pathname+'screen.png?'+Date.now()}
picture.onload=()=>setTimeout(refresh,1000);picture.onerror=()=>setTimeout(refresh,2000);
refresh();</script>''').encode()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            route=urlsplit(self.path).path
            if route==base+'/':
                data=document;kind='text/html; charset=utf-8'
            elif route==base+'/screen.png':
                try:
                    result=request({'command':'snapshot'})
                    if not result['ok']:raise RuntimeError('Guest preview unavailable')
                    from pathlib import Path
                    path=Path(result['path'])
                    if path.parent!=PRIVATE:raise RuntimeError('Unexpected preview path')
                    data=path.read_bytes();kind='image/png'
                    path.unlink(missing_ok=True)
                except Exception:
                    self.send_error(503,'Guest preview unavailable');return
            else:
                self.send_error(404);return
            self.send_response(200)
            self.send_header('Content-Type',kind)
            self.send_header('Content-Length',str(len(data)))
            self.send_header('Cache-Control','no-store')
            self.send_header('Referrer-Policy','no-referrer')
            self.send_header('X-Content-Type-Options','nosniff')
            self.end_headers()
            try:self.wfile.write(data)
            except (BrokenPipeError,ConnectionResetError):pass

    server=HTTPServer(('127.0.0.1',0),Handler)
    server.timeout=1
    print(json.dumps({'url':f'http://127.0.0.1:{server.server_port}{base}/'}),flush=True)
    until=time.monotonic()+1800
    try:
        while time.monotonic()<until:server.handle_request()
    finally:server.server_close()


if __name__=='__main__':main()
