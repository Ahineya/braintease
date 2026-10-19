#!/usr/bin/env python3
"""Browser sign-in for Fuse Chat. No API key and no access to Codex credentials.

The request names a guest host, not this Mac. Transfer the resulting 0600 file
into that guest and import it; the guest subsequently owns token refresh.
"""
import argparse
import base64
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import time
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler
from urllib.error import HTTPError
import uuid
import webbrowser

ISSUER = 'https://auth.openai.com'
RESOURCE = 'https://api.openai.com/v1'
SCOPES = 'openid profile email offline_access resource.invoke chatgpt.tokens.use.direct'


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def http_json(url, form=None):
    if urlsplit(url).scheme != 'https' or urlsplit(url).hostname != 'auth.openai.com':
        raise ValueError('Invalid OpenAI authentication endpoint')
    data = urlencode(form).encode() if form is not None else None
    request = Request(url, data=data, headers={'Accept': 'application/json', 'User-Agent': 'Fuse-Chat/0.1'})
    try:
        with build_opener(NoRedirect).open(request, timeout=30) as response:
            raw = response.read(1048577)
    except HTTPError as e:
        # Do not print token responses, authorization codes or request bodies.
        raise ValueError(f'OpenAI sign-in returned HTTP {e.code}; start a fresh sign-in') from None
    if len(raw) > 1048576:
        raise ValueError('Oversized authentication response')
    return json.loads(raw)


