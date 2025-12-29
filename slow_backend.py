#!/usr/bin/env python3
"""
Slow backend server for testing single-flight pattern.
Intentionally delays responses to make concurrent request behavior more visible.
"""

from http.server import BaseHTTPRequestHandler, HTTPServer
import time
import threading

request_count = 0
request_lock = threading.Lock()

class SlowHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global request_count

        with request_lock:
            request_count += 1
            current_request = request_count

        # Log when request arrives
        print(f'Backend: Request #{current_request} arrived at {time.time():.3f}', flush=True)

        # Simulate slow processing (2 seconds)
        time.sleep(2)

        # Send response
        response_body = f'Response from backend (request #{current_request})'
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(response_body)))
        self.end_headers()
        self.wfile.write(response_body.encode())

        print(f'Backend: Request #{current_request} completed at {time.time():.3f}', flush=True)

    def log_message(self, fmt, *args):
        # Suppress default HTTP logs, we have custom ones
        pass

if __name__ == '__main__':
    server_address = ('127.0.0.1', 8080)
    httpd = HTTPServer(server_address, SlowHandler)
    print(f'Slow backend server running on {server_address[0]}:{server_address[1]}')
    print('Each request will take 2 seconds to process')
    print('This helps demonstrate single-flight pattern:')
    print('  - With single-flight: Only 1 backend request for concurrent client requests')
    print('  - Without single-flight: N backend requests for N concurrent client requests')
    print('=' * 70)
    httpd.serve_forever()
