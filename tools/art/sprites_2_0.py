# Generates the 2.0 sprites (THE ASHEN FLEET flagship, the markets' CLOSED sign) as character
# grids + preview PNGs: python tools/art/sprites_2_0.py <out dir>. Paste the printed grids into
# ASHEN_SPRITE / CLOSED_SPRITE in ShadowFleet_Prototype_02.ino AND web_server.cpp (PAGE_HTML).
import sys
from PIL import Image

# ---- The Ashen Fleet flagship: a scorched wedge battleship, nose left ----
# Seen from above: an arrowhead hull, a raised spine, a bridge block near the
# stern, three engine bells glowing ember, scorch marks.
W, H = 40, 17
g = [['.'] * W for _ in range(H)]
mid = 8
for x in range(0, 36):
    half = round(x * 8.0 / 35)
    for y in range(mid - half, mid + half + 1):
        g[y][x] = 'h'
    g[mid - half][x] = 'l'                     # lit upper edge
    g[mid + half][x] = 'd'                     # shadowed lower edge
    if x > 8 and x % 6 == 0:                   # plating seams
        for y in range(mid - half + 1, mid + half):
            if abs(y - mid) > 1: g[y][x] = 'd'
for x in range(6, 34):                         # the spine
    g[mid][x] = 'l'
    if x > 14: g[mid - 1][x] = 'l'; g[mid + 1][x] = 'd'
for x in range(25, 32):                        # bridge block
    for y in range(mid - 3, mid + 4):
        g[y][x] = 'l' if y == mid - 3 or x == 25 else ('d' if y == mid + 3 or x == 31 else 'h')
for x in range(27, 30): g[mid - 1][x] = 'y'    # lit bridge windows
for y0 in (2, mid - 1, 13):                    # three engine bells
    for y in range(y0, y0 + 3):
        if 0 <= y < H:
            g[y][36] = 'r'; g[y][37] = 'e'; g[y][38] = 'y' if y == y0 + 1 else 'e'
            if y == y0 + 1: g[y][39] = 'e'
# scorch cracks (it's the ASHEN fleet)
for (x, y) in [(10, 6), (11, 7), (17, 4), (18, 5), (19, 5), (21, 11), (22, 12), (14, 10), (33, 3), (34, 13)]:
    if g[y][x] != '.': g[y][x] = 'e' if (x + y) % 2 else 'r'
ASHEN = [''.join(r) for r in g]

# ---- CLOSED: a hanging wooden sign, red painted letters ----
FONT = {  # 5x7
 'C': ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'],
 'L': ['#....', '#....', '#....', '#....', '#....', '#....', '#####'],
 'O': ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
 'S': ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'],
 'E': ['#####', '#....', '#....', '####.', '#....', '#....', '#####'],
 'D': ['####.', '#...#', '#...#', '#...#', '#...#', '#...#', '####.'],
}
SW, SH = 46, 22
s = [['.'] * SW for _ in range(SH)]
# chains from two nails to the board
for y in range(0, 6):
    for x in (9, 36):
        s[y][x - (y % 2)] = 'c'
s[0][8] = s[0][9] = s[0][36] = s[0][35] = 'n'
# the board: three planks, darker seams, a nail at each end of each plank
for y in range(6, SH):
    for x in range(1, SW - 1):
        s[y][x] = 'v' if (x * 7 + y * 3) % 11 == 0 else 'w'   # wood grain
for x in range(1, SW - 1): s[8][x] = 'k'; s[19][x] = 'k'     # plank seams, clear of the paint
for y in range(6, SH):
    s[y][1] = s[y][SW - 2] = 'k'
for x in range(1, SW - 1): s[6][x] = 'k'; s[SH - 1][x] = 'k'
for py in (7, 13, 20):
    s[py][3] = s[py][SW - 4] = 'n'
# letters, painted red, centered
word = 'CLOSED'
x0 = (SW - (len(word) * 6 - 1)) // 2
for i, ch in enumerate(word):
    for ry, row in enumerate(FONT[ch]):
        for rx, px in enumerate(row):
            if px == '#': s[10 + ry][x0 + i * 6 + rx] = 'R'
CLOSED = [''.join(r) for r in s]

PAL = {'h': (74, 80, 96), 'l': (122, 130, 150), 'd': (42, 46, 56), 'y': (255, 210, 122), 'e': (255, 140, 66), 'r': (224, 64, 74),
       'c': (140, 140, 150), 'n': (200, 200, 210), 'w': (139, 94, 52), 'v': (120, 80, 44), 'k': (70, 44, 24), 'R': (214, 40, 40)}
for name, grid in (('ashen', ASHEN), ('closed', CLOSED)):
    im = Image.new('RGB', (len(grid[0]) * 8, len(grid) * 8), (5, 7, 12))
    for y, row in enumerate(grid):
        for x, ch in enumerate(row):
            if ch in PAL:
                for dy in range(8):
                    for dx in range(8): im.putpixel((x * 8 + dx, y * 8 + dy), PAL[ch])
    im.save(sys.argv[1] + '/' + name + '.png')
    print(name, len(grid[0]), 'x', len(grid))
    for r in grid: print('"' + r + '",')
