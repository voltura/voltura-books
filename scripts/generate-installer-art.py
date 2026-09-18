"""Regenerate the 4x-resolution installer banner (requires Pillow and Segoe UI)."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
width, height = 656, 1256  # Four times NSIS's 164 x 314 wizard panel.
image = Image.new('RGB', (width, height))
draw = ImageDraw.Draw(image)
for y in range(height):
    t = y / (height - 1)
    draw.line((0, y, width, y), fill=tuple(round(a + (b-a)*t) for a, b in zip((10, 36, 49), (9, 82, 96))))

# Quiet page lines give the panel depth without competing with the wordmark.
for offset in range(0, 230, 28):
    draw.line((400+offset, 0, 656, 280-offset), fill='#143E4C', width=2)
fonts = Path('C:/Windows/Fonts')
def text(y, label, size, color, bold=False):
    font = ImageFont.truetype(str(fonts / ('segoeuib.ttf' if bold else 'segoeui.ttf')), size)
    draw.text((66, y), label, font=font, fill=color)

text(86, 'VOLTURA', 47, '#BDE4E6', True)
text(144, 'BOOKS', 84, 'white', True)
draw.rounded_rectangle((68, 266, 154, 273), radius=3, fill='#FFC857')

# Large version of the app's book symbol, with a gold bookmark and paper edges.
draw.rounded_rectangle((138, 429, 552, 937), radius=35, fill='#09313D')
draw.rounded_rectangle((112, 393, 530, 903), radius=34, fill='#11A5BA')
draw.rounded_rectangle((112, 393, 179, 903), radius=25, fill='#08758C')
draw.rounded_rectangle((161, 804, 517, 881), radius=15, fill='#F2F7F3')
draw.line((184, 827, 491, 827), fill='#B5D6D8', width=5)
draw.line((184, 852, 491, 852), fill='#B5D6D8', width=5)
draw.polygon(((386, 393), (447, 393), (447, 580), (417, 554), (386, 580)), fill='#FFC857')
draw.rounded_rectangle((224, 643, 443, 655), radius=5, fill='white')
draw.rounded_rectangle((224, 684, 383, 696), radius=5, fill='white')

text(1018, 'EPUB  →  KINDLE', 34, '#BDE4E6', True)
text(1084, 'Your next read.', 39, 'white')
text(1132, 'One click away.', 39, 'white')
image.save(root / 'src/installer-banner.bmp')
