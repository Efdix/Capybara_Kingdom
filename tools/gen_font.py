"""
Chinese pixel font generator.

The NDS text console only has ASCII, and BlocksDS ships no CJK font, so the
game draws its own 4bpp text layer. We build a small atlas: render each needed
glyph from a Windows CJK font at 12px, threshold it to 1bpp, and pack it into
16x16 cells (glyphs are <=12x12, centered).

Output:
  font_glyphs.bin  -- 1bpp tiles, 16x16 px = 32 bytes each (4 NDS tiles of 8x8
                      arranged 2x2, which is exactly how a 4bpp 16x16 sprite
                      tile block is laid out), row-major tile order
  font_map.txt     -- one character per line, in atlas order (index = line no.)
  font_preview.png -- visual check

Text drawn with this atlas uses 4bpp OBJ sprites on the sub screen.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

CELL = 16          # cell size in pixels
GLYPH_PX = 12      # font render size
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",     # SimHei -- clean and legible at 12px
    r"C:\Windows\Fonts\msyh.ttc",       # Microsoft YaHei
    r"C:\Windows\Fonts\simsun.ttc",     # SimSun
    r"C:\Windows\Fonts\Deng.ttf",
]

# All characters the game displays. Keep this list tight: each glyph costs
# 32 bytes and a sprite slot. ASCII digits/latin are included for numbers and
# level labels.
CHARS = (
    "水豚王国生日快乐蛋糕勇者公母森林沙漠雪地火山魔王城"
    "材料面粉鸡蛋牛奶糖巧克力草莓蜂蜜果酱坚果"
    "攻击魔法技能升级经验生命值升级点确定取消返回"
    "开始继续暂停存档读档退出游戏设置音量单人双人"
    "联机创建加入房间连接等待玩家准备好了吗请按"
    "恭喜通关完成了烤好了真棒厉害加油谢谢你们一起"
    "打败了获得了收集齐五个去找吧出发吧前进后退"
    "新纪录等级金钱背包使用装备学习解锁选择左右"
    "上下确认键触摸屏幕滑动剑冲撞横扫旋风狂暴火球"
    "冰锥落雷治愈术恢复伤害暴击闪避防御速度力量"
    "智力幸运商店买药水红蓝绿心星星金币宝物钥匙"
    "门打开关闭宝箱怪物史莱姆黄蜂蝎子雪球火灵仙人掌"
    "熔岩球蜻蜓树精沙虫冰狼熔岩巨人蛋糕魔王公主国王"
    "生日许愿蜡烛烟火礼物祝福永远幸福健康平安爱"
    "你们我们他们她他是我的一二三四五六七八九十"
    "零个只次回合波boss胜利失败逃跑等待中连接中"
    "没有无法不足满了再来一次真的吗当然了是的不是"
    "什么为什么怎么办怎么这里那里现在刚才以后之前"
    "小心危险注意帮助提示说明操作按键方向键ABXYLR"
    "START SELECT 年月日时分秒第关卡地图返回主菜单"
    "雨林树蛙飞蜂软泥巨猿树精长老农场田鼠乌鸦泥怪野猪硕鼠大王"
    "牧场野犬黑羊牛虻夜狼领袖狂暴黑牛沼泽淤泥毒蟒泥蟾凯门鳄王"
    "泥沼巨蟾火山焦糖火灵蝠翼熔岩火球焦糖熔岩巨兽双先锋霸主"
)

# Latin + digits + punctuation (kept in their own range for cleanliness)
ASCII = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
         "abcdefghijklmnopqrstuvwxyz"
         "0123456789"
         " .,:;!?'\"()[]-+/*%#<>=_&@$")

# Chinese punctuation, typographic marks, and the arrow set the menus use for
# direction hints. The vertical pair is here because a hint that offers
# "up / down" while the atlas can only draw "up" has to fall back to words.
CJK_PUNCT = "，。！？、：；“”‘’（）《》…—～·↑↓←→▶◀"


def render_glyph(ch, font):
    """Render one glyph into a CELL x CELL 1bpp bitmap (list of rows of bits)."""
    img = Image.new("L", (CELL, CELL), 0)
    d = ImageDraw.Draw(img)
    try:
        bbox = d.textbbox((0, 0), ch, font=font)
    except Exception:
        bbox = (0, 0, GLYPH_PX, GLYPH_PX)
    gw = bbox[2] - bbox[0]
    gh = bbox[3] - bbox[1]
    ox = (CELL - gw) // 2 - bbox[0]
    oy = (CELL - gh) // 2 - bbox[1]
    d.text((ox, oy), ch, fill=255, font=font)

    # Threshold. Use a mid threshold; at 12px antialiasing is minimal anyway.
    px = img.load()
    rows = []
    for y in range(CELL):
        row = 0
        any_on = False
        for x in range(CELL):
            if px[x, y] >= 100:
                row |= (1 << (15 - x))
                any_on = True
        rows.append((row, any_on))
    return rows


def pack_16x16_4bpp(rows):
    """Pack a 16x16 1bpp glyph into NDS 4bpp 16x16 sprite tile order.

    NDS 4bpp tiles are 8x8, 32 bytes each, 4 bits per pixel. A 16x16 sprite
    consists of 4 such tiles in this order: top-left, top-right, bottom-left,
    bottom-right. Within a tile, byte i holds pixel (2i) in the low nibble and
    pixel (2i+1) in the high nibble.
    """
    out = bytearray(128)
    for tile_y in (0, 1):
        for tile_x in (0, 1):
            tile_index = tile_y * 2 + tile_x
            base = tile_index * 32
            for y in range(8):
                for bx in range(4):
                    lo = 0
                    hi = 0
                    for half in (0, 1):
                        px_x = tile_x * 8 + bx * 2 + half
                        px_y = tile_y * 8 + y
                        bit = (rows[px_y][0] >> (15 - px_x)) & 1
                        # index 1 = opaque white (we draw in palette color 1)
                        v = 1 if bit else 0
                        if half == 0:
                            lo = v
                        else:
                            hi = v
                    out[base + y * 4 + bx] = (hi << 4) | lo
    return bytes(out)


def scan_sources_for_chars():
    """Collect every non-ASCII character that appears in the game sources.

    The atlas must cover exactly what the script displays; scanning the C
    sources means a newly typed line of dialogue can never render as a blank
    box. This is the authoritative source of characters -- CHARS below is only
    a safety net for text added later in a different file.
    """
    found = set()
    src_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                           "source")
    exts = (".c", ".h")
    for root, _dirs, files in os.walk(src_dir):
        for name in files:
            if not name.endswith(exts):
                continue
            path = os.path.join(root, name)
            try:
                with open(path, encoding="utf-8") as f:
                    text = f.read()
            except (OSError, UnicodeDecodeError):
                continue
            for ch in text:
                # Chinese characters plus CJK punctuation.
                if ord(ch) > 0x2000:
                    found.add(ch)
    return found


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "graphics"
    os.makedirs(out, exist_ok=True)

    font_path = None
    for cand in FONT_CANDIDATES:
        if os.path.exists(cand):
            font_path = cand
            break
    if not font_path:
        print("ERROR: no CJK font found", file=sys.stderr)
        sys.exit(1)
    print("using font:", font_path)

    # SimHei renders small CJK far better than the others at 12px
    font = None
    for size in (12, 13, 11):
        try:
            font = ImageFont.truetype(font_path, size)
            break
        except Exception:
            continue
    if font is None:
        font = ImageFont.load_default()

    # Deduplicate while preserving order
    # Characters actually used by the game sources come first so the atlas is
    # guaranteed to cover the script, then the broader safety-net set.
    scanned = scan_sources_for_chars()
    print(f"scanned {len(scanned)} CJK chars from game sources")

    seen = set()
    chars = []
    for ch in sorted(scanned) + list(CHARS) + list(CJK_PUNCT) + list(ASCII):
        if ch in seen:
            continue
        seen.add(ch)
        chars.append(ch)

    blob = bytearray()
    preview_cells = []
    empty = []
    for ch in chars:
        rows = render_glyph(ch, font)
        if not any(r[1] for r in rows):
            empty.append(ch)
        blob += pack_16x16_4bpp(rows)
        preview_cells.append(rows)

    gc_out = os.path.join(out, "font_glyphs.bin")
    with open(gc_out, "wb") as f:
        f.write(bytes(blob))

    with open(os.path.join(out, "font_map.txt"), "w", encoding="utf-8") as f:
        for ch in chars:
            f.write(ch + "\n")

    # Emit the glyph order straight into a C header so the game never has to
    # parse a file at runtime: atlas index N corresponds to character N of
    # this string.
    #
    # Resolved against the project root rather than the caller's cwd, so both
    # the Makefile and a bare `python tools/gen_font.py` land it in source/.
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hdr = os.path.join(root, "source", "font_chars.h")
    with open(hdr, "w", encoding="utf-8") as f:
        f.write("// Autogenerated by tools/gen_font.py -- do not edit.\n")
        f.write("// Atlas order: glyph N of font_glyphs.bin is FONT_CHARS[N].\n\n")
        f.write("#pragma once\n\n")
        f.write(f"#define FONT_GLYPH_COUNT {len(chars)}\n\n")
        f.write("static const char *const FONT_CHARS =\n")
        # wrap the literal at a sane line length for readability
        line = '    "'
        for ch in chars:
            esc = ch
            if ch == '"':
                esc = '\\"'
            elif ch == "\\":
                esc = "\\\\"
            if len(line.encode("utf-8")) > 150:
                f.write(line + '"\n')
                line = '    "'
            line += esc
        f.write(line + '";\n')
    print("char map header ->", hdr)

    print(f"glyphs: {len(chars)}  bytes: {len(blob)}  "
          f"({len(blob)//max(len(chars),1)} B each)")
    if empty:
        print("WARNING blank glyphs:", "".join(empty))

    # preview: first 40 glyphs, scaled
    from PIL import Image as I
    n = min(40, len(preview_cells))
    scale = 5
    sheet = I.new("RGB", (n * CELL * scale, CELL * scale + 12), (30, 26, 40))
    d = ImageDraw.Draw(sheet)
    for i in range(n):
        cell = I.new("L", (CELL, CELL), 0)
        cell.putdata([255 if (preview_cells[i][y][0] >> (15 - x)) & 1 else 0
                      for y in range(CELL) for x in range(CELL)])
        cell = cell.convert("RGB").resize((CELL * scale, CELL * scale),
                                          I.NEAREST)
        sheet.paste(cell, (i * CELL * scale, 0))
        d.text((i * CELL * scale + 2, CELL * scale + 1), chars[i],
               fill=(255, 240, 200))
    sheet.save(os.path.join(out, "preview_font.png"))
    print("preview ->", os.path.join(out, "preview_font.png"))


if __name__ == "__main__":
    main()
