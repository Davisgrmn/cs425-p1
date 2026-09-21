"""Exercise the real CLI against a local TCP server; never send external mail."""

from pathlib import Path
import socket
import subprocess
import sys
import threading
import unittest


CLIENT = str(Path(sys.argv.pop(1) if len(sys.argv) > 1 else
                  "build/release/myapp").resolve())
BASE = ["-f", "sender@example.com", "-t", "recipient@example.com"]


class ClientTests(unittest.TestCase):
    def run_client(self, args, body=b""):
        return subprocess.run([CLIENT, *args], input=body, capture_output=True, timeout=10)

    def session(self, options=(), body=b"", wire_body=b"", subject="", helo="localhost",
                bad_reply=None):
        commands = [
            f"HELO {helo}\r\n".encode(),
            b"MAIL FROM:<sender@example.com>\r\n",
            b"RCPT TO:<recipient@example.com>\r\n",
            b"DATA\r\n",
            (b"From: sender@example.com\r\nTo: recipient@example.com\r\n" +
             f"Subject: {subject}\r\n\r\n".encode() + wire_body + b".\r\n"),
            b"QUIT\r\n",
        ]
        replies = [b"220-first\r\n220 ready\r\n", b"250-first\r\n250 hello\r\n",
                   b"250 sender\r\n", b"250 recipient\r\n", b"354 data\r\n",
                   b"250 queued\r\n", b"221 bye\r\n"]
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(10)
            port = listener.getsockname()[1]

            def serve():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(5)
                        for index, reply in enumerate(replies):
                            if index:
                                expected = commands[index - 1]
                                actual = b""
                                while len(actual) < len(expected):
                                    chunk = connection.recv(len(expected) - len(actual))
                                    self.assertTrue(chunk, "Client disconnected before its command")
                                    actual += chunk
                                self.assertEqual(actual, expected)
                            if index == bad_reply:
                                connection.sendall(b"550 rejected by test server\r\n")
                                self.assertEqual(connection.recv(4096), b"",
                                                 "Client continued after rejection")
                                return
                            # Small sends also exercise the real socket callbacks.
                            for start in range(0, len(reply), 2):
                                connection.sendall(reply[start:start + 2])
                        self.assertEqual(connection.recv(4096), b"")
                except Exception as error:
                    errors.append(error)

            worker = threading.Thread(target=serve, daemon=True)
            worker.start()
            try:
                result = self.run_client([*BASE, *options, "-p", str(port), "localhost"], body)
            finally:
                worker.join(timeout=11)
            self.assertFalse(worker.is_alive(), "Local server did not finish")
            if errors:
                raise errors[0]
        expected_status = 0 if bad_reply is None else 2
        self.assertEqual(result.returncode, expected_status, result.stderr.decode())
        if bad_reply is None:
            self.assertEqual(result.stderr, b"")
        else:
            self.assertIn(b"550 rejected by test server", result.stderr)

    def test_usage_and_invalid_arguments(self):
        result = self.run_client([])
        self.assertEqual(result.returncode, 0)
        self.assertIn(b"Usage: myapp", result.stdout)
        for args in (["-x"], ["-f"], ["localhost"], [*BASE],
                     [*BASE, "one", "two"], [*BASE, "-f", "", "localhost"]):
            with self.subTest(args=args):
                result = self.run_client(args)
                self.assertEqual(result.returncode, 1)
                self.assertIn(b"Usage: myapp", result.stderr)
        for option in ("-f", "-t", "-s", "-H"):
            for newline in ("\r", "\n", "\r\n"):
                with self.subTest(option=option, newline=newline):
                    result = self.run_client([*BASE, option, "x" + newline + "QUIT", "localhost"])
                    self.assertEqual(result.returncode, 1)

    def test_explicit_options(self):
        self.session(["-s", "greeting", "-H", "client.example", "-b", ".hello\nworld"],
                     body=b"ignored stdin", wire_body=b"..hello\r\nworld\r\n",
                     subject="greeting", helo="client.example")

    def test_stdin_and_empty_bodies(self):
        self.session(body=b".\r\n..two\nlast\r", wire_body=b"..\r\n...two\r\nlast\r\n")
        self.session()
        self.session(["-b", ""], body=b"ignored stdin")
        self.session(body=b"body\n" * 3000, wire_body=b"body\r\n" * 3000)

    def test_every_rejected_reply(self):
        for stage in range(7):
            with self.subTest(stage=stage):
                self.session(bad_reply=stage)

    def test_connection_and_stdin_failures(self):
        result = self.run_client([*BASE, "-p", "invalid/service", "127.0.0.1"])
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"Cannot resolve", result.stderr)
        result = self.run_client([*BASE, "127.0.0.1"], body=b"before\0after")
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"NUL", result.stderr)
        with socket.socket() as bound:
            bound.bind(("127.0.0.1", 0))
            result = self.run_client([*BASE, "-p", str(bound.getsockname()[1]), "127.0.0.1"])
            self.assertEqual(result.returncode, 2)
            self.assertIn(b"Cannot connect", result.stderr)


if __name__ == "__main__":
    unittest.main()
