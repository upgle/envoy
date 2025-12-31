#!/usr/bin/env python3
from http.server import BaseHTTPRequestHandler, HTTPServer

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', '49')
        self.end_headers()
        self.wfile.write(b'This is the original response from backend server')
    def log_message(self, fmt, *args):
        print(f'Backend: {fmt % args}', flush=True)

HTTPServer(('127.0.0.1', 8081), Handler).serve_forever()
