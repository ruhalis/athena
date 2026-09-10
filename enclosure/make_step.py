# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["cadquery>=2.4"]
# ///
"""Athena enclosure, block model, as a STEP assembly for Fusion 360.

Same numbers as athena_box.scad and the Athena Enclosure page. Interior
coordinates: x right, y up, z back (0 = inside of the front wall). The STEP
uses a Y-up frame with the front toward +Z, origin at the interior
front-bottom-left corner, units mm.

    uv run enclosure/make_step.py        # writes enclosure/athena_box.step
"""
from pathlib import Path

import cadquery as cq

T, W, H, D = 3, 200, 248, 50


def blk(x, y, z, w, h, d):
    """Box with its min corner at interior (x, y, z) and size (w, h, d)."""
    return cq.Workplane("XY").box(w, h, d, centered=False).translate((x, y, -(z + d)))


def cyl_y(cx, cy, cz, r, length):
    """Cylinder, axis up, base at y = cy."""
    s = cq.Solid.makeCylinder(r, length, cq.Vector(cx, cy, -cz), cq.Vector(0, 1, 0))
    return cq.Workplane("XY").newObject([s])


def cyl_z(cx, cy, z0, r, length):
    """Cylinder, axis front-to-back, from interior z0 toward the back."""
    s = cq.Solid.makeCylinder(r, length, cq.Vector(cx, cy, -z0), cq.Vector(0, 0, -1))
    return cq.Workplane("XY").newObject([s])


def shell():
    b = blk(-T, -T, -T, W + 2 * T, H + 2 * T, D + 2 * T).cut(blk(0, 0, 0, W, H, D))
    b = b.cut(blk(4, 52, -T - 1, 192, 192, T + 2))                # panel window
    b = b.cut(cyl_z(75, 26, -T - 1, 0.75, T + 2))                  # mic ports, Ø1.5
    b = b.cut(cyl_z(125, 26, -T - 1, 0.75, T + 2))
    b = b.cut(cyl_y(171.5, -T - 1, 25, 22, T + 2))                 # speaker opening, bottom
    b = b.cut(cyl_z(29, 73, D - 1, 4.2, T + 2))                    # DC-022 thread, back
    b = b.cut(blk(W - 1, 112.5, 36, T + 2, 24.4, 9))               # USB-C cut-outs, right wall
    b = b.cut(blk(W - 1, 206.5, 36, T + 2, 24.4, 9))
    for i in range(4):                                             # vents, back wall
        b = b.cut(blk(20, 224 + i * 5.5, D - 1, 40, 2.5, T + 2))
        b = b.cut(blk(140, 224 + i * 5.5, D - 1, 40, 2.5, T + 2))
    return b


def devkit(x, y):
    """ESP32-S3-DevKitC-1 flat on the back wall, USB-C toward +x."""
    b = blk(x, y, 44.4, 63, 25.4, 1.6)                             # PCB
    b = b.union(blk(x, y + 0.2, 41.2, 18, 25, 3.2))                # module can
    b = b.union(blk(x + 54, y + 4, 37.1, 9, 3.3, 7.3))             # USB-C, UART
    b = b.union(blk(x + 54, y + 18.1, 37.1, 9, 3.3, 7.3))          # USB-C, native
    for px, py in ((3, 3), (60, 3), (3, 22.4), (60, 22.4)):        # standoffs
        b = b.union(cyl_z(x + px, y + py, 46, 1.6, 4))
    return b


def rgb(hexstr):
    h = hexstr.lstrip("#")
    return cq.Color(*(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)))


assy = cq.Assembly(name="athena_box")
assy.add(shell(), name="Box walls 3mm", color=cq.Color(0.84, 0.87, 0.90, 0.5))
assy.add(blk(140, 0, 0, T, 52, D).union(blk(143, 49, 0, 57, T, D)),
         name="Speaker chamber walls", color=cq.Color(0.84, 0.87, 0.90, 0.5))
assy.add(blk(4, 52, 0, 192, 192, 15), name="Panel P3 64x64", color=rgb("#1E2530"))
assy.add(blk(66.5, 19.5, 0, 17, 13, 3), name="Mic L", color=rgb("#23408F"))
assy.add(blk(116.5, 19.5, 0, 17, 13, 3), name="Mic R", color=rgb("#23408F"))
assy.add(cyl_y(171.5, 0, 25, 25, 4).union(cyl_y(171.5, 2, 25, 12, 16)),
         name="Speaker 50mm", color=rgb("#2E2E2E"))
assy.add(blk(150, 58, 47, 18, 17, 3).union(blk(151, 66, 44, 8, 6, 3)),
         name="Amp MAX98357A", color=rgb("#6B3FA0"))
assy.add(cyl_y(178, 58, 42, 5, 20), name="Cap 1000uF", color=rgb("#17285C"))
assy.add(devkit(133, 112), name="Audio ESP32-S3 DevKitC-1", color=rgb("#0F1418"))
assy.add(devkit(133, 206), name="Matrix ESP32-S3 DevKitC-1", color=rgb("#0F1418"))
assy.add(blk(22, 66, 34, 14, 14, 16).union(cyl_z(29, 73, 50, 4, 3)).union(cyl_z(29, 73, 53, 6, 6)),
         name="DC-022 jack", color=rgb("#222222"))
assy.add(blk(44, 64, 42, 21, 19, 8), name="WAGO 221-415 +5V", color=rgb("#F28C28"))
assy.add(blk(69, 64, 42, 21, 19, 8), name="WAGO 221-415 GND", color=rgb("#F28C28"))

out = Path(__file__).with_name("athena_box.step")
assy.export(str(out))
print("wrote", out, out.stat().st_size, "bytes")
