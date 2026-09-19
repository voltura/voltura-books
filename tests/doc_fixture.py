"""Pinned MIT DocSharp sample, plus invalid/encrypted/older-version derivatives.

The original fixture is fetched and verified by CMake; never shipped in dist.
"""
from pathlib import Path
import hashlib
import io
import struct
import subprocess
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET


def word_offset(data):
    sector_size = 1 << struct.unpack_from('<H', data, 30)[0]
    directory = struct.unpack_from('<I', data, 48)[0]
    # This pinned sample keeps its WordDocument entry in the first directory sector.
    start = (directory + 1) * sector_size
    for offset in range(start, start + sector_size, 128):
        length = struct.unpack_from('<H', data, offset + 64)[0]
        if data[offset:offset + length - 2].decode('utf-16-le') == 'WordDocument':
            return (struct.unpack_from('<I', data, offset + 116)[0] + 1) * sector_size
    raise AssertionError('WordDocument entry not found')


def create_doc_fixtures(folder, source):
    data = source.read_bytes()
    assert hashlib.sha256(data).hexdigest() == '53ab718412fe7df64e72f7f8245396f944815c2c467c32a3248ea6596f0541ea'
    (folder / 'read.doc').write_bytes(data)
    offset = word_offset(data)
    for name, field, value in [('older.doc', 2, 0x65), ('encrypted.doc', 10, 0x100), ('obfuscated.doc', 10, 0x8000)]:
        modified = bytearray(data)
        struct.pack_into('<H', modified, offset + field, value)
        (folder / name).write_bytes(modified)
    (folder / 'bad.doc').write_bytes(b'not a compound document')
    (folder / 'truncated.doc').write_bytes(data[:4096])


def main():
    helper, source = map(Path, sys.argv[1:])
    before = source.read_bytes()
    with tempfile.TemporaryDirectory(prefix='books-doc-test-') as temporary:
        folder = Path(temporary)
        create_doc_fixtures(folder, source)
        output = subprocess.run([str(helper), str(folder / 'read.doc')], capture_output=True, timeout=30, check=True)
        archive = zipfile.ZipFile(io.BytesIO(output.stdout))
        media = [name.lower() for name in archive.namelist() if '/media/' in name]
        assert not any(name.endswith(('.emf', '.wmf')) for name in media), 'Browser-incompatible metafile remains'
        assert sum(name.endswith('.png') for name in media) >= 3, 'EMF/WMF images were not normalized to PNG'
        content_types = ET.fromstring(archive.read('[Content_Types].xml'))
        content_ns = {'ct': 'http://schemas.openxmlformats.org/package/2006/content-types'}
        defaults = {node.get('Extension').lower(): node.get('ContentType') for node in content_types.findall('ct:Default', content_ns)}
        for extension in {Path(name).suffix.lstrip('.') for name in media}:
            if extension in {'png', 'jpg', 'jpeg', 'gif', 'bmp', 'tif', 'tiff'}:
                assert defaults.get(extension, '').startswith('image/'), f'Missing browser image type for {extension}'
        document = ET.fromstring(archive.read('word/document.xml'))
        ns = {'w': 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'}
        tables = document.findall('.//w:tbl', ns)
        assert tables, 'Tables lost'
        assert all(table.find('./w:tblPr/w:tblBorders', ns) is not None or table.findall('.//w:tcBorders', ns) for table in tables), 'Converted table has no visible borders'
        assert not [node for node in document.findall('.//w:textDirection', ns) if node.get(f'{{{ns["w"]}}}val') == 'lrTb'], 'Horizontal table text would render vertically'
        vml = {'v': 'urn:schemas-microsoft-com:vml'}
        picture_shapes = [shape for shape in document.findall('.//v:shape', vml) if shape.find('.//v:imagedata', vml) is not None]
        assert picture_shapes and all('position:absolute' not in shape.get('style', '').replace(' ', '').lower() for shape in picture_shapes), 'Converted picture remains outside document flow'
        assert document.findall('.//w:rPr/w:b', ns), 'Bold formatting lost'
        assert document.findall('.//w:sectPr', ns), 'Section layout lost'
        assert len(document.findall('.//w:p', ns)) > 30, 'Long document content lost'
        assert any('/media/' in name for name in archive.namelist()), 'Embedded pictures lost'
        text = ''.join(document.itertext())
        assert any(ord(char) > 127 for char in text), 'Unicode content lost'
        styles = ET.fromstring(archive.read('word/styles.xml'))
        defaults = {}
        for style in styles.findall('w:style', ns):
            if style.get(f'{{{ns["w"]}}}default') in {'1', 'true', 'on'}:
                style_type = style.get(f'{{{ns["w"]}}}type')
                defaults[style_type] = defaults.get(style_type, 0) + 1
        assert all(count == 1 for count in defaults.values()), f'Duplicate default styles leak formatting globally: {defaults}'
        for name, code in [('older.doc', 2), ('encrypted.doc', 3), ('obfuscated.doc', 3), ('bad.doc', 4), ('truncated.doc', 4)]:
            result = subprocess.run([str(helper), str(folder / name)], capture_output=True, timeout=30)
            assert result.returncode == code, (name, result.returncode)
            assert not result.stdout, 'Rejected document emitted partial output'
        assert (folder / 'read.doc').read_bytes() == before, 'Original changed'
    assert source.read_bytes() == before
    print('PASS: DOC formatting, tables, images, Unicode, long content, rejection and original integrity')


if __name__ == '__main__':
    main()
