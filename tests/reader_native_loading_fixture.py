"""Hidden native reader checks; never opts into visible acceptance tests."""
from pathlib import Path
import subprocess
import sys
import tempfile
from PIL import Image

with tempfile.TemporaryDirectory(prefix='books-native-loading-') as folder:
    folder = Path(folder)
    (folder / 'read.txt').write_text('A readable line.\n' * 5000, encoding='utf-8')
    (folder / 'read.rtf').write_text(r'{\rtf1\ansi\fs24 ' + r'Paragraph\par ' * 500 + '}', encoding='ascii')
    images = [Image.new('RGB', (100, 160), color) for color in ['red', 'green', 'blue']]
    images[0].save(folder / 'read.pdf', save_all=True, append_images=images[1:])
    (folder / 'read.doc').write_bytes(b'cancelled before parsing')
    subprocess.run([sys.argv[1], str(folder)], check=True, timeout=50)
