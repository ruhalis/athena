# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4"]
# ///
"""Athena enclosure, compact variant, as a STEP assembly for Fusion 360.

Same numbers as athena_compact.scad and the Athena Enclosure page. Interior
coordinates: x right, y up, z back (0 = inside of the front edge; the display
face sits at z = -T, flush with the outside). The STEP uses a Y-up frame with
the front toward +Z, origin at the interior front-bottom-left corner, units
mm. make_cube_step.py is the earlier 200 mm cube for the 192 mm panel; this
file follows its structure and helpers.

    uv run enclosure/make_compact_step.py        # writes enclosure/athena_compact.step
    uv run enclosure/make_compact_step.py --stl  # also the four printed parts as STL in enclosure/stl/,
                                                 # each already in its print orientation with Z up
"""
import math
import sys
from pathlib import Path

import cadquery as cq

T = 3                           # wall
DISP = 128                      # display side (Waveshare RGB-Matrix-P2-64x64)
DT = 15                         # display thickness
GAP = 0.2                       # clearance: display to the pocket, lid to each side wall
W = DISP + 2 * GAP              # 128.4 interior width and height
D = 100                         # interior depth
S = W + 2 * T                   # 134.4 outer width and height
SEAT = DT - T                   # 12: interior z of the display's back
POST = 10                       # corner post section, M3 heat-set inserts
PAD = 22                        # corner pad behind each display corner
MOUNT = 13                      # display M3 from the interior corner (PLACEHOLDER, measure the panel)
FLARE = PAD - POST              # 12: the pad tapers to the post over this depth

CHAM = (45, 66, 66)             # speaker chamber outer: depth, height, length
CY = (W - CHAM[1]) / 2          # 31.2 chamber min y
CZ = 30                         # chamber min z
CC = W / 2                      # 64.2 centre of a side face in y
CZC = CZ + CHAM[2] / 2          # 63 grille centre z

PITCH = 4                       # grille: hex field of holes
HOLE_D = 2.4
GRILLE_R = 17.5                 # 35 mm field over the 40 mm driver's cone

MIC_X = (CC - 25, CC + 25)      # mic ports, 50 mm apart, centred on the top face
MIC_Z = 20                      # 23 mm behind the front edge

KIT_X = (12, W - 12 - 25.4)     # DevKit min x, flat on the floor, symmetric about the centre
KIT_Z = D - 65                  # 35: DevKit min z
JACK = (CC, 22)                 # DC-022 centre on the lid
WAGO = (42, 65)                 # WAGO 221-415 min x
AMP = (44, 40)                  # MAX98357A min x, min z
CAP = (72, 48)                  # 1000 uF centre x, z
VENT = (24, 66)                 # intake slot groups, floor
EXH = (14, 74.4)                # exhaust slot groups, lid

# the four corners: (pad min x, pad min y, post min x, post min y)
CORNERS = [(W - PAD if sx else 0, W - PAD if sy else 0, W - POST if sx else 0, W - POST if sy else 0)
           for sx in (0, 1) for sy in (0, 1)]


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


def flare(px, py, qx, qy):
    """45 degree taper from the pad's back face (PAD square at interior z SEAT + T)
    to the post section (POST square) FLARE further back; the two share a corner."""
    return (
        cq.Workplane("XY")
        .workplane(offset=-(SEAT + T))
        .center(px + PAD / 2, py + PAD / 2)
        .rect(PAD, PAD)
        .workplane(offset=-FLARE)
        .center((qx + POST / 2) - (px + PAD / 2), (qy + POST / 2) - (py + PAD / 2))
        .rect(POST, POST)
        .loft()
    )


def mount_xy(c):
    """Display M3 centre for corner c, MOUNT in from the interior corner."""
    px, py = c[0], c[1]
    return (px + (PAD - MOUNT if px else MOUNT), py + (PAD - MOUNT if py else MOUNT))


def grille_pts():
    """Hex field of grille hole centres as (dy, dz) offsets from the grille centre."""
    pts = []
    for j in range(-5, 6):
        for i in range(-6, 7):
            dy = j * PITCH * math.sqrt(3) / 2
            dz = (i + (abs(j) % 2) / 2) * PITCH
            if math.hypot(dy, dz) <= GRILLE_R - HOLE_D / 2:
                pts.append((dy, dz))
    return pts


