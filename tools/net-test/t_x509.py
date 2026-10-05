#!/usr/bin/env python3
"""Scenario tests for kernel/net/x509.c using certificate chains generated with openssl."""
import subprocess, os, sys, tempfile, textwrap, random
BIN = "/tmp/t_x509"
os.environ["ASAN_OPTIONS"] = "detect_leaks=0"
T = tempfile.mkdtemp()
def sh(*a, inp=None):
    r = subprocess.run(list(a), input=inp, capture_output=True)
    if r.returncode != 0 and a[0] == "openssl":
        sys.stderr.write(r.stderr.decode()); raise SystemExit("openssl failed: " + " ".join(a))
    return r
def key(name, bits=2048):
    p = f"{T}/{name}.key"
    sh("openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", f"rsa_keygen_bits:{bits}", "-out", p)
    return p
def cert(name, subj, keyf, issuer=None, ext="", days=365, md="sha256", start=None, serial=None):
    """issuer = (certfile, keyfile) or None for self-signed"""
    cfg = f"{T}/{name}.cnf"
    open(cfg, "w").write("[req]\ndistinguished_name=dn\nprompt=no\n[dn]\nCN=%s\n[v3]\n%s\n" % (subj, ext))
    csr = f"{T}/{name}.csr"
    sh("openssl", "req", "-new", "-key", keyf, "-config", cfg, "-out", csr)
    out = f"{T}/{name}.pem"
    args = ["openssl", "x509", "-req", "-in", csr, "-days", str(days), f"-{md}", "-extfile", cfg, "-extensions", "v3", "-out", out]
    if issuer: args += ["-CA", issuer[0], "-CAkey", issuer[1], "-set_serial", str(serial or random.randint(2, 10**9))]
    else: args += ["-signkey", keyf]
    if start: args += ["-not_before", start] if False else []
    sh(*args)
    der = f"{T}/{name}.der"
    sh("openssl", "x509", "-in", out, "-outform", "DER", "-out", der)
    return out, der
def run(host, now, flags, root, *chain):
    r = subprocess.run([BIN, "chain", host, now, str(flags), root or "-"] + list(chain), capture_output=True, text=True)
    return r.stdout.strip().split(" ")[0] if r.stdout.strip() else "CRASH:" + r.stderr[:200]
E_OK, E_MAL, E_KEY, E_SIGALG, E_WEAK, E_BADSIG, E_EXP, E_NYV, E_HOST, E_ISS, E_NOTCA, E_PATH, E_CRIT, E_KU, E_CLOCK = "0", "-1", "-2", "-3", "-4", "-5", "-6", "-7", "-8", "-9", "-10", "-11", "-12", "-13", "-14"
import datetime
NOW = (datetime.datetime.utcnow() + datetime.timedelta(hours=1)).strftime("%Y%m%d%H%M%S")
fails = 0
def expect(label, got, want):
    global fails
    ok = got == want
    if not ok: fails += 1
    print(("PASS " if ok else "FAIL ") + label + ("" if ok else f"  got {got} want {want}"))

CA_EXT = "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash"
rk = key("root", 4096); ik = key("inter"); lk = key("leaf")
root_pem, root = cert("root", "Test Root", rk, None, CA_EXT, days=3650, md="sha384")
inter_pem, inter = cert("inter", "Test Intermediate", ik, (root_pem, rk), "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign", days=3000, md="sha512")
LEAF_EXT = "basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:example.test,DNS:*.wild.test,IP:10.0.2.2"
leaf_pem, leaf = cert("leaf", "example.test", lk, (inter_pem, ik), LEAF_EXT, days=300)

