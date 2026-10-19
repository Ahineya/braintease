#!/usr/bin/env python3
"""Host-only worker protocol fixture. Guest TLS tests use the real htget."""
import json
import sys
from urllib.parse import urlsplit

url = urlsplit(sys.argv[-2]); path = url.path
headers = dict(pair.split(': ', 1) for pair in sys.argv[1:-2] if ': ' in pair)
status, fields, body = '200 OK', [], b''
if path == '/binary': body = bytes([0, 65, 255, 195, 169])
elif path == '/empty': status = '204 No Content'
elif path == '/bad-json': body = b'{'
elif path == '/failed':
    sys.stdout.buffer.write(b'HTTP/1.1 200 OK\r\n\r\npartial')
    sys.exit(1)
elif path == '/bad-headers':
    sys.stdout.buffer.write(b'garbage\r\n\r\nbody')
    sys.exit(0)
elif path == '/redirect': status, fields = '302 Found', [('Location', '/echo?query=yes')]
elif path == '/relative/file': status, fields = '307 Temporary Redirect', [('Location', '../echo?query=yes#fragment')]
elif path == '/query': status, fields = '302 Found', [('Location', '?answer=42')] if not url.query else []
elif path == '/loop': status, fields = '302 Found', [('Location', '/loop')]
elif path == '/cross': status, fields = '302 Found', [('Location', 'http://other.test/echo')]
else: body = json.dumps(dict(url=sys.argv[-2], headers=headers)).encode()
if path == '/query' and url.query: status, body = '200 OK', url.query.encode()
if path == '/missing': status = '404 Not Found'
response = 'HTTP/1.1 ' + status + '\r\n' + ''.join(k + ': ' + v + '\r\n' for k, v in fields) + 'Content-Length: ' + str(len(body)) + '\r\n\r\n'
sys.stdout.buffer.write(response.encode() + body)
