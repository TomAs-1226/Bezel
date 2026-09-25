"""Draw the app's icons (src-tauri/icons/) from the identity's tokens: two chain links, one white and one
crimson, on the ground tone. Run with Pillow: python scripts/make-icons.py (the output is committed)."""
from pathlib import Path

from PIL import Image, ImageDraw

GROUND = (13, 14, 18, 255)       # --cat-ground
SURFACE = (26, 28, 33, 255)      # --cat-surface-2
INK = (232, 233, 236, 255)       # --cat-ink
SIGNAL = (233, 69, 96, 255)      # --cat-signal
OUT = Path(__file__).resolve().parents[1] / "src-tauri" / "icons"
S = 1024  # drawn large, scaled down


def link(size: int, angle: float, offset: tuple[float, float], color: tuple[int, int, int, int], width: int) -> Image.Image:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    w, h = size * 0.44, size * 0.21
    cx, cy = size / 2 + offset[0], size / 2 + offset[1]
    d.rounded_rectangle((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), radius=h / 2, outline=color, width=width)
    return layer.rotate(angle, resample=Image.BICUBIC, center=(size / 2, size / 2))


def draw() -> Image.Image:
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((32, 32, S - 32, S - 32), radius=220, fill=GROUND, outline=SURFACE, width=18)
    stroke = 74
    im.alpha_composite(link(S, 45, (-150, 0), INK, stroke))
    im.alpha_composite(link(S, 45, (150, 0), SIGNAL, stroke))
    return im


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    big = draw()
    for name, px in (("32x32.png", 32), ("128x128.png", 128), ("128x128@2x.png", 256), ("icon.png", 512)):
        big.resize((px, px), Image.LANCZOS).save(OUT / name)
    big.resize((256, 256), Image.LANCZOS).save(OUT / "icon.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                                                                      (64, 64), (128, 128), (256, 256)])
    print(f"icons written to {OUT}")


if __name__ == "__main__":
    main()
