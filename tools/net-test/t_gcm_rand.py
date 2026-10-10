# Checks t_gcm_rand's output against python-cryptography's AES-GCM.
import subprocess, sys
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
bad = 0; n = 0
for line in subprocess.run(['/tmp/t_gcm_rand'], capture_output=True, text=True).stdout.splitlines():
    k, nonce, aad, pt, ct, tag = line.split()
    k = bytes.fromhex(k); nonce = bytes.fromhex(nonce)
    aad = b'' if aad == '-' else bytes.fromhex(aad)
    pt = b'' if pt == '-' else bytes.fromhex(pt)
    ct = b'' if ct == '-' else bytes.fromhex(ct)
    want = AESGCM(k).encrypt(nonce, pt, aad)
    if want != ct + bytes.fromhex(tag):
        print('MISMATCH len(pt)=%d len(aad)=%d' % (len(pt), len(aad))); bad += 1
    n += 1
print('GCM vs openssl: %d cases, %s' % (n, 'FAILED' if bad else 'ok'))
sys.exit(1 if bad else 0)
