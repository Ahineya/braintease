"""Independent OpenSSL-generated keys and certificates, never installed normally."""
from pathlib import Path
import subprocess


def make(directory):
    directory=Path(directory)
    directory.mkdir(parents=True,exist_ok=True)
    def openssl(*args):
        subprocess.run(['openssl',*map(str,args)],cwd=directory,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    openssl('genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out','ca.key')
    openssl('req','-new','-x509','-key','ca.key','-sha256','-days','3','-subj','/CN=Ripple TLS Fixture Root',
            '-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign,cRLSign','-out','ca.pem')
    openssl('genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:prime256v1','-out','server.key')
    openssl('req','-new','-key','server.key','-subj','/CN=tls.test','-out','server.csr')
    (directory/'server.ext').write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:tls.test,IP:192.168.64.1\n')
    for name,days in [('server',2),('expired',-1)]:
        openssl('x509','-req','-in','server.csr','-CA','ca.pem','-CAkey','ca.key','-set_serial','42' if days>0 else '43',
                *(['-days',str(days)] if days>0 else ['-not_before','20000101000000Z','-not_after','20000102000000Z']),'-sha256','-extfile','server.ext','-out',name+'.pem')
    for serial,name,curve in [(44,'p384','secp384r1'),(45,'p521','secp521r1'),(46,'rsa',None)]:
        openssl('genpkey','-algorithm','EC' if curve else 'RSA','-pkeyopt',
                'ec_paramgen_curve:'+curve if curve else 'rsa_keygen_bits:2048','-out',name+'.key')
        openssl('req','-new','-key',name+'.key','-subj','/CN=tls.test','-out',name+'.csr')
        openssl('x509','-req','-in',name+'.csr','-CA','ca.pem','-CAkey','ca.key','-set_serial',str(serial),
                '-days','2','-sha256','-extfile','server.ext','-out',name+'.pem')
    openssl('req','-new','-x509','-newkey','rsa:2048','-noenc','-days','2',
            '-subj','/CN=Unrelated Root','-keyout','other.key','-out','other.pem',
            '-addext','basicConstraints=critical,CA:TRUE')
    openssl('req','-new','-key','ca.key','-subj','/CN=Ripple TLS Fixture Root','-out','cross.csr')
    (directory/'cross.ext').write_text('basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n')
    openssl('x509','-req','-in','cross.csr','-CA','other.pem','-CAkey','other.key',
            '-set_serial','47','-days','2','-extfile','cross.ext','-out','cross.pem')
    (directory/'cross-chain.pem').write_bytes((directory/'server.pem').read_bytes()+(directory/'cross.pem').read_bytes())
    for serial,name,extension in [(48,'wrong-san','subjectAltName=DNS:other.test\n'),(49,'no-san','')]:
        (directory/(name+'.ext')).write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n'+extension)
        openssl('x509','-req','-in','server.csr','-CA','ca.pem','-CAkey','ca.key',
                '-set_serial',str(serial),'-days','2','-extfile',name+'.ext','-out',name+'.pem')
    return directory
