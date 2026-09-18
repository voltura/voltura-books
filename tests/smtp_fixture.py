"""Loopback-only SMTP integration tests; no mail leaves this computer."""
import base64
import email
import email.policy
import hashlib
import pathlib
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROBE = ROOT / "build/Release/smtp_probe.exe"
OPENSSL = shutil.which("openssl") or r"C:\Program Files\Git\usr\bin\openssl.exe"


class Server:
    def __init__(self, cert, key, mode, starttls=False, direct=False):
        self.mode, self.starttls, self.direct = mode, starttls, direct
        self.message = None
        self.error = None
        self.authenticated = False
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(cert, key)
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.sock.listen()
        self.sock.settimeout(15)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        conn = None
        stream = None
        try:
            conn, _ = self.sock.accept()
            conn.settimeout(15)
            if not self.starttls and not self.direct:
                conn = self.context.wrap_socket(conn, server_side=True)
            stream = conn.makefile("rwb", buffering=0)

            def reply(line):
                stream.write(line + b"\r\n")

            reply(b"220 localhost fixture")
            secure = not self.starttls and not self.direct
            while line := stream.readline():
                command = line.strip().split(b" ", 1)[0].upper()
                if command == b"EHLO":
                    reply(b"250-localhost")
                    if not secure and (not self.direct or self.starttls):
                        reply(b"250-STARTTLS")
                    reply(b"250 AUTH PLAIN LOGIN")
                elif command == b"STARTTLS":
                    reply(b"220 Begin TLS")
                    stream.close()
                    conn = self.context.wrap_socket(conn, server_side=True)
                    stream = conn.makefile("rwb", buffering=0)
                    secure = True
                elif command == b"AUTH":
                    assert not self.direct, "Direct mode must not authenticate"
                    assert secure, "Credentials sent without TLS"
                    parts = line.strip().split(b" ")
                    if parts[1].upper() == b"PLAIN":
                        if len(parts) < 3:
                            reply(b"334 ")
                            token = stream.readline().strip()
                        else:
                            token = parts[2]
                        credentials = base64.b64decode(token).split(b"\0")
                        assert credentials[-2:] == [b"sender@example.com", b"fixture-password"]
                    else:
                        reply(b"334 VXNlcm5hbWU6")
                        assert base64.b64decode(stream.readline().strip()) == b"sender@example.com"
                        reply(b"334 UGFzc3dvcmQ6")
                        assert base64.b64decode(stream.readline().strip()) == b"fixture-password"
                    self.authenticated = self.mode != "auth_failure"
                    reply(b"235 Authenticated" if self.authenticated else b"535 Invalid credentials")
                elif command in (b"MAIL", b"RCPT"):
                    assert self.direct or (secure and self.authenticated)
                    reply(b"250 OK")
                elif command == b"DATA":
                    reply(b"354 Send message")
                    chunks = []
                    while True:
                        line = stream.readline()
                        if line in (b".\r\n", b""):
                            break
                        chunks.append(line[1:] if line.startswith(b"..") else line)
                    self.message = b"".join(chunks)
                    if self.mode == "uncertain":
                        break
                    reply(b"552 Message too large" if self.mode == "rejected" else b"250 Accepted")
                elif command == b"QUIT":
                    reply(b"221 Bye")
                    break
                else:
                    reply(b"250 OK")
        except (ssl.SSLError, ConnectionError) as exc:
            if self.mode != "untrusted":
                self.error = exc
        except Exception as exc:
            self.error = exc
        finally:
            if stream:
                stream.close()
            if conn:
                conn.close()
            self.sock.close()


def main():
    with tempfile.TemporaryDirectory(prefix="voltura-books-smtp-") as temp:
        temp = pathlib.Path(temp)
        cert, key = temp / "cert.pem", temp / "key.pem"
        subprocess.run([OPENSSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, capture_output=True)
        deep = temp
        for i in range(6):
            deep /= f"long folder {i} " + "x" * 35
        deep.mkdir(parents=True)
        book = deep / "Böcker 日本語 📖 & spaces.epub"
        with zipfile.ZipFile(book, "w") as archive:
            archive.writestr("mimetype", "application/epub+zip")
            archive.writestr("test.bin", bytes(range(256)) * 4096)
        expected = hashlib.sha256(book.read_bytes()).digest()
        for mode, starttls, direct in [("success", False, False), ("success", True, False), ("auth_failure", False, False),
                               ("rejected", False, False), ("uncertain", False, False), ("untrusted", False, False),
                               ("success", False, True), ("success", True, True), ("rejected", False, True),
                               ("uncertain", False, True), ("untrusted", True, True)]:
            server = Server(cert, key, mode, starttls, direct)
            run = subprocess.run([str(PROBE), str(server.port), str(book),
                                  "" if mode == "untrusted" else str(cert),
                                  "direct" if direct else "starttls" if starttls else "tls"], capture_output=True, timeout=25)
            server.thread.join(20)
            assert not server.thread.is_alive(), "Fixture did not finish"
            assert server.error is None, repr(server.error)
            output = run.stdout.decode("utf-8")
            assert (run.returncode == 0) == (mode == "success"), (mode, output, run.stderr)
            if mode == "success":
                message = email.message_from_bytes(server.message, policy=email.policy.default)
                attachment, = list(message.iter_attachments())
                assert hashlib.sha256(attachment.get_payload(decode=True)).digest() == expected
                assert str(message["Subject"]) == book.name, str(message["Subject"])
                assert attachment.get_filename() == book.name, attachment.get_filename()
                assert message["From"] == "sender@example.com"
                assert message["To"] == "reader@kindle.com"
            if mode == "auth_failure":
                assert "login" in output and server.message is None
            if mode == "uncertain":
                assert "may already have been submitted" in output
            if mode == "untrusted":
                assert "secure connection" in output and not server.authenticated
            if mode == "rejected":
                assert "552" in output
            print(f"PASS {'direct' if direct else 'provider'} {mode} {'STARTTLS' if starttls else 'plain SMTP' if direct else 'TLS'}")
        formats = {"pdf":"application/pdf", "rtf":"application/rtf", "txt":"text/plain", "html":"text/html", "htm":"text/html", "doc":"application/msword", "docx":"application/vnd.openxmlformats-officedocument.wordprocessingml.document", "jpg":"image/jpeg", "jpeg":"image/jpeg", "png":"image/png", "gif":"image/gif", "bmp":"image/bmp"}
        for extension, mime in formats.items():
            document = temp / ("Attachment." + extension)
            payload = bytes(range(256)) * 32
            document.write_bytes(payload)
            server = Server(cert, key, "success", False, False)
            run = subprocess.run([str(PROBE), str(server.port), str(document), str(cert), "tls"], capture_output=True, timeout=25)
            server.thread.join(20)
            assert run.returncode == 0 and server.error is None
            message = email.message_from_bytes(server.message, policy=email.policy.default)
            attachment, = list(message.iter_attachments())
            assert attachment.get_content_type() == mime
            assert attachment.get_payload(decode=True) == payload
            assert attachment.get_filename() == document.name
        print("PASS all additional formats: MIME types and attachment integrity")
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
            port = unused.getsockname()[1]
        run = subprocess.run([str(PROBE), str(port), str(book), str(cert), "tls"], capture_output=True, timeout=25)
        assert run.returncode == 1 and b"Could not reach" in run.stdout
        print("PASS unavailable server")


if __name__ == "__main__":
    main()
