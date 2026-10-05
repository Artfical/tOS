#!/usr/bin/env python3
"""Cross-checks kernel/net/rsa.c against openssl and Python big integers."""
import subprocess, os, sys, random, hashlib, tempfile, re
BIN = "/tmp/t_rsa"
def run(*a):
    return subprocess.run([BIN] + [str(x) for x in a], capture_output=True, text=True).stdout.strip()
def sh(cmd, inp=None):
    return subprocess.run(cmd, input=inp, capture_output=True)
def keyinfo(pem):
    out = sh(["openssl", "rsa", "-in", pem, "-noout", "-text"]).stdout.decode()
    mod = re.search(r"modulus:\n((?:\s+[0-9a-f:]+\n)+)", out).group(1)
    mod = re.sub(r"[\s:]", "", mod)
    mod = mod.lstrip("0") if len(mod) % 2 == 0 and mod.startswith("00") else mod
    if mod.startswith("00"): mod = mod[2:]
    e = int(re.search(r"publicExponent: (\d+)", out).group(1))
    return mod, e
fails = 0
tmp = tempfile.mkdtemp()
for bits, e in [(2048, 65537), (3072, 65537), (4096, 65537), (2048, 3), (2048, 17)]:
    pem = f"{tmp}/k{bits}_{e}.pem"
    sh(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", f"rsa_keygen_bits:{bits}", "-pkeyopt", f"rsa_keygen_pubexp:{e}", "-out", pem])
    mod, e = keyinfo(pem)
    for hname, hid in [("sha256", 1), ("sha384", 2), ("sha512", 3)]:
        for trial in range(3):
            data = os.urandom(random.randint(1, 500))
            sig = sh(["openssl", "dgst", f"-{hname}", "-sign", pem], data).stdout
            dig = getattr(hashlib, hname)(data).hexdigest()
            r = run("verify", mod, e, hid, dig, sig.hex())
            if r != "ok": print("FAIL valid sig", bits, e, hname, r); fails += 1
            # tampered signature, wrong digest, wrong hash id must all fail
            bad = bytearray(sig); bad[len(bad)//2] ^= 1
            if run("verify", mod, e, hid, dig, bytes(bad).hex()) != "bad": print("FAIL tampered accepted", bits, hname); fails += 1
            wd = getattr(hashlib, hname)(data + b"x").hexdigest()
            if run("verify", mod, e, hid, wd, sig.hex()) != "bad": print("FAIL wrong digest accepted", bits, hname); fails += 1
            other = hid % 3 + 1
            od = getattr({1: hashlib.sha256, 2: hashlib.sha384, 3: hashlib.sha512}[other], "__call__")(data).hexdigest()
            if run("verify", mod, e, other, od, sig.hex()) != "bad": print("FAIL wrong hash alg accepted", bits, hname); fails += 1
    # raw op against Python pow
    n = int(mod, 16)
    for _ in range(3):
        x = random.randrange(2, n)
        want = format(pow(x, e, n), "x").zfill(len(mod))
        got = run("op", mod, e, format(x, "x").zfill(len(mod)))
        if got != want: print("FAIL op", bits, e); fails += 1
    # input >= modulus must be rejected
    if run("op", mod, e, format(n, "x").zfill(len(mod))) != "err": print("FAIL out-of-range accepted"); fails += 1
    # encryption: decrypt with openssl
    msg = os.urandom(48)
    ct = run("enc", mod, e, msg.hex())
    if e == 65537:
        pt = sh(["openssl", "pkeyutl", "-decrypt", "-inkey", pem, "-pkeyopt", "rsa_padding_mode:pkcs1"], bytes.fromhex(ct)).stdout
        if pt != msg: print("FAIL encrypt/decrypt", bits); fails += 1
    print("bits", bits, "e", e, "done")
print("FAILURES:", fails)
sys.exit(1 if fails else 0)
