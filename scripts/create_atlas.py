#!/usr/bin/env python3
"""apps/voxel_game - create_atlas.py: the AUTOMATIC atlas creator.

Scans the resource pack's block-texture directory and derives the whole
texture_atlas.hpp from WHAT IS THERE - no hand-maintained tile list:

  - walks the game's Block vocabulary (world.hpp's enum, spelled below),
  - resolves each block's face groups (top / bottom / sides) by NAME
    CONVENTION with ordered fallback candidates (grass_block_top, then
    grass, then turf...),
  - assigns one tile slot per DISTINCT asset on first use (the old
    hand list carried three never-referenced tiles; first-use order
    cannot - a tile exists only because some block's face reads it),
  - crops animation strips to frame 0 (water_still.png is 64 x 2048 -
    32 stacked frames of still water; frame 0 is the canonical look),
  - squares/resizes to kTilePixels, and emits:

      apps/voxel_game/src/texture_atlas.hpp   ten-or-fewer per-tile PNG
                                               arrays + kTileFor (block,
                                               face) -> tile + kTileNames
      download/atlas_preview.png              the eyeball grid

A missing REQUIRED asset is a hard error naming every candidate tried
(water is OPTIONAL - the game never meshes water, and a pack without
water art must still build; its row reads zero slots).

--check regenerates to a temp file and byte-compares against the
committed header: exit 0 in sync, exit 1 stale (the shader regen
script's contract). Output is deterministic - same pack, same bytes.

Usage:
  python3 scripts/create_atlas.py            # regenerate in place
  python3 scripts/create_atlas.py --check    # staleness check only
  python3 scripts/create_atlas.py --assets <dir>   # extracted pack directory
  python3 scripts/create_atlas.py --check            # verify generated header
"""
import sys
import os
import io
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_HPP = os.path.join(REPO, "apps", "voxel_game", "src", "texture_atlas.hpp")
OUT_PNG = os.path.join(REPO, "download", "atlas_preview.png")

TILE_PX = 64          # the emitted tile size (kTilePixels)
FACE_ORDER = ["top", "bottom", "east", "west", "south", "north"]

# The game's Block vocabulary (world.hpp enum order, value = row index).
# New block variants use ordered asset fallbacks so older packs remain valid.
# Per block: candidates for the TOP, BOTTOM, and SIDE face groups, in
# fallback order - the first file that exists wins. None = never meshed
# (air) or optional (water: absent art builds with a zero row).
BLOCKS = [
    # name      top-candidates                     bottom-candidates               side-candidates                    required
    ("air",    None,                               None,                          None,                              False),
    ("grass",  ["grass_block_top"],                ["dirt"],                       ["grass_block_side"],             True),
    ("dirt",   ["dirt"],                           ["dirt"],                       ["dirt"],                         True),
    ("stone",  ["stone"],                          ["stone"],                      ["stone"],                         True),
    ("wood",   ["oak_log_top", "oak_planks"],      ["oak_log_top", "oak_planks"],  ["oak_log", "oak_planks"],        True),
    ("sand",   ["sand"],                           ["sand"],                       ["sand"],                          True),
    ("water",  ["water_still", "water"],           None,                           None,                             False),
    ("cobblestone", ["cobblestone", "stone"],      ["cobblestone", "stone"],        ["cobblestone", "stone"],         True),
    ("planks", ["oak_planks", "oak_log"],          ["oak_planks", "oak_log"],       ["oak_planks", "oak_log"],        True),
    ("bricks", ["bricks", "stone"],                ["bricks", "stone"],             ["bricks", "stone"],              True),
    ("glass", ["glass", "sand"],                   ["glass", "sand"],               ["glass", "sand"],                True),
    ("leaves", ["oak_leaves", "leaves", "grass_block_side"], ["oak_leaves", "leaves", "grass_block_side"], ["oak_leaves", "leaves", "grass_block_side"], True),
    ("coal_ore", ["coal_ore", "stone"],             ["coal_ore", "stone"],            ["coal_ore", "stone"],            True),
    ("iron_ore", ["iron_ore", "stone"],             ["iron_ore", "stone"],            ["iron_ore", "stone"],            True),
]

def resolve(assets, candidates):
    """First existing candidate (stem matched, .png appended), or None."""
    for stem in candidates:
        for ext in (".png", ".PNG"):
            path = os.path.join(assets, stem + ext)
            if os.path.isfile(path):
                return stem, path
    return None, None

