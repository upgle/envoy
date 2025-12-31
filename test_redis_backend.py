#!/usr/bin/env python3
"""
Simple HTTP backend server for testing Envoy global_cache with Redis.
Returns different responses to test caching behavior.
"""

from http.server import HTTPServer, BaseHTTPRequestHandler
import json
import time
from datetime import datetime

class TestBackendHandler(BaseHTTPRequestHandler):
    request_count = 0

    def do_GET(self):
        TestBackendHandler.request_count += 1

        # Prepare response
        response_data = {
            "message": "Hello from backend!",
            "timestamp": datetime.now().isoformat(),
            "request_number": self.request_count,
            "path": self.path
        }

        response_body = json.dumps(response_data, indent=2).encode('utf-8')

        # Send response
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(response_body)))
        # Add cache control header (5 minutes TTL)
        self.send_header('Cache-Control', 'max-age=300')
        self.end_headers()
        self.wfile.write(response_body)

        # Log to console
        print(f"[{datetime.now()}] Request #{self.request_count}: {self.path}", flush=True)

    def log_message(self, format, *args):
        # Suppress default logging
        pass

if __name__ == '__main__':
    server_address = ('', 8080)
    httpd = HTTPServer(server_address, TestBackendHandler)
    print(f"Backend server listening on http://localhost:8080", flush=True)
    print(f"Ready to receive requests...", flush=True)
    httpd.serve_forever()