expect("valid chain (sha512 inter under sha384 4096-bit root)", run("example.test", NOW, 1, root, leaf, inter), E_OK)
expect("case-insensitive host", run("EXAMPLE.test", NOW, 0, root, leaf, inter), E_OK)
expect("trailing dot in host", run("example.test.", NOW, 0, root, leaf, inter), E_OK)
expect("wrong host", run("other.test", NOW, 0, root, leaf, inter), E_HOST)
expect("wildcard one label", run("a.wild.test", NOW, 0, root, leaf, inter), E_OK)
expect("wildcard does not match apex", run("wild.test", NOW, 0, root, leaf, inter), E_HOST)
expect("wildcard does not match two labels", run("a.b.wild.test", NOW, 0, root, leaf, inter), E_HOST)
expect("IP SAN match", run("10.0.2.2", NOW, 0, root, leaf, inter), E_OK)
expect("IP SAN mismatch", run("10.0.2.3", NOW, 0, root, leaf, inter), E_HOST)
expect("suffix attack (example.test.evil.com)", run("example.test.evil.com", NOW, 0, root, leaf, inter), E_HOST)
expect("expired (now = 2040)", run("example.test", "20400101000000", 0, root, leaf, inter), E_EXP)
expect("not yet valid (now = 2020)", run("example.test", "20200101000000", 0, root, leaf, inter), E_NYV)
expect("missing intermediate", run("example.test", NOW, 0, root, leaf), E_ISS)
expect("untrusted root (empty trust store)", run("example.test", NOW, 0, None, leaf, inter), E_ISS)
expect("root also sent in chain", run("example.test", NOW, 0, root, leaf, inter, root), E_OK)
expect("leaf requires keyEncipherment (present)", run("example.test", NOW, 1, root, leaf, inter), E_OK)

# --- leaf variants
ds_pem, ds = cert("leaf_ds", "example.test", lk, (inter_pem, ik), "basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nsubjectAltName=DNS:example.test")
expect("RSA kx flag with digitalSignature-only leaf", run("example.test", NOW, 1, root, ds, inter), E_KU)
expect("same leaf without the flag", run("example.test", NOW, 0, root, ds, inter), E_OK)
cl_pem, cl = cert("leaf_client", "example.test", lk, (inter_pem, ik), "basicConstraints=CA:FALSE\nextendedKeyUsage=clientAuth\nsubjectAltName=DNS:example.test")
expect("EKU clientAuth only", run("example.test", NOW, 0, root, cl, inter), E_KU)
cr_pem, cr = cert("leaf_crit", "example.test", lk, (inter_pem, ik), "basicConstraints=CA:FALSE\nsubjectAltName=DNS:example.test\n1.2.3.4.5=critical,ASN1:UTF8String:boom")
expect("unknown critical extension", run("example.test", NOW, 0, root, cr, inter), E_CRIT)
cn_pem, cn = cert("leaf_cn", "cn-only.test", lk, (inter_pem, ik), "basicConstraints=CA:FALSE")
expect("CN fallback when no SAN", run("cn-only.test", NOW, 0, root, cn, inter), E_OK)
expect("CN fallback does not match other host", run("example.test", NOW, 0, root, cn, inter), E_HOST)
sn_pem, sn = cert("leaf_san_cn", "cn-wins.test", lk, (inter_pem, ik), "basicConstraints=CA:FALSE\nsubjectAltName=DNS:san.test")
expect("SAN present: CN is ignored", run("cn-wins.test", NOW, 0, root, sn, inter), E_HOST)
sha1_pem, sha1 = cert("leaf_sha1", "example.test", lk, (inter_pem, ik), LEAF_EXT, md="sha1")
expect("SHA-1 signed leaf", run("example.test", NOW, 0, root, sha1, inter), E_WEAK)
s384_pem, s384 = cert("leaf_s384", "example.test", lk, (inter_pem, ik), LEAF_EXT, md="sha384")
expect("SHA-384 signed leaf", run("example.test", NOW, 0, root, s384, inter), E_OK)

