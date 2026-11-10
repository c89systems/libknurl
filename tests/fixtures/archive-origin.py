"""Serve small archive inputs and a request log for the Bash acquisition test."""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import sys

fixture = Path(sys.argv[1])
data = (fixture / "archive.tar.gz").read_bytes()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        with (fixture / "requests").open("a") as log:
            log.write(self.path + "\n")
        if self.path not in ("/archive.tar.gz", "/truncated.tar.gz"):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        body = data if self.path == "/archive.tar.gz" else data[:len(data) // 2]
        self.wfile.write(body)
        self.close_connection = True

    def log_message(self, *args):
        pass


with HTTPServer(("127.0.0.1", 0), Handler) as server:
    (fixture / "port").write_text(str(server.server_port))
    server.serve_forever()