def shell():
    b = blk(-T, -T, -T, S, S, D + 2 * T).cut(blk(0, 0, -T - 1, W, W, D + 2 * T + 2))   # the sleeve
    for px, py, qx, qy in CORNERS:
        b = b.union(blk(px, py, SEAT, PAD, PAD, T))                        # pad behind the display's corner
        b = b.union(flare(px, py, qx, qy))                                 # 45 degree flare to the post
        b = b.union(blk(qx, qy, SEAT + T, POST, POST, D - SEAT - T))       # corner post to the lid

    for c in CORNERS:
        qx, qy = c[2], c[3]
        b = b.cut(cyl_z(qx + POST / 2, qy + POST / 2, D - 8, 2, 9))        # M3 heat-set holes, diam 4 x 8
        mx, my = mount_xy(c)
        b = b.cut(cyl_z(mx, my, SEAT - 1, 1.7, T + 2))                     # display M3, diam 3.4 through the pad
        b = b.cut(cyl_z(mx, my, SEAT + T, 3.25, FLARE + 4))                # diam 6.5 counterbore through the flare

    for mx in MIC_X:                                                       # mic ports through the top wall
        b = b.cut(cyl_y(mx, W - 1, MIC_Z, 0.75, T + 2))                     # diam 1.5, through
        b = b.cut(cyl_y(mx, W - 1, MIC_Z, 4, 2.5))                          # diam 8 x 1.5 seat from inside

    for sx in (-T - 1, W - 1):                                             # speaker grilles, left and right walls
        pts = [(CC + dy, -(CZC + dz)) for dy, dz in grille_pts()]
        grille = (
            cq.Workplane("YZ")
            .workplane(offset=sx)
            .pushPoints(pts)
            .circle(HOLE_D / 2)
            .extrude(T + 2)
        )
        b = b.cut(grille)

    for zr in (22, 27.5):                                                  # intake slots, bottom wall, behind the display
        for xg in VENT:
            b = b.cut(blk(xg, -T - 1, zr, 40, T + 2, 2.5))

    return b


def lid():
    b = blk(GAP, GAP, D, W - 2 * GAP, W - 2 * GAP, T)
    for _, _, qx, qy in CORNERS:
        b = b.cut(cyl_z(qx + POST / 2, qy + POST / 2, D - 1, 1.7, T + 2))  # M3 clearance
    b = b.cut(cyl_z(JACK[0], JACK[1], D - 1, 4.2, T + 2))                  # DC-022, diam 8 thread
    for kx in KIT_X:
        b = b.cut(blk(kx + 0.5, 5, D - 1, 24.4, 9, T + 2))                 # USB-C cut-outs, one per DevKit
    for i in range(4):
        for xg in EXH:
            b = b.cut(blk(xg, 100 + i * 5.5, D - 1, 40, 2.5, T + 2))       # exhaust slots near the top
    return b


def chamber_l():
    """Left speaker chamber, far wall at x 42..45."""
    b = blk(0, CY, CZ, CHAM[0], CHAM[1], CHAM[2])
    b = b.cut(blk(-1, CY + T, CZ + T, CHAM[0] - T + 1, CHAM[1] - 2 * T, CHAM[2] - 2 * T))
    b = b.cut(cyl_x(CHAM[0] - T - 1, CY + 8, CZ + 8, 1.5, T + 2))          # lead pass-through
    return b


def chamber_r():
    """Right speaker chamber, far wall at x 83.4..86.4."""
    b = blk(W - CHAM[0], CY, CZ, CHAM[0], CHAM[1], CHAM[2])
    b = b.cut(blk(W - CHAM[0] + T, CY + T, CZ + T, CHAM[0] - T + 1, CHAM[1] - 2 * T, CHAM[2] - 2 * T))
    b = b.cut(cyl_x(W - CHAM[0] - 1, CY + 8, CZ + 8, 1.5, T + 2))
    return b


def devkit(x0):
    """ESP32-S3-DevKitC-1 flat on the floor, USB-C toward the lid, as one solid."""
    b = blk(x0, 6, KIT_Z, 25.4, 1.6, 63)                                   # PCB on 6 mm standoffs
    b = b.union(blk(x0 + 0.2, 7.6, KIT_Z, 25, 3.2, 18))                    # module can at the front end
    b = b.union(blk(x0 + 2, 7.6, KIT_Z + 63 - 7.3, 9, 3.3, 7.8))           # USB-C x2
    b = b.union(blk(x0 + 14.4, 7.6, KIT_Z + 63 - 7.3, 9, 3.3, 7.8))
    for px, pz in ((3, 3), (22.4, 3), (3, 60), (22.4, 60)):
        b = b.union(cyl_y(x0 + px, 0, KIT_Z + pz, 1.6, 6))                 # standoffs
    return b


