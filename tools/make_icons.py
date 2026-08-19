#!/usr/bin/env python3
"""从原画生成应用图标资源。

用法:  python tools/make_icons.py [原画路径]        # 默认 assets/source-artwork.png

这是**开发期工具**，不在 CI 里跑（CI 机器没有 Pillow，也不需要重新生成图标——
assets/ 下的产物是提交进仓库的）。放在这里是为了让图标可复现：
换原画、调裁切范围之后重跑一次就行，不用记得当初是怎么切的。
母图本身也提交进 assets/，否则"可复现"是假的——图标只能从现有产物再降质放大。

两件事值得说明：

① **分尺寸出图**。原画是一整幅插画（牛、蜡烛图、披风、笔记本），直接缩到 16×16
   就是一团噪点。所以大尺寸(≥48)用宽裁切保留场景，小尺寸(≤32)用窄裁切让脸占满，
   保证任务栏/标题栏上还认得出是谁。真正的图标集都是这么做的。

② **ICO 里 ≤64 用 BMP、≥128 用 PNG**。PNG 压缩的 ICO 条目从 Vista 起就支持，
   但小尺寸用 BMP 是兼容性最稳的写法，而 256×256 用 BMP 会让文件白白大 4 倍。
"""
import struct
import sys
from pathlib import Path

from PIL import Image

# 裁切范围（原画像素坐标，left, top, right, bottom），必须是正方形
CROP_WIDE = (300, 60, 1580, 1340)    # 头 + 围巾 + 牛角，用于 ≥48px
CROP_TIGHT = (420, 230, 1360, 1170)  # 收到脸部，用于 ≤32px

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
QRC_SIZES = [16, 24, 32, 48, 64, 128, 256]   # 窗口/任务栏图标走 Qt 资源
TIGHT_MAX = 32                                # 到这个尺寸为止用窄裁切

BANNER_WIDTH = 1200
BANNER_LEFT_TRIM = 110   # 原图左边缘带了个轮播箭头 "‹"（截图残留），裁掉


def render(src: Image.Image, px: int) -> Image.Image:
    box = CROP_TIGHT if px <= TIGHT_MAX else CROP_WIDE
    w, h = box[2] - box[0], box[3] - box[1]
    assert w == h, f"裁切范围不是正方形: {box} -> {w}x{h}"
    return src.crop(box).resize((px, px), Image.LANCZOS)


def bmp_entry(im: Image.Image) -> bytes:
    """把一张 RGBA 图打包成 ICO 里的 BMP 条目（DIB + 全零 AND 掩码）。"""
    w, h = im.size
    px = im.load()
    # BITMAPINFOHEADER。高度写两倍 —— ICO 的 DIB 把颜色位图和掩码位图叠在一起算
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in range(h - 1, -1, -1):          # BMP 自下而上
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a))      # BGRA
        rows.append(bytes(row))
    # AND 掩码：32 位色靠 alpha 通道决定透明度，掩码全 0（全不透明）即可。
    # 每行按 4 字节对齐
    mask_stride = ((w + 31) // 32) * 4
    mask = b"\x00" * (mask_stride * h)
    return hdr + b"".join(rows) + mask


def png_entry(im: Image.Image) -> bytes:
    import io
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_ico(path: Path, images: dict) -> None:
    entries = []
    for px in sorted(images):
        im = images[px]
        blob = png_entry(im) if px >= 128 else bmp_entry(im)
        entries.append((px, blob))

    out = struct.pack("<HHH", 0, 1, len(entries))       # ICONDIR
    offset = 6 + 16 * len(entries)
    dir_bytes, data_bytes = b"", b""
    for px, blob in entries:
        dim = 0 if px >= 256 else px                    # 256 在 1 字节里记作 0
        dir_bytes += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32,
                                 len(blob), offset)
        data_bytes += blob
        offset += len(blob)
    path.write_bytes(out + dir_bytes + data_bytes)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    assets = root / "assets"
    assets.mkdir(exist_ok=True)

    src_path = Path(sys.argv[1]) if len(sys.argv) > 1 \
        else assets / "source-artwork.png"
    if not src_path.is_file():
        print(f"找不到原画: {src_path}")
        return 1

    src = Image.open(src_path).convert("RGBA")
    print(f"原画 {src.size[0]}x{src.size[1]}")

    # ① Windows 可执行文件图标
    write_ico(assets / "icon.ico", {px: render(src, px) for px in ICO_SIZES})
    print(f"  assets/icon.ico            {ICO_SIZES}")

    # ② Qt 窗口/任务栏图标（走 qrc 编进 exe —— 不额外发文件，
    #    发布包的白名单门禁就不用为图标开口子）
    for px in QRC_SIZES:
        render(src, px).save(assets / f"logo_{px}.png", optimize=True)
    print(f"  assets/logo_NN.png         {QRC_SIZES}")

    # ③ README 展示图
    banner = src.crop((BANNER_LEFT_TRIM, 0, src.size[0], src.size[1]))
    bw, bh = banner.size
    banner = banner.resize((BANNER_WIDTH, round(bh * BANNER_WIDTH / bw)),
                           Image.LANCZOS)
    banner.convert("RGB").save(assets / "banner.jpg", quality=88, optimize=True)
    print(f"  assets/banner.jpg          {banner.size[0]}x{banner.size[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
