using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.Versioning;
using System.Xml;
using DocSharp.Binary.StructuredStorage.Reader;
using DocSharp.Binary.DocFileFormat;
using DocSharp.Binary.OpenXmlLib.WordprocessingML;
using DocSharp.Binary.OpenXmlLib;
using DocSharp.Binary.WordprocessingMLMapping;

[assembly: TargetFramework(".NETFramework,Version=v4.8")]

// Private pipe protocol: stdout contains DOCX bytes only; exit status commits it.
// No Office automation, macros, external applications, or network access.
internal static class DocHelper
{
    private const long OutputLimit = 128L * 1024 * 1024;

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length != 1) return 4;
        try
        {
            using (var input = new FileStream(args[0], FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var storage = new StructuredStorageReader(input))
            {
                using (var fib = new BinaryReader(storage.GetStream("WordDocument")))
                {
                    if (fib.ReadUInt16() != 0xa5ec) return 2;
                    ushort version = fib.ReadUInt16();
                    if (version < 0xc1 || version > 0x112) return 2;
                    fib.BaseStream.Position = 10;
                    if ((fib.ReadUInt16() & 0x8100) != 0) return 3;
                }
                var document = new WordDocument(storage);
                var temporaryPath = Path.GetTempFileName();
                using (var temporary = new FileStream(temporaryPath, FileMode.Open, FileAccess.ReadWrite, FileShare.Read, 65536, FileOptions.DeleteOnClose | FileOptions.SequentialScan))
                {
                    using (var output = new LimitedOutput(temporary, OutputLimit))
                    using (var package = WordprocessingDocument.Create(output, WordprocessingDocumentType.Document))
                        Converter.Convert(document, package);
                    NormalizePackage(temporary);
                    if (temporary.Length > OutputLimit) throw new OutputLimitException();
                    temporary.Position = 0;
                    using (var output = new LimitedOutput(Console.OpenStandardOutput(), OutputLimit))
                        temporary.CopyTo(output, 65536);
                }
            }
            return 0;
        }
        catch (OutputLimitException) { return 5; }
        catch { return 4; }
    }

    // Browsers do not decode EMF/WMF. DocSharp preserves those formats from
    // binary DOC, so normalize them inside the contained helper before the
    // existing DOCX renderer sees the package. Relationship ids and VML sizes
    // stay unchanged; only their private package targets and content types move.
    private static void NormalizePackage(Stream package)
    {
        package.Position = 0;
        using (var archive = new ZipArchive(package, ZipArchiveMode.Update, true))
        {
            var conversions = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in archive.Entries.Where(e => IsMetafile(e.FullName)).ToArray())
            {
                var sourceName = entry.FullName;
                var target = UniquePngName(archive, sourceName);
                var sourcePath = Path.GetTempFileName();
                var pngPath = Path.GetTempFileName();
                try
                {
                    using (var source = new FileStream(sourcePath, FileMode.Open, FileAccess.ReadWrite, FileShare.None, 65536, FileOptions.DeleteOnClose | FileOptions.SequentialScan))
                    {
                        using (var content = entry.Open()) CopyBounded(content, source);
                        source.Position = 0;
                        using (var image = Image.FromStream(source, true, true))
                        using (var png = new FileStream(pngPath, FileMode.Open, FileAccess.ReadWrite, FileShare.None, 65536, FileOptions.DeleteOnClose | FileOptions.SequentialScan))
                        {
                            image.Save(png, ImageFormat.Png);
                            if (png.Length > OutputLimit) throw new OutputLimitException();
                            png.Position = 0;
                            entry.Delete();
                            using (var content = archive.CreateEntry(target, CompressionLevel.Optimal).Open()) CopyBounded(png, content);
                        }
                    }
                }
                finally
                {
                    try { File.Delete(sourcePath); } catch { }
                    try { File.Delete(pngPath); } catch { }
                }
                conversions.Add(sourceName, target);
            }
            if (conversions.Count != 0)
            {
                foreach (var relationships in archive.Entries.Where(e => e.FullName.EndsWith(".rels", StringComparison.OrdinalIgnoreCase)).ToArray())
                    RewriteRelationships(archive, relationships, conversions);
                var contentTypes = archive.GetEntry("[Content_Types].xml");
                if (contentTypes != null) RewriteContentTypes(archive, contentTypes, conversions);
            }
            NormalizeConvertedMarkup(archive);
            NormalizeStyleDefaults(archive);
        }
    }

    // Normalize conversion artifacts that browsers cannot interpret like Word:
    // redundant horizontal direction markers, floating VML pictures without a
    // Word layout engine, and table borders omitted for newer binary records.
    private static void NormalizeConvertedMarkup(ZipArchive archive)
    {
        const string wordNamespace = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
        const string vmlNamespace = "urn:schemas-microsoft-com:vml";
        foreach (var entry in archive.Entries.Where(e => e.FullName.StartsWith("word/", StringComparison.OrdinalIgnoreCase) && e.FullName.EndsWith(".xml", StringComparison.OrdinalIgnoreCase)).ToArray())
        {
            var document = ReadXml(entry);var changed = false;
            foreach (XmlElement direction in document.GetElementsByTagName("textDirection", wordNamespace).Cast<XmlElement>().ToArray())
            {
                if (!direction.GetAttribute("val", wordNamespace).Equals("lrTb", StringComparison.OrdinalIgnoreCase)) continue;
                direction.ParentNode.RemoveChild(direction);changed = true;
            }
            foreach (XmlElement shape in document.GetElementsByTagName("shape", vmlNamespace))
            {
                if (shape.GetElementsByTagName("imagedata", vmlNamespace).Count == 0 || !shape.HasAttribute("style")) continue;
                var normalized = NormalizePictureStyle(shape.GetAttribute("style"));
                if (normalized == shape.GetAttribute("style")) continue;
                shape.SetAttribute("style", normalized);changed = true;
            }
            foreach (XmlElement table in document.GetElementsByTagName("tbl", wordNamespace).Cast<XmlElement>().ToArray())
            {
                if (table.GetElementsByTagName("tblBorders", wordNamespace).Count != 0 || table.GetElementsByTagName("tcBorders", wordNamespace).Count != 0) continue;
                var properties = DirectChild(table, "tblPr", wordNamespace);
                if (properties == null)
                {
                    properties = document.CreateElement("w", "tblPr", wordNamespace);
                    table.PrependChild(properties);
                }
                var borders = document.CreateElement("w", "tblBorders", wordNamespace);
                foreach (var side in new[] { "top", "left", "bottom", "right", "insideH", "insideV" })
                {
                    var border = document.CreateElement("w", side, wordNamespace);
                    border.SetAttribute("val", wordNamespace, "single");border.SetAttribute("sz", wordNamespace, "4");
                    border.SetAttribute("space", wordNamespace, "0");border.SetAttribute("color", wordNamespace, "auto");borders.AppendChild(border);
                }
                properties.AppendChild(borders);changed = true;
            }
            if (changed) ReplaceXml(archive, entry, document);
        }
    }

    private static string NormalizePictureStyle(string style)
    {
        var properties = style.Split(new[] { ';' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(value => value.Trim()).Where(value => value.Length != 0)
            .Where(value =>
            {
                var separator = value.IndexOf(':');
                var name = separator < 0 ? value : value.Substring(0, separator);
                return !name.Equals("position", StringComparison.OrdinalIgnoreCase) &&
                    !name.Equals("z-index", StringComparison.OrdinalIgnoreCase) &&
                    !name.StartsWith("mso-position-", StringComparison.OrdinalIgnoreCase);
            }).ToList();
        properties.Insert(0, "position:static");properties.Insert(1, "display:block");
        return string.Join(";", properties) + ";";
    }

    private static XmlElement DirectChild(XmlElement parent, string name, string xmlNamespace)
    {
        return parent.ChildNodes.OfType<XmlElement>().FirstOrDefault(child => child.LocalName == name && child.NamespaceURI == xmlNamespace);
    }

    // A style type can have only one default. DocSharp marks several generated
    // styles as defaults, causing docx-preview to turn their rules into global
    // selectors (for example, FollowedHyperlink underlining every span).
    private static void NormalizeStyleDefaults(ZipArchive archive)
    {
        const string wordNamespace = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
        var entry = archive.GetEntry("word/styles.xml");if (entry == null) return;
        var document = ReadXml(entry);var changed = false;
        foreach (XmlElement style in document.GetElementsByTagName("style", wordNamespace))
        {
            var defaultAttribute = style.GetAttributeNode("default", wordNamespace);if (defaultAttribute == null) continue;
            var type = style.GetAttribute("type", wordNamespace);var id = style.GetAttribute("styleId", wordNamespace);
            var keep = (type == "paragraph" && id == "Normal") || (type == "table" && id == "TableNormal") || (type == "character" && id == "DefaultParagraphFont");
            if (!keep) { style.RemoveAttributeNode(defaultAttribute);changed = true; }
        }
        if (changed) ReplaceXml(archive, entry, document);
    }

    private static bool IsMetafile(string name)
    {
        var extension = Path.GetExtension(name);
        return extension.Equals(".emf", StringComparison.OrdinalIgnoreCase) || extension.Equals(".wmf", StringComparison.OrdinalIgnoreCase);
    }

    private static string UniquePngName(ZipArchive archive, string source)
    {
        var stem = source.Substring(0, source.Length - Path.GetExtension(source).Length);
        var candidate = stem + ".png";
        for (var suffix = 1; archive.GetEntry(candidate) != null; ++suffix) candidate = stem + "-converted-" + suffix + ".png";
        return candidate;
    }

    private static void CopyBounded(Stream input, Stream output)
    {
        var buffer = new byte[65536];
        long total = 0;
        for (int count; (count = input.Read(buffer, 0, buffer.Length)) != 0;)
        {
            if (count > OutputLimit - total) throw new OutputLimitException();
            output.Write(buffer, 0, count); total += count;
        }
    }

    private static XmlDocument ReadXml(ZipArchiveEntry entry)
    {
        var document = new XmlDocument { PreserveWhitespace = true, XmlResolver = null };
        var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };
        using (var stream = entry.Open())
        using (var reader = XmlReader.Create(stream, settings)) document.Load(reader);
        return document;
    }

    private static void ReplaceXml(ZipArchive archive, ZipArchiveEntry entry, XmlDocument document)
    {
        var name = entry.FullName;entry.Delete();
        var settings = new XmlWriterSettings { Encoding = new System.Text.UTF8Encoding(false), Indent = false, CloseOutput = true };
        using (var stream = archive.CreateEntry(name, CompressionLevel.Optimal).Open())
        using (var writer = XmlWriter.Create(stream, settings)) document.Save(writer);
    }

    private static void RewriteRelationships(ZipArchive archive, ZipArchiveEntry entry, IDictionary<string, string> conversions)
    {
        var document = ReadXml(entry);var changed = false;
        foreach (XmlElement relationship in document.GetElementsByTagName("Relationship", "http://schemas.openxmlformats.org/package/2006/relationships"))
        {
            var target = relationship.GetAttribute("Target");
            foreach (var conversion in conversions)
            {
                var absolute = "/" + conversion.Key;
                if (!target.Equals(absolute, StringComparison.OrdinalIgnoreCase)) continue;
                relationship.SetAttribute("Target", "/" + conversion.Value);changed = true;break;
            }
        }
        if (changed) ReplaceXml(archive, entry, document);
    }

    private static void RewriteContentTypes(ZipArchive archive, ZipArchiveEntry entry, IDictionary<string, string> conversions)
    {
        var document = ReadXml(entry);var changed = false;
        foreach (XmlElement part in document.GetElementsByTagName("Override", "http://schemas.openxmlformats.org/package/2006/content-types"))
        {
            var name = part.GetAttribute("PartName");
            foreach (var conversion in conversions)
            {
                if (!name.Equals("/" + conversion.Key, StringComparison.OrdinalIgnoreCase)) continue;
                part.SetAttribute("PartName", "/" + conversion.Value);part.SetAttribute("ContentType", "image/png");changed = true;break;
            }
        }
        var imageTypes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
            { "gif", "image/gif" }, { "bmp", "image/bmp" }, { "tif", "image/tiff" }, { "tiff", "image/tiff" }
        };
        var defaults = document.GetElementsByTagName("Default", "http://schemas.openxmlformats.org/package/2006/content-types").Cast<XmlElement>().ToArray();
        foreach (var extension in archive.Entries.Where(e => e.FullName.StartsWith("word/media/", StringComparison.OrdinalIgnoreCase)).Select(e => Path.GetExtension(e.FullName).TrimStart('.')).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            string contentType;
            if (!imageTypes.TryGetValue(extension, out contentType) || defaults.Any(e => e.GetAttribute("Extension").Equals(extension, StringComparison.OrdinalIgnoreCase))) continue;
            var item = document.CreateElement("Default", "http://schemas.openxmlformats.org/package/2006/content-types");
            item.SetAttribute("Extension", extension);item.SetAttribute("ContentType", contentType);document.DocumentElement.AppendChild(item);changed = true;
        }
        if (changed) ReplaceXml(archive, entry, document);
    }
}
internal sealed class OutputLimitException : IOException { }
internal sealed class LimitedOutput : Stream
{
    private readonly Stream output;
    private long count;
    private readonly long limit;
    internal LimitedOutput(Stream output, long limit) { this.output = output; this.limit = limit; }
    public override bool CanRead { get { return false; } }
    public override bool CanSeek { get { return false; } }
    public override bool CanWrite { get { return true; } }
    public override long Length { get { return count; } }
    public override long Position { get { return count; } set { throw new NotSupportedException(); } }
    public override void Write(byte[] buffer, int offset, int length)
    {
        if (length > limit - count) throw new OutputLimitException();
        output.Write(buffer, offset, length); count += length;
    }
    public override void Flush() { output.Flush(); }
    public override int Read(byte[] buffer, int offset, int length) { throw new NotSupportedException(); }
    public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
    public override void SetLength(long length) { throw new NotSupportedException(); }
}
