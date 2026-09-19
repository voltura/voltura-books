"""Small, reproducible OOXML packages; no Office installation required."""
import zipfile


def create_docx_fixtures(folder, picture):
    ns = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'
    rel = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
    def paragraph(text):
        return f'<w:p><w:r><w:t>{text}</w:t></w:r></w:p>'
    heading = '<w:p><w:r><w:rPr><w:b/><w:color w:val="008080"/><w:sz w:val="36"/></w:rPr><w:t>Formatted DOCX: café 日本語</w:t></w:r></w:p>'
    table = '<w:tbl><w:tblPr><w:tblW w:w="5000" w:type="pct"/></w:tblPr><w:tblGrid><w:gridCol w:w="2400"/><w:gridCol w:w="2400"/></w:tblGrid><w:tr><w:tc>' + paragraph('First cell') + '</w:tc><w:tc>' + paragraph('Second cell') + '</w:tc></w:tr></w:tbl>'
    image = '''<w:p><w:r><w:drawing><wp:inline xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"><wp:extent cx="914400" cy="914400"/><a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"><pic:blipFill><a:blip r:embed="image"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="914400" cy="914400"/></a:xfrm><a:prstGeom prst="rect"/></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>'''
    section = '<w:sectPr><w:headerReference w:type="default" r:id="header"/><w:footerReference w:type="default" r:id="footer"/><w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="720" w:bottom="720" w:left="720" w:right="720"/></w:sectPr>'
    body = heading + table + image + '<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>List item</w:t></w:r></w:p>'
    body += '<w:p><w:r><w:t>Note reference</w:t><w:footnoteReference w:id="1"/></w:r></w:p>'
    body += '<w:p><w:hyperlink r:id="external"><w:r><w:t>Blocked link</w:t></w:r></w:hyperlink></w:p><w:altChunk r:id="chunk"/>'
    body += ''.join(paragraph(f'Paragraph {i}: readable document content.') for i in range(100))
    body += '<w:p><w:r><w:br w:type="page"/></w:r></w:p>' + paragraph('Final DOCX paragraph')
    def package(name, content):
        with zipfile.ZipFile(folder / name, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="xml" ContentType="application/xml"/><Default Extension="png" ContentType="image/png"/><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/></Types>')
            archive.writestr('_rels/.rels', f'<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="main" Type="{rel}/officeDocument" Target="word/document.xml"/></Relationships>')
            archive.writestr('word/document.xml', f'<w:document xmlns:w="{ns}" xmlns:r="{rel}"><w:body>{content}{section}</w:body></w:document>')
            relationships = [('image', 'image', 'media/picture.png'), ('header', 'header', 'header1.xml'), ('footer', 'footer', 'footer1.xml'), ('notes', 'footnotes', 'footnotes.xml'), ('numbering', 'numbering', 'numbering.xml'), ('chunk', 'aFChunk', 'chunk.html')]
            archive.writestr('word/_rels/document.xml.rels', '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' + ''.join(f'<Relationship Id="{key}" Type="{rel}/{kind}" Target="{target}"/>' for key, kind, target in relationships) + f'<Relationship Id="external" Type="{rel}/hyperlink" Target="https://example.com/blocked" TargetMode="External"/></Relationships>')
            archive.writestr('word/media/picture.png', picture)
            archive.writestr('word/header1.xml', f'<w:hdr xmlns:w="{ns}">{paragraph("Document header")}</w:hdr>')
            archive.writestr('word/footer1.xml', f'<w:ftr xmlns:w="{ns}">{paragraph("Document footer")}</w:ftr>')
            archive.writestr('word/footnotes.xml', f'<w:footnotes xmlns:w="{ns}"><w:footnote w:id="1">{paragraph("Footnote content")}</w:footnote></w:footnotes>')
            archive.writestr('word/numbering.xml', f'<w:numbering xmlns:w="{ns}"><w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="bullet"/><w:lvlText w:val="•"/></w:lvl></w:abstractNum><w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num></w:numbering>')
            archive.writestr('word/chunk.html', '<script>window.hacked=true</script><p>UNSAFE CHUNK</p>')
    package('read.docx', body)
    package('single.docx', paragraph('Single paragraph'))
    (folder / 'bad.docx').write_bytes(b'not a ZIP')
    # Encrypted Office documents are OLE containers, not ordinary ZIP packages.
    (folder / 'encrypted.docx').write_bytes(bytes.fromhex('D0CF11E0A1B11AE1') + b'EncryptedPackage')
    with zipfile.ZipFile(folder / 'xml-bad.docx', 'w') as archive:
        archive.writestr('word/document.xml', '<broken>')
    with zipfile.ZipFile(folder / 'dtd.docx', 'w') as archive:
        archive.writestr('word/document.xml', '<!DOCTYPE x [<!ENTITY x "bad">]><x>&x;</x>')
    with zipfile.ZipFile(folder / 'traversal.docx', 'w') as archive:
        archive.writestr('../word/document.xml', 'bad')
