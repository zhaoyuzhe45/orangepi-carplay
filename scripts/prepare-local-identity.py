"""Import an RSA TLS identity or generate a local self-signed development identity.

Generated identities are NOT MFi certificates or evidence of CarPlay certification.
Existing certificate/key files are never overwritten by --generate.
"""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import tempfile


def openssl(executable, *args):
    result = subprocess.run([executable, *map(str, args)], capture_output=True)
    if result.returncode:
        raise ValueError('OpenSSL failed; verify the RSA PEM files and OpenSSL installation.')
    return result.stdout


def prepare(root, executable, generate):
    certs = root / 'config/certs'
    cert, key = certs / 'lockdown-cert.pem', certs / 'lockdown-key.pem'
    if generate:
        if cert.exists() or key.exists() or (root / 'src/lockdown_identity.h').exists() or (root / 'src/lockdown_tls_identity.h').exists():
            raise ValueError('An identity already exists; --generate refuses to replace it.')
        certs.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=certs) as temp:
            new_key, new_cert = Path(temp) / 'key.pem', Path(temp) / 'cert.pem'
            openssl(executable, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256',
                    '-days', '3650', '-subj', '/CN=CarPlay Box Development',
                    '-keyout', new_key, '-out', new_cert)
            new_key.replace(key)
            new_cert.replace(cert)
    if not cert.is_file() or not key.is_file():
        raise ValueError('Place lockdown-cert.pem and lockdown-key.pem in config/certs, or use --generate for development.')
    public = openssl(executable, 'pkey', '-in', key, '-pubout')
    cert_public = openssl(executable, 'x509', '-in', cert, '-pubkey', '-noout')
    if public.strip() != cert_public.strip():
        raise ValueError('Certificate and private key do not match; headers were not changed.')
    rsa_public = openssl(executable, 'rsa', '-in', key, '-RSAPublicKey_out')
    cert_text = openssl(executable, 'x509', '-in', cert, '-outform', 'PEM').decode('ascii')
    key_text = openssl(executable, 'pkey', '-in', key, '-outform', 'PEM').decode('ascii')
    identity = ('#ifndef LOCKDOWN_IDENTITY_H\n#define LOCKDOWN_IDENTITY_H\n\n'
                '#define LOCKDOWN_DEVICE_PUBLIC_KEY_BASE64 ' +
                json.dumps(base64.b64encode(rsa_public).decode('ascii')) + '\n\n#endif\n')
    tls = ('#ifndef LOCKDOWN_TLS_IDENTITY_H\n#define LOCKDOWN_TLS_IDENTITY_H\n\n'
           'static const char lockdown_tls_certificate_pem[] =\n    ' + json.dumps(cert_text) + ';\n\n'
           'static const char lockdown_tls_private_key_pem[] =\n    ' + json.dumps(key_text) + ';\n\n#endif\n')
    (root / 'src').mkdir(parents=True, exist_ok=True)
    for name, content in [('lockdown_identity.h', identity), ('lockdown_tls_identity.h', tls)]:
        target = root / 'src' / name
        temporary = target.with_suffix('.h.tmp')
        temporary.write_text(content, encoding='ascii', newline='\n')
        temporary.replace(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--openssl', default='openssl')
    parser.add_argument('--generate', action='store_true')
    args = parser.parse_args()
    try:
        prepare(args.root.resolve(), args.openssl, args.generate)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
    print('Local RSA TLS identity prepared; generated headers must remain ignored by Git.')


if __name__ == '__main__':
    main()