def unbase(s):
    if not isinstance(s, str) or any(c not in 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_' for c in s):
        raise ValueError('Invalid base64url')
    return base64.urlsafe_b64decode(s + '=' * (-len(s) % 4))


def base(b):
    return base64.urlsafe_b64encode(b).decode().rstrip('=')


def der(tag, payload):
    n = len(payload)
    length = bytes([n]) if n < 128 else bytes([128 + (n.bit_length() + 7) // 8]) + n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([tag]) + length + payload


def integer(b):
    b = b.lstrip(b'\0') or b'\0'
    return der(2, (b'\0' if b[0] & 128 else b'') + b)


def verify_id_token(token, client_id, nonce, jwks, now=None):
    """Verify RS256 with OpenSSL, then validate OIDC claims. Never decode-and-trust."""
    if not isinstance(token, str) or len(token) > 65536:
        raise ValueError('Invalid ID token')
    parts = token.split('.')
    if len(parts) != 3:
        raise ValueError('Invalid ID token')
    header, claims = (json.loads(unbase(p)) for p in parts[:2])
    if header.get('alg') != 'RS256' or header.get('crit'):
        raise ValueError('Unsupported ID-token signature algorithm')
    keys = [k for k in jwks.get('keys', []) if k.get('kid') == header.get('kid') and k.get('kty') == 'RSA'
            and k.get('use', 'sig') == 'sig' and k.get('alg', 'RS256') == 'RS256']
    if len(keys) != 1:
        raise ValueError('ID-token signing key not found')
    key = keys[0]
    n, e = unbase(key['n']), unbase(key['e'])
    if not 2048 <= int.from_bytes(n, 'big').bit_length() <= 8192 or int.from_bytes(e, 'big') != 65537:
        raise ValueError('Invalid ID-token signing key')
    rsa = der(48, integer(n) + integer(e))
    spki = der(48, bytes.fromhex('300d06092a864886f70d0101010500') + der(3, b'\0' + rsa))
    pem = b'-----BEGIN PUBLIC KEY-----\n' + base64.encodebytes(spki) + b'-----END PUBLIC KEY-----\n'
    with tempfile.TemporaryDirectory(prefix='fuse-chat-verify-') as temp:
        path = Path(temp)
        (path / 'key.pem').write_bytes(pem)
        (path / 'signature').write_bytes(unbase(parts[2]))
        check = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(path / 'key.pem'),
                                '-signature', str(path / 'signature')], input='.'.join(parts[:2]).encode(),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if check.returncode:
            raise ValueError('ID-token signature verification failed')
    now = time.time() if now is None else now
    audience = claims.get('aud')
    audiences = audience if isinstance(audience, list) else [audience]
    if (claims.get('iss') != ISSUER or client_id not in audiences
            or (len(audiences) > 1 and claims.get('azp') != client_id)
            or not isinstance(claims.get('exp'), (int, float)) or claims['exp'] <= now
            or claims.get('iat', now) > now + 120
            or not isinstance(claims.get('nonce'), str) or not hmac.compare_digest(claims['nonce'], nonce)
            or not isinstance(claims.get('sub'), str) or not claims['sub']):
        raise ValueError('ID-token identity, audience, expiry or nonce validation failed')
    return claims


def atomic_json(path, record):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as f:
            os.fchmod(f.fileno(), 0o600)
            json.dump(record, f, ensure_ascii=True)
            f.write('\n')
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def callback_values(query, state, previous_client=None):
    values = parse_qs(query, keep_blank_values=True)
    if any(len(v) != 1 for v in values.values()):
        raise ValueError('Duplicate OAuth callback parameters')
    values = {k: v[0] for k, v in values.items()}
    if not hmac.compare_digest(values.get('state', ''), state):
        raise ValueError('OAuth state mismatch')
    if values.get('error'):
        raise ValueError('Sign-in was declined or failed')
    client = values.get('client_id', previous_client)
    if not client or not client.startswith('oaiapp_') or (previous_client and client != previous_client):
        raise ValueError('Missing or mismatched issued client ID')
    if not values.get('code'):
        raise ValueError('Missing authorization code')
    return client, values['code']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--request', type=Path, required=True, help='public request exported by chat --login-request')
    p.add_argument('--output', type=Path, required=True, help='protected credential file to import into the guest')
    p.add_argument('--reauthorize', type=Path, help='this app’s existing credential record, exported from the same guest')
    p.add_argument('--no-browser', action='store_true')
    a = p.parse_args()
    os.umask(0o077)
    request = json.loads(a.request.read_text())
    host = request.get('ext_agent_host_id', '')
    if request.get('format') != 1 or not host.startswith('urn:uuid:'):
        raise ValueError('Invalid guest login request')
    uuid.UUID(host[9:])
    previous = json.loads(a.reauthorize.read_text()) if a.reauthorize else None
    if previous and (previous.get('ext_agent_host_id') != host or previous.get('issuer') != ISSUER):
        raise ValueError('Saved registration belongs to a different guest or issuer')
    verifier, state, nonce = (secrets.token_urlsafe(48) for _ in range(3))
    outcome = {}

    class Callback(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass  # Callback URLs carry the authorization code.

        def do_GET(self):
            parsed = urlsplit(self.path)
            if parsed.path != '/auth/callback':
                self.send_error(404)
                return
            try:
                client, code = callback_values(parsed.query, state, previous.get('client_id') if previous else None)
                outcome.update(client=client, code=code)
                body = b'Sign-in received. Return to the terminal to finish.'
            except ValueError as e:
                # A stray wrong-state request must not terminate the real login.
                self.send_error(400, str(e))
                if 'state mismatch' not in str(e):
                    outcome['error'] = str(e)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    with HTTPServer(('127.0.0.1', 0), Callback) as server:
        server.timeout = 1
        redirect = f'http://127.0.0.1:{server.server_port}/auth/callback'
        params = dict(client_id=previous['client_id'] if previous else 'dynamic_agent_client',
                      ext_agent_host_id=host, response_type='code', redirect_uri=redirect, scope=SCOPES,
                      resource=RESOURCE, state=state, nonce=nonce, code_challenge_method='S256',
                      code_challenge=base(hashlib.sha256(verifier.encode()).digest()))
        if previous:
            if previous.get('id_token'):
                params['id_token_hint'] = previous['id_token']
            if previous.get('email'):
                params['login_hint'] = previous['email']
        else:
            params['agent_name_hint'] = 'Fuse Chat'
        url = ISSUER + '/api/accounts/authorize?' + urlencode(params)
        print('Continue with ChatGPT in your Mac browser. Waiting up to ten minutes...', flush=True)
        if a.no_browser or not webbrowser.open(url):
            # This URL contains only a public PKCE challenge/state on initial login.
            # Returning login carries an ID-token hint: never print that URL.
            if previous:
                raise ValueError('Could not open browser for returning sign-in; enable a system browser')
            print(url, flush=True)
        deadline = time.monotonic() + 600
        while not outcome and time.monotonic() < deadline:
            server.handle_request()
    if not outcome:
        raise ValueError('Sign-in timed out')
    if 'error' in outcome:
        raise ValueError(outcome['error'])
    tokens = http_json(ISSUER + '/api/accounts/oauth/token', dict(grant_type='authorization_code',
                       client_id=outcome['client'], code=outcome['code'], code_verifier=verifier,
                       redirect_uri=redirect, resource=RESOURCE))
    discovery = http_json(ISSUER + '/.well-known/openid-configuration')
    if discovery.get('issuer') != ISSUER:
        raise ValueError('Unexpected OpenID issuer')
    claims = verify_id_token(tokens.get('id_token'), outcome['client'], nonce, http_json(discovery['jwks_uri']))
    if previous and claims['sub'] != previous.get('subject'):
        raise ValueError('Returning sign-in changed the selected account identity')
    scopes = tokens.get('scope', '').split()
    if not {'chatgpt.tokens.use.direct', 'resource.invoke'} <= set(scopes):
        raise ValueError('ChatGPT plan inference was not authorized')
    if (tokens.get('token_type', '').lower() != 'bearer'
            or not tokens.get('access_token') or not tokens.get('refresh_token')
            or not isinstance(tokens.get('expires_in'), int) or not 0 < tokens['expires_in'] <= 604800):
        raise ValueError('Incomplete token response')
    record = dict(email=claims.get('email', ''), issuer=ISSUER, subject=claims['sub'], client_id=outcome['client'],
                  ext_agent_host_id=host, id_token=tokens['id_token'], access_token=tokens['access_token'],
                  refresh_token=tokens['refresh_token'], token_type='Bearer', expires_in=tokens['expires_in'],
                  expires_at=int(time.time()) + tokens['expires_in'], scopes=scopes,
                  saved_at=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
    atomic_json(a.output, record)
    print(f'Signed in. Import {a.output} in that guest, then remove this transfer file. Tokens were not printed.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError) as e:
        raise SystemExit(f'Fuse Chat sign-in: {e}') from None
