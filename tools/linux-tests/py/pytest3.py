# pip on tOS: it starts, and it installs a package from PyPI over HTTPS
import subprocess, sys, os, ssl, tempfile, urllib.request
fails = 0
def check(cond, what):
    global fails
    if cond: print("pytest3: ok", what)
    else: print("pytest3: FAIL", what); fails += 1
py = sys.executable
r = subprocess.run([py, "-m", "pip", "--version"], capture_output=True, text=True, timeout=300)
check(r.returncode == 0 and r.stdout.startswith("pip "), "pip --version: " + r.stdout.strip()[:60])
check(os.path.isfile(ssl.get_default_verify_paths().cafile or os.environ.get("SSL_CERT_FILE", "")), "ssl finds the CA certificates")
try:
    body = urllib.request.urlopen("https://pypi.org/simple/six/", timeout=60).read()
    check(b"six" in body, "urllib fetches https://pypi.org/simple/six/ (certificates verified)")
except Exception as e:
    check(False, "https to pypi.org: %r" % (e,))
d = tempfile.mkdtemp()
r = subprocess.run([py, "-m", "pip", "install", "--no-cache-dir", "--disable-pip-version-check", "--target", d, "six"], capture_output=True, text=True, timeout=900)
print(r.stdout[-300:], r.stderr[-300:])
check(r.returncode == 0 and os.path.exists(d + "/six.py"), "pip install six")
r = subprocess.run([py, "-c", "import sys; sys.path.insert(0, %r); import six; print(six.__version__, six.PY3)" % d], capture_output=True, text=True)
check(r.returncode == 0 and "True" in r.stdout, "the installed package imports: " + r.stdout.strip())
print("pytest3: all passed" if not fails else "pytest3: %d FAILED" % fails)
sys.exit(1 if fails else 0)