def rgb(hexstr):
    h = hexstr.lstrip("#")
    return cq.Color(*(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)))


WALL = cq.Color(0.84, 0.87, 0.90, 0.5)


def build():
    """The whole assembly: printed parts first, then everything they hold."""
    assy = cq.Assembly(name="athena_compact")
    assy.add(shell(), name="Shell 3mm", color=WALL)
    assy.add(lid(), name="Lid 3mm", color=WALL)
    assy.add(chamber_l(), name="Speaker chamber L", color=WALL)
    assy.add(chamber_r(), name="Speaker chamber R", color=WALL)
    assy.add(blk(GAP, GAP, -T, DISP, DISP, DT), name="Panel P2 64x64", color=rgb("#1E2530"))
    assy.add(blk(MIC_X[0] - 8.5, W - 3, MIC_Z - 6.5, 17, 3, 13), name="Mic L", color=rgb("#23408F"))
    assy.add(blk(MIC_X[1] - 8.5, W - 3, MIC_Z - 6.5, 17, 3, 13), name="Mic R", color=rgb("#23408F"))
    assy.add(cyl_x(0, CC, CZC, 20, 4).union(cyl_x(4, CC, CZC, 10, 12)),
             name="Speaker L 40mm", color=rgb("#2A2A2A"))
    assy.add(cyl_x(W - 4, CC, CZC, 20, 4).union(cyl_x(W - 16, CC, CZC, 10, 12)),
             name="Speaker R 40mm", color=rgb("#2A2A2A"))
    assy.add(blk(AMP[0], 0, AMP[1], 18, 3, 17), name="Amp MAX98357A", color=rgb("#6B3FA0"))
    assy.add(cyl_y(CAP[0], 0, CAP[1], 5, 20), name="Cap 1000uF", color=rgb("#17285C"))
    assy.add(devkit(KIT_X[0]), name="Matrix ESP32-S3 DevKitC-1", color=rgb("#0F1418"))
    assy.add(devkit(KIT_X[1]), name="Audio ESP32-S3 DevKitC-1", color=rgb("#0F1418"))
    assy.add(blk(JACK[0] - 7, JACK[1] - 7, D - 16, 14, 14, 16), name="DC-022 jack", color=rgb("#222222"))
    assy.add(blk(WAGO[0], 0, 62, 21, 8, 19), name="WAGO 221-415 +5V", color=rgb("#F28C28"))
    assy.add(blk(WAGO[1], 0, 62, 21, 8, 19), name="WAGO 221-415 GND", color=rgb("#F28C28"))
    feet = None
    for fx in (15, W - 15):
        for fz in (15, D - 15):
            foot = cyl_y(fx, -T - 6, fz, 6, 6)
            feet = foot if feet is None else feet.union(foot)
    assy.add(feet, name="Feet x4", color=rgb("#333333"))
    return assy


def print_parts():
    """The four printed parts, each rotated into its print orientation (slicer Z up) and
    set down on Z = 0: the shell front edge up (its back opening on the bed), the lid
    flat, each chamber open side down."""
    def on_bed(shape):
        bb = shape.val().BoundingBox()
        return shape.translate((-bb.xmin, -bb.ymin, -bb.zmin))
    # the STEP frame already has the front toward +Z, so the shell and the lid only need setting down
    parts = {
        "shell": on_bed(shell()),
        "lid": on_bed(lid()),
        # open side faces -X (left) or +X (right): turn it toward -Z
        "chamber_L": on_bed(chamber_l().rotate((0, 0, 0), (0, 1, 0), -90)),
        "chamber_R": on_bed(chamber_r().rotate((0, 0, 0), (0, 1, 0), 90)),
    }
    return parts


if __name__ == "__main__":
    out = Path(__file__).with_name("athena_compact.step")
    build().export(str(out))
    print("wrote", out, out.stat().st_size, "bytes")
    if "--stl" in sys.argv:
        stl_dir = Path(__file__).with_name("stl")
        stl_dir.mkdir(exist_ok=True)
        for name, shape in print_parts().items():
            f = stl_dir / f"athena_compact_{name}.stl"
            cq.exporters.export(shape, str(f), tolerance=0.02, angularTolerance=0.1)
            bb = shape.val().BoundingBox()
            print(f"wrote {f} {f.stat().st_size} bytes, {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f} mm, Z up")
