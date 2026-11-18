# Generates every launcher/web/desktop icon from one source PNG (round badge on transparency).
#   python android/make_icons.py [source.png]
# Default source: android/icon-source.png
import os, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'icon-source.png')
RES = os.path.join(HERE, 'common', 'res')
DENS = {'mdpi': 1, 'hdpi': 1.5, 'xhdpi': 2, 'xxhdpi': 3, 'xxxhdpi': 4}

src = Image.open(SRC).convert('RGBA')
# The badge's ring: centered on the round part (the side buttons stick out to the right).
a = src.split()[3]
l, t, r, b = a.getbbox()
d = b - t                      # vertical extent = ring diameter
cx, cy = l + d / 2, t + d / 2
ring = src.crop((int(cx - d / 2), int(cy - d / 2), int(cx + d / 2), int(cy + d / 2)))


def circle(img, inset=0.0):
    """Cut img to a circle (inset = fraction trimmed from each side)."""
    s = img.size[0] * 4
    m = Image.new('L', (s, s), 0)
    p = s * inset
    ImageDraw.Draw(m).ellipse((p, p, s - p - 1, s - p - 1), fill=255)
    m = m.resize(img.size, Image.LANCZOS)
    out = img.copy()
    out.putalpha(Image.composite(img.split()[3], Image.new('L', img.size, 0), m))
    return out


def fit(img, size):
    return img.resize((size, size), Image.LANCZOS)


# Ring cut just inside its dark outer outline, so the launcher's circle mask lands on the orange rim.
ring_clean = circle(ring, 0.012)

for dn, k in DENS.items():
    d_ = os.path.join(RES, 'mipmap-' + dn)
    os.makedirs(d_, exist_ok=True)
    legacy = round(48 * k)
    fit(ring_clean, legacy).save(os.path.join(d_, 'ic_launcher.png'))
    fit(ring_clean, legacy).save(os.path.join(d_, 'ic_launcher_round.png'))
    # Adaptive foreground: 108dp canvas, visible mask ~72dp. Ring slightly larger (76dp) so the
    # mask never shows a gap; the background is the rim's orange anyway.
    fg_px = round(108 * k)
    fg = Image.new('RGBA', (fg_px, fg_px), (0, 0, 0, 0))
    rs = round(76 * k)
    o = (fg_px - rs) // 2
    fg.alpha_composite(fit(ring_clean, rs), (o, o))
    fg.save(os.path.join(d_, 'ic_launcher_foreground.png'))

# Web: favicon + touch/PWA icons (full badge, with the side buttons).
web = os.path.join(ROOT, 'web')
sq = max(src.size)
full = Image.new('RGBA', (sq, sq), (0, 0, 0, 0))
full.alpha_composite(src, ((sq - src.size[0]) // 2, (sq - src.size[1]) // 2))
fit(full, 512).save(os.path.join(web, 'icon-512.png'))
fit(full, 192).save(os.path.join(web, 'icon-192.png'))
# iOS ignores transparency (shows black): use the ring on the rim colour, square.
apple = Image.new('RGBA', (180, 180), (254, 182, 0, 255))
apple.alpha_composite(fit(ring_clean, 180))
apple.convert('RGB').save(os.path.join(web, 'apple-touch-icon.png'))
fit(full, 64).save(os.path.join(web, 'favicon.ico'), sizes=[(16, 16), (32, 32), (48, 48), (64, 64)])

# Desktop: window icon (loaded at runtime by SDL) + Windows .ico for the .exe.
res = os.path.join(ROOT, 'res')
os.makedirs(res, exist_ok=True)
fit(full, 256).save(os.path.join(res, 'icon.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
fit(full, 128).save(os.path.join(res, 'icon-128.png'))
print('ring', (cx, cy, d), 'ok')
