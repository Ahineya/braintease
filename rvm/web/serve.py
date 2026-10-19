#!/usr/bin/env python3
"""Serve only the generated browser bundle, bound to localhost by default."""
from http.server import SimpleHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import argparse,functools
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--directory',type=Path,default=Path(__file__).resolve().parents[2]/'build/rvm-web/site')
p.add_argument('--port',type=int,default=8086)
a=p.parse_args();directory=a.directory.resolve()
if not all((directory/n).is_file() for n in ['rvm.wasm','disk.img.gz','index.html']):p.error('build the isolated browser site first')
class Handler(SimpleHTTPRequestHandler):
 def end_headers(self):
  self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');super().end_headers()
 def list_directory(self,path):self.send_error(404);return None
server=ThreadingHTTPServer(('127.0.0.1',a.port),functools.partial(Handler,directory=str(directory)))
print(f'RVM browser: http://127.0.0.1:{server.server_port}/',flush=True)
try:server.serve_forever()
except KeyboardInterrupt:pass
finally:server.server_close()
