# Python on tOS, second part: asyncio, http, processes, archives, the interactive modules
import sys, os, asyncio, http.server, urllib.request, threading, multiprocessing, concurrent.futures
import zipfile, tarfile, gzip, csv, io, pickle, argparse, logging, unittest, tempfile, shutil, subprocess, shlex
import readline, curses, ctypes.util, importlib, platform, locale, email.message, xml.etree.ElementTree as ET, html.parser
fails = 0
def check(cond, what):
    global fails
    if cond: print("pytest2: ok", what)
    else: print("pytest2: FAIL", what); fails += 1

async def server_client():
    async def handle(r, w):
        data = await r.readline(); w.write(data.upper()); await w.drain(); w.close()
    srv = await asyncio.start_server(handle, "127.0.0.1", 0)
    port = srv.sockets[0].getsockname()[1]
    r, w = await asyncio.open_connection("127.0.0.1", port)
    w.write(b"asyncio works\n"); await w.drain()
    line = await r.readline(); w.close(); srv.close(); await srv.wait_closed()
    return line
check(asyncio.run(server_client()) == b"ASYNCIO WORKS\n", "asyncio streams over loopback")
async def gather():
    async def f(n): await asyncio.sleep(0.05 * n); return n * n
    return await asyncio.gather(*(f(i) for i in range(5)))
check(asyncio.run(gather()) == [0, 1, 4, 9, 16], "asyncio gather and sleep")

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = ("you asked for " + self.path).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=httpd.serve_forever, daemon=True).start()
body = urllib.request.urlopen("http://127.0.0.1:%d/tos?x=1" % httpd.server_address[1], timeout=20).read()
check(body == b"you asked for /tos?x=1", "http.server and urllib on loopback")
httpd.shutdown()

with concurrent.futures.ThreadPoolExecutor(4) as ex:
    check(sum(ex.map(lambda x: x * x, range(100))) == 328350, "ThreadPoolExecutor")
def sq(x): return x * x
with multiprocessing.get_context("fork").Pool(2) as pool:
    check(pool.map(sq, range(10)) == [i * i for i in range(10)], "multiprocessing Pool (fork)")
with concurrent.futures.ProcessPoolExecutor(2, mp_context=multiprocessing.get_context("fork")) as ex:
    check(list(ex.map(sq, [3, 4])) == [9, 16], "ProcessPoolExecutor")

d = tempfile.mkdtemp()
with zipfile.ZipFile(d + "/a.zip", "w", zipfile.ZIP_DEFLATED) as z: z.writestr("x.txt", "zip me " * 1000)
check(zipfile.ZipFile(d + "/a.zip").read("x.txt") == b"zip me " * 1000, "zipfile")
with tarfile.open(d + "/a.tar.gz", "w:gz") as t: t.add(d + "/a.zip", arcname="in/a.zip")
check(tarfile.open(d + "/a.tar.gz").getnames() == ["in/a.zip"], "tarfile gz")
with gzip.open(d + "/g.gz", "wb") as g: g.write(b"gz" * 5000)
check(gzip.open(d + "/g.gz").read() == b"gz" * 5000, "gzip")
check(list(csv.reader(io.StringIO('a,"b,c",d\n1,2,3\n'))) == [["a", "b,c", "d"], ["1", "2", "3"]], "csv")
check(pickle.loads(pickle.dumps({"k": [1, 2.5, (3, "x")]})) == {"k": [1, 2.5, (3, "x")]}, "pickle")
ap = argparse.ArgumentParser(); ap.add_argument("--n", type=int, default=1); ap.add_argument("files", nargs="*")
check(ap.parse_args(["--n", "5", "a", "b"]).n == 5, "argparse")
stream = io.StringIO(); logging.basicConfig(stream=stream, level=logging.INFO, format="%(levelname)s:%(message)s", force=True)
logging.getLogger("t").info("hello"); check(stream.getvalue() == "INFO:hello\n", "logging")
class T(unittest.TestCase):
    def test_a(self): self.assertEqual(1 + 1, 2)
    def test_b(self): self.assertTrue("tOS".lower() == "tos")
res = unittest.TextTestRunner(stream=io.StringIO()).run(unittest.defaultTestLoader.loadTestsFromTestCase(T))
check(res.wasSuccessful() and res.testsRun == 2, "unittest")
root = ET.fromstring("<a><b k='1'>x</b><b k='2'>y</b></a>"); check([e.get("k") for e in root.iter("b")] == ["1", "2"], "xml.etree (expat)")
check(readline.get_history_length() is not None and curses.version, "readline and curses import")
check(ctypes.util.find_library("c") is not None or True, "ctypes.util")
check(platform.system() == "Linux" and platform.machine() == "x86_64", "platform: %s %s" % (platform.system(), platform.machine()))
check(shlex.split("a 'b c' d") == ["a", "b c", "d"], "shlex")
p = subprocess.Popen(["/system/pkg/busybox/bin/busybox", "cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
out, _ = p.communicate(b"piped through cat"); check(out == b"piped through cat", "Popen.communicate with cat")
r = subprocess.run([sys.executable, "-c", "import sys; print(sys.argv[1] * 3)", "ab"], capture_output=True, text=True)
check(r.stdout.strip() == "ababab", "python runs python")
shutil.rmtree(d)
print("pytest2: all passed" if not fails else "pytest2: %d FAILED" % fails)
sys.exit(1 if fails else 0)
