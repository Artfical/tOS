# Python on tOS: the parts of the standard library that need the kernel and the C libraries
import sys, os, json, hashlib, zlib, bz2, lzma, sqlite3, ssl, ctypes, threading, socket, subprocess
import re, struct, time, datetime, decimal, collections, tempfile, shutil, base64, binascii, random
import select, signal, mmap, errno, pathlib, io, heapq, math, fractions, statistics, fcntl, termios, pty
fails = 0
def check(cond, what):
    global fails
    if cond: print("pytest: ok", what)
    else: print("pytest: FAIL", what); fails += 1

check(sys.version_info[:2] == (3, 14) and sys.platform == "linux", "Python %d.%d on %s" % (sys.version_info[0], sys.version_info[1], sys.platform))
check(json.loads(json.dumps({"a": [1, 2, {"b": None}], "c": "\u00e9"})) == {"a": [1, 2, {"b": None}], "c": "\u00e9"}, "json round trip")
check(hashlib.sha256(b"abc").hexdigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "hashlib sha256")
check(hashlib.md5(b"abc").hexdigest() == "900150983cd24fb0d6963f7d28e17f72", "hashlib md5")
data = os.urandom(200000) + b"tOS" * 50000
check(zlib.decompress(zlib.compress(data, 9)) == data, "zlib round trip")
check(bz2.decompress(bz2.compress(data)) == data, "bz2 round trip")
check(lzma.decompress(lzma.compress(data)) == data, "lzma round trip")
db = sqlite3.connect(":memory:")
db.execute("create table t(a integer primary key, b text)")
db.executemany("insert into t(b) values(?)", [("row%d" % i,) for i in range(1000)])
check(db.execute("select count(*), max(b) from t where a % 2 = 0").fetchone() == (500, "row999"), "sqlite3 in memory")
path = tempfile.mktemp(suffix=".db")
d2 = sqlite3.connect(path); d2.execute("create table x(v)"); d2.execute("insert into x values (42)"); d2.commit(); d2.close()
check(sqlite3.connect(path).execute("select v from x").fetchone() == (42,), "sqlite3 on a file")
os.unlink(path)
ctx = ssl.create_default_context()
check("OpenSSL" in ssl.OPENSSL_VERSION and ctx.verify_mode == ssl.CERT_REQUIRED, "ssl context (%s)" % ssl.OPENSSL_VERSION)
libc = ctypes.CDLL(None)
libc.strlen.restype = ctypes.c_size_t
check(libc.strlen(b"hello tOS") == 9, "ctypes calls libc")
class Pt(ctypes.Structure): _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_double)]
p = Pt(3, 4.5)
check(p.x == 3 and p.y == 4.5 and ctypes.sizeof(Pt) == 16, "ctypes structures")
CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_int)
def cmp(a, b): return a - b
arr = (ctypes.c_int * 5)(5, 3, 1, 4, 2)
libc.qsort(arr, 5, 4, ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(lambda a, b: ctypes.cast(a, ctypes.POINTER(ctypes.c_int))[0] - ctypes.cast(b, ctypes.POINTER(ctypes.c_int))[0]))
check(list(arr) == [1, 2, 3, 4, 5], "ctypes callback (qsort)")
# threads
total = [0]; lock = threading.Lock()
def work(n):
    for _ in range(2000):
        with lock: total[0] += 1
ts = [threading.Thread(target=work, args=(i,)) for i in range(4)]
[t.start() for t in ts]; [t.join() for t in ts]
check(total[0] == 8000, "threads and a lock")
# sockets on loopback
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("127.0.0.1", 0)); srv.listen(1)
port = srv.getsockname()[1]
def echo():
    c, _ = srv.accept()
    while True:
        b = c.recv(4096)
        if not b: break
        c.sendall(b.upper())
    c.close()
