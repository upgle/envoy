#!/usr/bin/env python3
"""
Very slow backend server for testing single-flight timeout.
Delays responses for 3 seconds, longer than the 1 second timeout.
"""

from http.server import BaseHTTPRequestHandler, HTTPServer
import time
import threading

request_count = 0
request_lock = threading.Lock()

class VerySlowHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global request_count

        with request_lock:
            request_count += 1
            current_request = request_count

        # Log when request arrives
        print(f'Backend: Request #{current_request} arrived at {time.time():.3f}', flush=True)

        # Simulate very slow processing (3 seconds - longer than 1s timeout)
        time.sleep(3)

        # Send response
        response_body = f'Response from backend (request #{current_request})'
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(response_body)))
        self.end_headers()
        self.wfile.write(response_body.encode())

        print(f'Backend: Request #{current_request} completed at {time.time():.3f}', flush=True)

    def log_message(self, fmt, *args):
        # Suppress default HTTP logs
        pass

if __name__ == '__main__':
    server_address = ('127.0.0.1', 8080)
    httpd = HTTPServer(server_address, VerySlowHandler)
    print(f'Very slow backend server running on {server_address[0]}:{server_address[1]}')
    print('Each request will take 3 seconds to process')
    print('With 1 second single-flight timeout:')
    print('  - First request goes upstream and waits 3 seconds')
    print('  - Subsequent requests timeout after 1 second and go upstream independently')
    print('  - Result: Multiple backend requests (timeout behavior)')
    print('=' * 70)
    httpd.serve_forever()
