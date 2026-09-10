# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4"]
# ///
"""Athena enclosure, cube variant, as a STEP assembly for Fusion 360.

Same numbers as athena_cube.scad and the Athena Enclosure page. Interior
coordinates: x right, y up, z back (0 = inside of the front wall). The STEP
uses a Y-up frame with the front toward +Z, origin at the interior
front-bottom-left corner, units mm. make_step.py is the earlier port of the
slab variant (athena_box.scad); this file follows its structure and helpers.

    uv run enclosure/make_cube_step.py        # writes enclosure/athena_cube.step
"""
import math
from pathlib import Path

import cadquery as cq

T = 3                           # wall
S = 200                         # outer side, the same on all three axes
W = S - 2 * T                   # 194 interior, all three axes
LIP = 0.6                       # front frame overlap onto the panel edge
WIN = 192 - 2 * LIP             # 190.8 window
WX = (W - WIN) / 2              # 1.6 window offset from the interior wall
GAP = 0.2                       # lid clearance to each side wall
POST = 10                       # corner post section, M3 heat-set inserts

CHAM = (45, 66, 66)             # speaker chamber outer: depth, height, length
CY = (W - CHAM[1]) / 2          # 64 chamber min y
CZ = (W - CHAM[2]) / 2          # 64 chamber min z
CC = W / 2                      # 97 centre of a side face in y and z

PITCH = 4                       # grille: hex field of holes
HOLE_D = 2.4
GRILLE_R = 17.5                 # 35 mm field over the 40 mm driver's cone

MIC_X = (72, 122)               # mic ports, 50 mm apart, centred on the top face
MIC_Z = 20                      # 23 mm behind the front face

KIT_X = (40, 128.6)             # DevKit min x, flat on the floor, symmetric about the centre
KIT_Z = 129                     # DevKit min z
JACK = (97, 22)                 # DC-022 centre on the lid


def blk(x, y, z, w, h, d):
    """Box with its min corner at interior (x, y, z) and size (w, h, d)."""
    return cq.Workplane("XY").box(w, h, d, centered=False).translate((x, y, -(z + d)))


def cyl_x(cx, cy, cz, r, length):
    """Cylinder, axis right, base at x = cx."""
    s = cq.Solid.makeCylinder(r, length, cq.Vector(cx, cy, -cz), cq.Vector(1, 0, 0))
    return cq.Workplane("XY").newObject([s])


def cyl_y(cx, cy, cz, r, length):
    """Cylinder, axis up, base at y = cy."""
    s = cq.Solid.makeCylinder(r, length, cq.Vector(cx, cy, -cz), cq.Vector(0, 1, 0))
    return cq.Workplane("XY").newObject([s])


def cyl_z(cx, cy, z0, r, length):
    """Cylinder, axis front-to-back, from interior z0 toward the back."""
    s = cq.Solid.makeCylinder(r, length, cq.Vector(cx, cy, -z0), cq.Vector(0, 0, -1))
    return cq.Workplane("XY").newObject([s])


def grille_pts():
    """Hex field of grille hole centres as (dy, dz) offsets from the face centre."""
    pts = []
    for j in range(-5, 6):
        for i in range(-6, 7):
            dy = j * PITCH * math.sqrt(3) / 2
            dz = (i + (abs(j) % 2) / 2) * PITCH
            if math.hypot(dy, dz) <= GRILLE_R - HOLE_D / 2:
                pts.append((dy, dz))
    return pts


def shell():
    b = blk(-T, -T, -T, S, S, S).cut(blk(0, 0, 0, W, W, W + T + 1))     # interior, open at the back for the lid
    for px in (0, W - POST):
        for py in (0, W - POST):
            b = b.union(blk(px, py, 16, POST, POST, W - 16))            # corner posts, front ends hold the panel

    # window: 190.8 square, 45 degree chamfer over the outer 2 mm to a 194.8
    # square at the front face, as in the SCAD: a frustum from the front face
    # (z -3) to z -1 plus the straight 190.8 prism behind it.
    win_a = blk(WX, WX, -1, WIN, WIN, 2)                                 # 190.8 square, z -1..1
    win_b = blk(WX - 2, WX - 2, -T - 1, WIN + 4, WIN + 4, 1)             # 194.8 square, z -4..-3
    wc = WX + WIN / 2
    frustum = (
        cq.Workplane("XY")
        .workplane(offset=3)                                            # interior z -3
        .center(wc, wc)
        .rect(WIN + 4, WIN + 4)
        .workplane(offset=-2)                                           # interior z -1
        .rect(WIN, WIN)
        .loft()
    )
    b = b.cut(win_b.union(frustum).union(win_a))

    for px in (0, W - POST):
        for py in (0, W - POST):
            b = b.cut(cyl_z(px + POST / 2, py + POST / 2, W - 8, 2, 9))  # M3 heat-set holes, diam 4 x 8

    for mx in MIC_X:                                                    # mic ports through the top wall
        b = b.cut(cyl_y(mx, W - 1, MIC_Z, 0.75, T + 2))                  # diam 1.5, through
        b = b.cut(cyl_y(mx, W - 1, MIC_Z, 4, 2.5))                       # diam 8 x 1.5 seat from inside

    for sx in (-T - 1, W - 1):                                          # speaker grilles, left and right walls
        pts = [(CC + dy, -(CC + dz)) for dy, dz in grille_pts()]
        grille = (
            cq.Workplane("YZ")
            .workplane(offset=sx)
            .pushPoints(pts)
            .circle(HOLE_D / 2)
            .extrude(T + 2)
        )
        b = b.cut(grille)

    for zr in (24, 29.5):                                               # intake slots, bottom wall, behind the panel
        for xg in (20, 77, 134):
            b = b.cut(blk(xg, -T - 1, zr, 40, T + 2, 2.5))

    return b