th = threading.Thread(target=echo); th.start()
cl = socket.create_connection(("127.0.0.1", port)); cl.sendall(b"hello loopback"); got = cl.recv(100)
cl.shutdown(socket.SHUT_WR); th.join(); cl.close(); srv.close()
check(got == b"HELLO LOOPBACK", "a TCP echo on 127.0.0.1")
# unix socket
sp = tempfile.mktemp(suffix=".sock")
us = socket.socket(socket.AF_UNIX); us.bind(sp); us.listen(1)
uc = socket.socket(socket.AF_UNIX); uc.connect(sp); conn, _ = us.accept(); uc.sendall(b"unix!"); 
check(conn.recv(10) == b"unix!", "an AF_UNIX socket")
os.unlink(sp)
a, b = socket.socketpair(); a.sendall(b"pair"); check(b.recv(10) == b"pair", "socketpair")
# processes
r = subprocess.run(["/system/pkg/busybox/bin/busybox", "echo", "from", "child"], capture_output=True, text=True)
check(r.returncode == 0 and r.stdout.strip() == "from child", "subprocess.run captures output")
r = subprocess.run(["/system/pkg/busybox/bin/busybox", "sh", "-c", "exit 7"])
check(r.returncode == 7, "a child's exit status")
pid = os.fork()
if pid == 0: os._exit(5)
_, st = os.waitpid(pid, 0)
check(os.WIFEXITED(st) and os.WEXITSTATUS(st) == 5, "os.fork and waitpid")
rd, wr = os.pipe()
pid = os.fork()
if pid == 0:
    os.close(rd); os.write(wr, b"through a pipe"); os._exit(0)
os.close(wr); check(os.read(rd, 100) == b"through a pipe", "a pipe from a child"); os.waitpid(pid, 0)
# files
d = tempfile.mkdtemp()
fp = os.path.join(d, "f.txt")
with open(fp, "w") as f: f.write("line1\nline2\n")
with open(fp, "a") as f: f.write("line3\n")
check(open(fp).read().splitlines() == ["line1", "line2", "line3"], "write, append, read")
os.rename(fp, fp + ".bak"); check(os.path.exists(fp + ".bak") and not os.path.exists(fp), "rename")
shutil.copy(fp + ".bak", fp); check(os.path.getsize(fp) == 18, "shutil.copy and getsize")
check(sorted(os.listdir(d)) == ["f.txt", "f.txt.bak"], "listdir")
mm_f = open(fp, "r+b"); mm = mmap.mmap(mm_f.fileno(), 0); check(mm[:5] == b"line1", "mmap of a file"); mm.close(); mm_f.close()
shutil.rmtree(d); check(not os.path.exists(d), "rmtree")
# time, signals, polling
t0 = time.time(); time.sleep(0.2); check(0.15 < time.time() - t0 < 2, "time.sleep")
got = []
signal.signal(signal.SIGALRM, lambda s, f: got.append(s)); signal.setitimer(signal.ITIMER_REAL, 0.2); time.sleep(1)
check(got == [signal.SIGALRM], "a timer signal reaches a Python handler")
r, w = os.pipe(); check(select.select([r], [], [], 0.1) == ([], [], []), "select times out")
os.write(w, b"x"); check(select.select([r], [], [], 1)[0] == [r], "select sees data")
ep = select.epoll(); ep.register(r, select.EPOLLIN); check(ep.poll(1)[0][0] == r, "epoll")
# the rest of the library
check(re.sub(r"(\d+)", lambda m: str(int(m[1]) * 2), "a1 b22 c333") == "a2 b44 c666", "re")
check(struct.unpack("<IHb", struct.pack("<IHb", 1, 2, -3)) == (1, 2, -3), "struct")
check(decimal.Decimal("0.1") + decimal.Decimal("0.2") == decimal.Decimal("0.3"), "decimal")
check(fractions.Fraction(1, 3) + fractions.Fraction(1, 6) == fractions.Fraction(1, 2), "fractions")
check(statistics.mean([1, 2, 3, 4]) == 2.5 and math.isclose(math.sqrt(2) ** 2, 2), "math and statistics")
check(base64.b64decode(base64.b64encode(b"\x00\xff tOS")) == b"\x00\xff tOS", "base64")
check(datetime.datetime(2026, 11, 2, 12, 0) - datetime.datetime(2026, 10, 31) == datetime.timedelta(days=2, hours=12), "datetime")
check(collections.Counter("mississippi").most_common(1) == [("i", 4)] or collections.Counter("mississippi").most_common(2)[0][1] == 4, "collections")
random.seed(5); check(len({random.random() for _ in range(100)}) == 100, "random")
check(pathlib.Path("/system/pkg/python3/bin").is_dir(), "pathlib")
mfd, sfd = pty.openpty(); os.write(mfd, b"hi\n"); check(os.read(sfd, 10) == b"hi\n", "a pty pair (pty.openpty)")
print("pytest: all passed" if not fails else "pytest: %d FAILED" % fails)
sys.exit(1 if fails else 0)
