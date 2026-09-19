"""Local EPUB cover fixtures; never read credentials or send email."""
from pathlib import Path
import zipfile
import io
import subprocess
import tempfile
from PIL import Image

root = Path(__file__).resolve().parents[1]
probe = root / 'build/Release/cover_probe.exe'
buffer = io.BytesIO()
Image.new('RGB', (400, 600), 'teal').save(buffer, format='PNG')
container = '<container><rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>'
cases = [
    ('epub3', '<package><manifest><item href="images/cover%20art.png" properties="nav cover-image" media-type="image/png"/></manifest></package>', True),
    ('epub2', '<package><metadata><meta name="cover" content="coverid"/></metadata><manifest><item id="coverid" href="images/cover%20art.png" media-type="image/png"/></manifest></package>', True),
    ('guide', '<package><guide><reference type="cover" href="cover.xhtml"/></guide></package>', True),
    ('none', '<package><manifest/></package>', False),
    ('badxml', '<package>', False),
    ('badimage', '<package><manifest><item href="broken.png" properties="cover-image"/></manifest></package>', False),
]
with tempfile.TemporaryDirectory(prefix='books-cover-') as folder:
    for name, opf, expected in cases:
        path = Path(folder) / (name + ' å & cover.epub')
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('META-INF/container.xml', container)
            archive.writestr('OPS/book.opf', opf)
            archive.writestr('OPS/images/cover art.png', buffer.getvalue())
            archive.writestr('OPS/cover.xhtml', '<html><body><img src="images/cover%20art.png"/></body></html>')
            archive.writestr('OPS/broken.png', b'not an image')
        result = subprocess.run([str(probe), str(path)], capture_output=True, text=True)
        assert (result.returncode == 0) == expected, (name, result.returncode, result.stdout, result.stderr)
        if expected:
            assert result.stdout.strip() == '146x220', result.stdout
        print(name + ': passed')

with tempfile.TemporaryDirectory(prefix='books-document-preview-') as folder:
    for extension in ['png', 'jpg', 'gif', 'bmp', 'pdf']:
        path = Path(folder) / ('Preview.' + extension)
        Image.new('RGB', (400, 600), 'teal').save(path)
        result = subprocess.run([str(probe), str(path)], capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, (extension, result.stdout, result.stderr)
        print(extension + ' preview: passed')
    path = Path(folder) / 'Preview.docx'
    with zipfile.ZipFile(path, 'w') as archive:
        archive.writestr('docProps/thumbnail.png', buffer.getvalue())
    assert subprocess.run([str(probe), str(path)], capture_output=True, timeout=30).returncode == 0
    for name, color, image_format, expected in [
        ('white', 'white', 'JPEG', 1),
        ('near-white', (252, 252, 252), 'JPEG', 1),
        ('transparent', (0, 0, 0, 0), 'PNG', 1),
        ('content', 'teal', 'PNG', 0),
    ]:
        thumbnail = io.BytesIO()
        Image.new('RGBA' if name == 'transparent' else 'RGB', (395, 512), color).save(thumbnail, format=image_format)
        path = Path(folder) / (name + '.docx')
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('docProps/thumbnail.' + ('jpeg' if image_format == 'JPEG' else 'png'), thumbnail.getvalue())
        result = subprocess.run([str(probe), str(path)], capture_output=True, timeout=30)
        assert result.returncode == expected, (name, result.stdout, result.stderr)
    print('Blank DOCX thumbnails use file-type fallback; visible thumbnails retained')
    for extension in ['rtf', 'txt', 'html', 'doc', 'pdf', 'docx']:
        path = Path(folder) / ('No-preview.' + extension)
        path.write_bytes(b'not a preview')
        assert subprocess.run([str(probe), str(path)], capture_output=True, timeout=30).returncode == 1, extension
    print('Document thumbnail and fallback checks passed')

details_probe = root / 'build/Release/details_probe.exe'
with tempfile.TemporaryDirectory(prefix='books-details-') as folder:
    path = Path(folder) / 'Metadata.epub'
    with zipfile.ZipFile(path, 'w') as archive:
        archive.writestr('META-INF/container.xml', container)
        archive.writestr('OPS/book.opf', '<package><metadata><title>A &amp; B</title><creator>First Author</creator><creator>Second Author</creator><publisher>Example Press</publisher></metadata></package>')
    def details(path):
        run = subprocess.run([str(details_probe), str(path)], capture_output=True, text=True, timeout=30)
        assert run.returncode == 0
        return run.stdout.splitlines()
    assert details(path) == ['A & B', 'First Author, Second Author', 'Example Press', '0', '0']
    path = Path(folder) / 'Metadata.docx'
    with zipfile.ZipFile(path, 'w') as archive:
        archive.writestr('docProps/core.xml', '<properties><title>Example document</title><creator>Writer</creator></properties>')
        archive.writestr('docProps/app.xml', '<properties><Pages>12</Pages></properties>')
    assert details(path) == ['Example document', 'Writer', '', '12', '1']
    path = Path(folder) / 'Metadata.pdf'
    Image.new('RGB', (200, 300), 'white').save(path)
    assert details(path)[3:] == ['1', '0']
    path = Path(folder) / 'No-metadata.rtf'
    path.write_text('fixture')
    assert details(path)[3:] == ['0', '0']
    print('EPUB, DOCX, PDF metadata and absent-metadata checks passed')
