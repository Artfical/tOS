import socket
def chk(name, ok):
    print(('PASS ' if ok else 'FAIL ') + name)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('10.0.2.2', 80))
req = 'GET /small.txt HTTP/1.0\r\nHost: x\r\n\r\n'
n = s.send(req)
chk('send returns the number of bytes sent', n == len(req))
data = b''
while True:
    d = s.recv(512)
    if not d: break
    data += d
s.close()
chk('recv returns bytes', isinstance(data, bytes))
chk('response has the 16-byte body', data.endswith(b'hello-from-host\n') and data.startswith(b'HTTP/'))
def raises(f):
    try:
        f(); return False
    except Exception:
        return True
chk('port 99999 is refused', raises(lambda: socket.socket().connect(('10.0.2.2', 99999))))
chk('port 0 is refused', raises(lambda: socket.socket().connect(('10.0.2.2', 0))))
chk('999.1.1.1 is not an address (DNS lookup fails, no connect to garbage)', raises(lambda: socket.getaddrinfo('999.1.1.1', 80)))
chk('UDP connect() is refused instead of using the wrong port', raises(lambda: socket.socket(socket.AF_INET, socket.SOCK_DGRAM).connect(('10.0.2.2', 53))))
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.bind(('0.0.0.0', 5005))
u.close()
u2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ok = True
for i in range(8):
    x = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        x.bind(('0.0.0.0', 6000 + i))
    except Exception:
        ok = False
    x.close()
chk('closing a bound UDP socket frees its slot (8 bind/close cycles)', ok)
print('T2 DONE')