# --- CA constraint violations
nk = key("notca")
nc_pem, nc = cert("notca", "Not A CA", nk, (root_pem, rk), "basicConstraints=critical,CA:FALSE")
nl_pem, nl = cert("leaf_under_notca", "example.test", lk, (nc_pem, nk), LEAF_EXT)
expect("intermediate with CA:FALSE", run("example.test", NOW, 0, root, nl, nc), E_NOTCA)
nb_pem, nb = cert("nobc", "No BC", nk, (root_pem, rk), "keyUsage=keyCertSign")
nbl_pem, nbl = cert("leaf_under_nobc", "example.test", lk, (nb_pem, nk), LEAF_EXT)
expect("intermediate without basicConstraints", run("example.test", NOW, 0, root, nbl, nb), E_NOTCA)
nku_pem, nku = cert("nokcs", "No KeyCertSign", nk, (root_pem, rk), "basicConstraints=critical,CA:TRUE\nkeyUsage=digitalSignature")
nkl_pem, nkl = cert("leaf_under_nokcs", "example.test", lk, (nku_pem, nk), LEAF_EXT)
expect("CA without keyCertSign", run("example.test", NOW, 0, root, nkl, nku), E_NOTCA)
# pathlen: root -> inter1(pathlen 0) -> inter2 -> leaf
k2 = key("inter2")
i2_pem, i2 = cert("inter2", "Inter Two", k2, (inter_pem, ik), "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign")
l2_pem, l2 = cert("leaf_deep", "example.test", lk, (i2_pem, k2), LEAF_EXT)
expect("pathLen=0 intermediate has another CA below it", run("example.test", NOW, 0, root, l2, i2, inter), E_PATH)
# pathlen ok: inter with pathlen 1 above inter2
ik1 = key("inter_p1")
ip1_pem, ip1 = cert("inter_p1", "Inter P1", ik1, (root_pem, rk), "basicConstraints=critical,CA:TRUE,pathlen:1\nkeyUsage=critical,keyCertSign")
k3 = key("inter3")
i3_pem, i3 = cert("inter3", "Inter Three", k3, (ip1_pem, ik1), "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign")
l3_pem, l3 = cert("leaf_deep2", "example.test", lk, (i3_pem, k3), LEAF_EXT)
expect("pathLen=1 with exactly one CA below", run("example.test", NOW, 0, root, l3, i3, ip1), E_OK)

# --- tampering
raw = bytearray(open(leaf, "rb").read())
for label, pos in [("signature byte", len(raw) - 5), ("subject/tbs byte", 200)]:
    t = bytearray(raw); t[pos] ^= 0x01
    open(f"{T}/tampered.der", "wb").write(t)
    got = run("example.test", NOW, 0, root, f"{T}/tampered.der", inter)
    expect("tampered " + label + " is rejected", "rejected" if got != E_OK else got, "rejected")
# wrong issuer key: leaf signed by an unrelated CA with the same subject name as the intermediate
fk = key("fake_inter")
fi_pem, fi = cert("fake_inter", "Test Intermediate", fk, None, "basicConstraints=critical,CA:TRUE")
fl_pem, fl = cert("leaf_fake", "example.test", lk, (fi_pem, fk), LEAF_EXT)
expect("forged intermediate with the right name but wrong key", run("example.test", NOW, 0, root, fl, inter), E_BADSIG)
# 1024-bit intermediate: refused
wk = key("weak1024", 1024)
w_pem, w = cert("weak1024", "Weak Inter", wk, (root_pem, rk), CA_EXT)
wl_pem, wl = cert("leaf_weak", "example.test", lk, (w_pem, wk), LEAF_EXT)
expect("1024-bit RSA intermediate", run("example.test", NOW, 0, root, wl, w), E_KEY)
# self-signed leaf trusted directly / not trusted
ss_pem, ss = cert("selfsigned", "example.test", lk, None, "basicConstraints=CA:FALSE\nsubjectAltName=DNS:example.test")
expect("self-signed leaf, trusted explicitly", run("example.test", NOW, 0, ss, ss), E_OK)
expect("self-signed leaf, not trusted", run("example.test", NOW, 0, root, ss), E_ISS)
expect("no certificates at all", subprocess.run([BIN, "chain", "example.test", NOW, "0", root], capture_output=True, text=True).stdout.split(" ")[0] in ("", "-1") and "ok" or "bad", "ok")

# --- mutation fuzzing under ASan/UBSan: must never crash
r = subprocess.run([BIN, "fuzz", "1", "3000", leaf, inter, root], capture_output=True, text=True)
expect("fuzz: 3000 mutated chains, no crash", "fuzz done" in r.stdout and "ERROR" not in r.stderr, True)
r = subprocess.run([BIN, "fuzz", "2", "3000", leaf, inter], capture_output=True, text=True)
expect("fuzz seed 2", "fuzz done" in r.stdout and "ERROR" not in r.stderr, True)
print("FAILURES:", fails)
sys.exit(1 if fails else 0)
