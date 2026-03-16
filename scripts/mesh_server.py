#!/usr/bin/env python3
"""Minimal HTTP server with CORS for serving URDF mesh files to Foxglove Studio."""
import os
from http.server import HTTPServer, SimpleHTTPRequestHandler

os.chdir("/data")

class CORSHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

HTTPServer(("0.0.0.0", 8081), CORSHandler).serve_forever()
