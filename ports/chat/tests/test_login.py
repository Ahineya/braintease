#!/usr/bin/env python3
"""Offline OAuth checks with an independently generated OpenSSL signing key."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
spec = importlib.util.spec_from_file_location('fuse_chat_login', Path(__file__).resolve().parents[1] / 'login.py')
login = importlib.util.module_from_spec(spec)
spec.loader.exec_module(login)


class LoginTests(unittest.TestCase):
    def test_callback(self):
        self.assertEqual(login.callback_values('state=x&code=y&client_id=oaiapp_test', 'x'), ('oaiapp_test', 'y'))
        self.assertEqual(login.callback_values('state=x&code=y', 'x', 'oaiapp_test'), ('oaiapp_test', 'y'))
        for query in ('state=bad&code=y&client_id=oaiapp_test', 
                      'state=x&code=y&client_id=dynamic_agent_client', 'state=x&state=x&code=y',
                      'state=x&error=access_denied', 'state=x&code=y&client_id=oaiapp_other'):
            with self.assertRaises(ValueError):
                login.callback_values(query, 'x', 'oaiapp_test')

    def test_signature_and_claims(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048',
                            '-out', str(path / 'private')], check=True, capture_output=True)
            modulus = subprocess.check_output(['openssl', 'rsa', '-in', str(path / 'private'), '-noout', '-modulus']).decode().strip().split('=')[1]
            jwks = dict(keys=[dict(kty='RSA', kid='fixture', n=login.base(bytes.fromhex(modulus)), e='AQAB')])
            claims = dict(iss=login.ISSUER, sub='user', aud='oaiapp_test', exp=2000, iat=500, nonce='nonce')

            def token(value):
                unsigned = '.'.join(login.base(json.dumps(part).encode()) for part in (dict(alg='RS256', kid='fixture'), value))
                sig = subprocess.check_output(['openssl', 'dgst', '-sha256', '-sign', str(path / 'private')], input=unsigned.encode())
                return unsigned + '.' + login.base(sig)

            signed = token(claims)
            self.assertEqual(login.verify_id_token(signed, 'oaiapp_test', 'nonce', jwks, now=1000), claims)
            for changed in [dict(claims, iss='other'), dict(claims, exp=999), dict(claims, nonce='other'),
                            dict(claims, aud='other'), dict(claims, sub=''), dict(claims, aud=['oaiapp_test', 'other'])]:
                with self.assertRaises(ValueError):
                    login.verify_id_token(token(changed), 'oaiapp_test', 'nonce', jwks, now=1000)
            with self.assertRaises(ValueError):
                parts = signed.split('.')
                login.verify_id_token(parts[0] + '.' + login.base(b'{}') + '.' + parts[2], 'oaiapp_test', 'nonce', jwks, now=1000)
            login.atomic_json(path / 'account.json', dict(test=True))
            self.assertEqual(os.stat(path / 'account.json').st_mode & 0o777, 0o600)
            self.assertEqual(json.loads((path / 'account.json').read_text()), dict(test=True))


if __name__ == '__main__':
    unittest.main()