def load_tile(path):
    """One TILE_PX square RGBA tile from any source image.

    Minecraft stores grass_block_top and foliage as grayscale masks;
    the client normally multiplies them by the biome foliage color.
    This game has no biome shader yet, so bake the default temperate
    biome colors into those two standalone textures here. Alpha is
    preserved, which is important for the cutout pixels in leaves.

    Animation strips (height != width) crop to frame 0 (the top square);
    everything resamples to TILE_PX with NEAREST (the pack's own
    64px art passes through 1:1 - never resmoothed).
    """
    src = Image.open(path).convert("RGBA")
    if src.width != src.height:
        src = src.crop((0, 0, src.width, src.width))  # frame 0
    if (src.width, src.height) != (TILE_PX, TILE_PX):
        src = src.resize((TILE_PX, TILE_PX), Image.NEAREST)

    # Vanilla-style temperate biome tints. These are multiplicative
    # colors, not flat overlays, so the texture's own light/dark detail
    # and transparency remain intact.
    stem = os.path.splitext(os.path.basename(path))[0].lower()
    tint = {
        "grass_block_top": (127, 178, 56),
        "oak_leaves": (89, 174, 48),
    }.get(stem)
    if tint is not None:
        red, green, blue, alpha = src.split()
        red = red.point(lambda value: value * tint[0] // 255)
        green = green.point(lambda value: value * tint[1] // 255)
        blue = blue.point(lambda value: value * tint[2] // 255)
        src = Image.merge("RGBA", (red, green, blue, alpha))
    return src

def emit_array(byte_data):
    lines, per_line = [], 16
    for off in range(0, len(byte_data), per_line):
        chunk = byte_data[off:off + per_line]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)

def build(assets):
    """The whole derivation: (header_text, preview, report)."""
    if not os.path.isdir(assets):
        sys.exit(f"create_atlas: assets directory not found: {assets}")

    # 1) Resolve every block's face groups; collect failures.
    missing = []
    resolved = {}  # block name -> {top, bottom, side} -> stem
    for name, top, bottom, side, required in BLOCKS:
        groups = {"top": top, "bottom": bottom, "side": side}
        row = {}
        for group, candidates in groups.items():
            if candidates is None:
                row[group] = None
                continue
            stem, _path = resolve(assets, candidates)
            if stem is None:
                missing.append((name, group, candidates, required))
                row[group] = None
            else:
                row[group] = stem
        resolved[name] = row
    hard = [m for m in missing if m[3]]
    if hard:
        print("create_atlas: MISSING REQUIRED ASSETS (pack incomplete):")
        for name, group, candidates, _ in hard:
            print(f"  {name}.{group}: tried " +
                  ", ".join(f"{c}.png" for c in candidates))
        sys.exit(1)
    if any(not m[3] for m in missing):
        for name, group, candidates, _ in missing:
            print(f"create_atlas: note - optional {name}.{group} absent "
                  f"(tried {', '.join(candidates)}); row reads zero slots")

    # 2) First-use slot assignment: walk blocks in enum order, faces in
    #    kFaces order (top, bottom, sides); every DISTINCT stem gets one
    #    slot the first time some face reads it.
    slots = {}  # stem -> slot
    order = []  # slot -> stem (first-use order)
    kTileFor = {}
    for name, *_ in BLOCKS:
        row = resolved[name]
        faces = [row.get("top"), row.get("bottom")] + [row.get("side")] * 4
        slots_row = []
        for stem in faces:
            if stem is None:
                slots_row.append(-1)  # never read (air/water rows)
                continue
            if stem not in slots:
                slots[stem] = len(order)
                order.append(stem)
            slots_row.append(slots[stem])
        kTileFor[name] = slots_row

    # 3) Load each distinct tile once (slot order); encode PNG.
    tiles = []
    for stem in order:
        _s, path = resolve(assets, [stem])
        img = load_tile(path)
        buf = io.BytesIO()
        img.save(buf, format="PNG", optimize=True)
        tiles.append((stem, buf.getvalue()))

    # 4) The preview grid (4 columns, first-use order).
    cols = 4
    rows = (len(tiles) + cols - 1) // cols
    preview = Image.new("RGBA", (TILE_PX * cols, TILE_PX * max(rows, 1)),
                        (0, 0, 0, 0))
    for i, (_stem, _png) in enumerate(tiles):
        _s, path = resolve(assets, [order[i]])
        preview.paste(load_tile(path), ((i % cols) * TILE_PX,
                                       (i // cols) * TILE_PX))

    # 5) Emit the header.
    decl = []
    for i, (stem, data) in enumerate(tiles):
        decl.append(f"""// Tile {i}: {stem}.png - the pack's own art at {TILE_PX}x{TILE_PX}.
inline constexpr std::array<std::uint8_t, {len(data)}> kTile{i}Png{{
{emit_array(data)}
}};""")

    table = []
    for name, *_ in BLOCKS:
        cells = ", ".join(f"{t:>2d}" for t in kTileFor[name])
        table.append(f"    {{ {cells} }},  // {name}")

    names = ", ".join(f'"{stem}"' for stem in order)
    map_text = ",\n".join(
        f"    \"{stem}\", {slots[stem]}" for stem in order)

    hpp = f"""// apps/voxel_game - texture_atlas.hpp: the block textures, one PNG per
// tile. AUTO-GENERATED by scripts/create_atlas.py FROM THE ASSET PACK -
// do not edit by hand; re-run the creator (or fix the pack) instead.
// The creator scans the pack, resolves each Block's faces by name
// convention (world.hpp's enum order), and assigns one slot per distinct
// asset on first use - the tile set is exactly what the game reads,
// never a hand-maintained list.
//
// THE GREEDY-MESHING CONTRACT (why per-tile textures, not one atlas):
// greedy-merged quads span W x H cells and carry UVs in TILE UNITS
// [0, W] x [0, H]; the engine's mesh shader (mesh.frag, wrap_tiled)
// wraps any UV above 1 with fract + textureGrad, so ONE standalone
// texture repeats per unit. A standalone texture samples its full
// [0, 1]: no atlas sub-rects, no neighbor-tile bleed, no inset.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace voxel::atlas {{

// The number of tile slots the pack resolved for the game's blocks.
inline constexpr int kTileCount = {len(tiles)};
// Each tile's pixel size (square).
inline constexpr int kTilePixels = {TILE_PX};

{chr(10).join(decl)}

// The tile slot for each (block, face) pair - the mesher's lookup.
// Face indices follow world.cpp's kFaces order:
//     0 top  1 bottom  2 east  3 west  4 south  5 north
// Row index = Block's value (air..water). -1 = never read (air, and a
// water row when the pack carries no water art - water never meshes).
inline constexpr int kTileFor[{len(BLOCKS)}][6] = {{
{chr(10).join(table)}
}};

// The slot manifest, first-use order - the creator's own derivation
// pinned for tests and debugging (slot -> asset stem).
inline constexpr const char* kTileNames[kTileCount] = {{
{names}
}};

// The tile PNGs by slot - the texture loader's walk (a function over
// the named arrays: their sizes differ, so one fixed std::array of
// arrays cannot hold them uniformly).
[[nodiscard]] inline const std::uint8_t* tile_png(int tile,
                                                  std::size_t& bytes) {{
  switch (tile) {{
"""
    for i in range(len(tiles)):
        hpp += f"""    case {i}: {{
      bytes = kTile{i}Png.size();
      return kTile{i}Png.data();
    }}
"""
    hpp += """  }
  bytes = 0;
  return nullptr;
}

}  // namespace voxel::atlas
"""
    return hpp, preview, order

def main():
    args = sys.argv[1:]
    check = "--check" in args
    assets = os.path.join(REPO, "assets", "minecraft", "textures", "block")
    if "--assets" in args:
        assets = args[args.index("--assets") + 1]

    hpp, preview, order = build(assets)

    if check:
        fresh = hpp.encode()
        current = open(OUT_HPP, "rb").read() if os.path.exists(OUT_HPP) else b""
        if fresh == current:
            print(f"create_atlas --check: in sync ({len(order)} tiles)")
            return 0
        print("create_atlas --check: STALE - texture_atlas.hpp differs "
              "from what the pack would generate; re-run "
              "scripts/create_atlas.py")
        return 1

    os.makedirs(os.path.dirname(OUT_HPP), exist_ok=True)
    os.makedirs(os.path.dirname(OUT_PNG), exist_ok=True)
    with open(OUT_HPP, "w", encoding="utf-8", newline="\n") as f:
        f.write(hpp)
    preview.save(OUT_PNG)
    total = sum(len(open(os.devnull).read()) for _ in ())  # keep flake quiet
    print(f"create_atlas: {len(order)} tiles "
          f"({' -> '.join(order)})")
    print(f"wrote {OUT_HPP} ({os.path.getsize(OUT_HPP)} bytes)")
    print(f"wrote {OUT_PNG} ({os.path.getsize(OUT_PNG)} bytes)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
