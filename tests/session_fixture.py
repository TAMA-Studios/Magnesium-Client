"""Loopback HTTP fixture; no credentials or response bodies printed."""
import http.server, json, subprocess, sys, threading
from urllib.parse import urlparse, parse_qs
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def reply(self, status, data):
        self.send_response(status); self.send_header('Content-Type','application/json'); self.end_headers()
        self.wfile.write(json.dumps(data).encode())
    def do_GET(self):
        if self.path.startswith('/pages'):
            query = parse_qs(urlparse(self.path).query)
            after = int(query.get('after', ['0'])[0]); before = int(query.get('before', ['0'])[0])
            ids = [i for i in range(1, 1301) if i > after and (before == 0 or i < before)]
            ids = ids[:50] if 'after' in query else ids[-50:]
            self.reply(200, {'messages':[{'id':i,'created':0,'sender':'alice','text':'history'} for i in ids]})
        elif self.path.startswith('/broken-page'): self.reply(200, {'unexpected':True})
        elif self.path.startswith('/recent'):
            cursor = 901 if 'after=900' in self.path else 900 if 'limit=50' in self.path and 'after=' not in self.path else 1
            self.reply(200, {'messages':[{'id':cursor,'created':0,'sender':'alice','text':'recent history'}]})
        elif self.path.startswith('/offline'): self.reply(503, {'error':'offline'})
        elif self.path.startswith('/invalid'): self.reply(401, {'error':'invalid token'})
        elif self.path.startswith('/malformed'): self.reply(200, {'unexpected':True})
        elif self.path.startswith('/mismatch'): self.reply(200, {'username':'someone else'})
        else: self.reply(200, {'username':'alice'})
    def do_POST(self):
        data=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path.startswith('/legacy'): self.reply(409, {'error':'token_reset_required: legacy'})
        elif data.get('password') != 'secret': self.reply(401, {'error':'invalid credentials'})
        else: self.reply(200, {'ok':True,'username':'alice','token':'test-token'})
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True); thread.start()
try: result=subprocess.run([sys.argv[1],f'http://127.0.0.1:{server.server_port}'],timeout=30); sys.exit(result.returncode)
finally: server.shutdown(); server.server_close()
