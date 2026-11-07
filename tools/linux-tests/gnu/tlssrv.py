import ssl, http.server
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain('/programs/gt/c.pem', '/programs/gt/k.pem')
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        b = b'hello tls from tOS'
        self.send_response(200); self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self, *a): pass
srv = http.server.HTTPServer(('127.0.0.1', 8443), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print('tlssrv: listening', flush=True)
srv.serve_forever()