def lid():
    b = blk(GAP, GAP, W, W - 2 * GAP, W - 2 * GAP, T)
    for px in (0, W - POST):
        for py in (0, W - POST):
            b = b.cut(cyl_z(px + POST / 2, py + POST / 2, W - 1, 1.7, T + 2))   # M3 clearance
    b = b.cut(cyl_z(JACK[0], JACK[1], W - 1, 4.2, T + 2))                       # DC-022, diam 8 thread
    for kx in KIT_X:
        b = b.cut(blk(kx + 0.5, 5, W - 1, 24.4, 9, T + 2))                      # USB-C cut-outs, one per DevKit
    for i in range(4):
        for xg in (20, 134):
            b = b.cut(blk(xg, 165 + i * 5.5, W - 1, 40, 2.5, T + 2))            # exhaust slots near the top
    return b


def chamber_l():
    """Left speaker chamber, far wall at x 42..45."""
    b = blk(0, CY, CZ, CHAM[0], CHAM[1], CHAM[2])
    b = b.cut(blk(-1, CY + T, CZ + T, CHAM[0] - T + 1, CHAM[1] - 2 * T, CHAM[2] - 2 * T))
    b = b.cut(cyl_x(CHAM[0] - T - 1, CY + 8, CZ + 8, 1.5, T + 2))               # lead pass-through
    return b


def chamber_r():
    """Right speaker chamber, far wall at x 149..152."""
    b = blk(W - CHAM[0], CY, CZ, CHAM[0], CHAM[1], CHAM[2])
    b = b.cut(blk(W - CHAM[0] + T, CY + T, CZ + T, CHAM[0] - T + 1, CHAM[1] - 2 * T, CHAM[2] - 2 * T))
    b = b.cut(cyl_x(W - CHAM[0] - 1, CY + 8, CZ + 8, 1.5, T + 2))
    return b


def devkit(x0):
    """ESP32-S3-DevKitC-1 flat on the floor, USB-C toward the lid, as one solid."""
    b = blk(x0, 6, KIT_Z, 25.4, 1.6, 63)                                        # PCB on 6 mm standoffs
    b = b.union(blk(x0 + 0.2, 7.6, KIT_Z, 25, 3.2, 18))                         # module can at the front end
    b = b.union(blk(x0 + 2, 7.6, KIT_Z + 63 - 7.3, 9, 3.3, 7.8))                # USB-C x2
    b = b.union(blk(x0 + 14.4, 7.6, KIT_Z + 63 - 7.3, 9, 3.3, 7.8))
    for px, pz in ((3, 3), (22.4, 3), (3, 60), (22.4, 60)):
        b = b.union(cyl_y(x0 + px, 0, KIT_Z + pz, 1.6, 6))                      # standoffs
    return b


def rgb(hexstr):
    h = hexstr.lstrip("#")
    return cq.Color(*(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)))


WALL = cq.Color(0.84, 0.87, 0.90, 0.5)

assy = cq.Assembly(name="athena_cube")
assy.add(shell(), name="Shell 3mm", color=WALL)
assy.add(lid(), name="Lid 3mm", color=WALL)
assy.add(chamber_l(), name="Speaker chamber L", color=WALL)
assy.add(chamber_r(), name="Speaker chamber R", color=WALL)
assy.add(blk(1, 1, 0, 192, 192, 15), name="Panel P3 64x64", color=rgb("#1E2530"))
assy.add(blk(MIC_X[0] - 8.5, W - 3, MIC_Z - 6.5, 17, 3, 13), name="Mic L", color=rgb("#23408F"))
assy.add(blk(MIC_X[1] - 8.5, W - 3, MIC_Z - 6.5, 17, 3, 13), name="Mic R", color=rgb("#23408F"))
assy.add(cyl_x(0, CC, CC, 20, 4).union(cyl_x(4, CC, CC, 10, 12)),
         name="Speaker L 40mm", color=rgb("#2A2A2A"))
assy.add(cyl_x(W - 4, CC, CC, 20, 4).union(cyl_x(W - 16, CC, CC, 10, 12)),
         name="Speaker R 40mm", color=rgb("#2A2A2A"))
assy.add(blk(160, 0, 95, 18, 3, 17), name="Amp MAX98357A", color=rgb("#6B3FA0"))
assy.add(cyl_y(172, 0, 122, 5, 20), name="Cap 1000uF", color=rgb("#17285C"))
assy.add(devkit(KIT_X[0]), name="Matrix ESP32-S3 DevKitC-1", color=rgb("#0F1418"))
assy.add(devkit(KIT_X[1]), name="Audio ESP32-S3 DevKitC-1", color=rgb("#0F1418"))
assy.add(blk(JACK[0] - 7, JACK[1] - 7, W - 16, 14, 14, 16), name="DC-022 jack", color=rgb("#222222"))
assy.add(blk(75, 0, 150, 21, 8, 19), name="WAGO 221-415 +5V", color=rgb("#F28C28"))
assy.add(blk(98, 0, 150, 21, 8, 19), name="WAGO 221-415 GND", color=rgb("#F28C28"))
feet = None
for fx in (15, W - 15):
    for fz in (15, W - 15):
        foot = cyl_y(fx, -T - 6, fz, 6, 6)
        feet = foot if feet is None else feet.union(foot)
assy.add(feet, name="Feet x4", color=rgb("#333333"))

out = Path(__file__).with_name("athena_cube.step")
assy.export(str(out))
print("wrote", out, out.stat().st_size, "bytes")
