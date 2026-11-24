# The installer's icon: a disc split black and white (the two gods) in a gold ring.
import sys
from PIL import Image, ImageDraw
out = sys.argv[1]
S = 256
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
gold = (212, 175, 92, 255)
d.ellipse([4, 4, S - 5, S - 5], fill=gold)
m = 22
box = [m, m, S - m - 1, S - m - 1]
d.pieslice(box, 90, 270, fill=(18, 17, 22, 255))
d.pieslice(box, 270, 90, fill=(240, 236, 226, 255))
# The swirl: two half-size discs.
r = (S - 2 * m) // 4
cx = S // 2
d.ellipse([cx - r, m, cx + r, m + 2 * r], fill=(240, 236, 226, 255))
d.ellipse([cx - r, S - m - 2 * r - 1, cx + r, S - m - 1], fill=(18, 17, 22, 255))
dot = r // 3
d.ellipse([cx - dot, m + r - dot, cx + dot, m + r + dot], fill=(18, 17, 22, 255))
d.ellipse([cx - dot, S - m - r - dot - 1, cx + dot, S - m - r + dot - 1], fill=(240, 236, 226, 255))
img.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print('ok')
