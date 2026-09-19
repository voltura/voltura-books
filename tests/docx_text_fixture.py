"""Targeted DOCX extraction checks: no visible controls, browser, or Office."""
from pathlib import Path
import base64
import subprocess
import sys
import tempfile
import zipfile
from docx_fixture import create_docx_fixtures

with tempfile.TemporaryDirectory(prefix='books-docx-text-') as folder:
    folder = Path(folder)
    create_docx_fixtures(folder, base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aO1sAAAAASUVORK5CYII='))
    with zipfile.ZipFile(folder / 'large-xml.docx', 'w', zipfile.ZIP_DEFLATED) as archive:
        with archive.open('word/document.xml', 'w') as output:
            block = b'X' * (1024 * 1024)
            for _ in range(129):
                output.write(block)
    probe = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'build/Release/docx_text_tests.exe'
    subprocess.run([str(probe), str(folder)], check=True, timeout=30)
