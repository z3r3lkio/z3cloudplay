#!/usr/bin/env python3
"""Generate original z3cloudplay branding assets reproducibly (Pillow >= 9).
Does not change SDK icon metadata or third-party assets.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
ROOT = Path(__file__).resolve().parents[1]

def font(size, bold=False):
    family = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    for base in ("/usr/share/fonts/truetype/dejavu", "/usr/local/share/fonts"):
        p = Path(base) / family
        if p.exists():
            return ImageFont.truetype(str(p), size)
    return ImageFont.load_default()

def asset(size):
    image = Image.new("RGBA", (size, size), (11, 18, 33, 255))
    d = ImageDraw.Draw(image)
    pad = size//25
    d.rounded_rectangle((pad,pad,size-pad,size-pad), radius=size//6,
                        fill=(16,29,51,255), outline=(40,205,238,255),width=max(2,size//105))
    # Cloud silhouette: an intentional custom brand glyph, not a copied logo.
    cyan=(54,220,242,255)
    y=size*0.35
    d.ellipse((size*.25,y-size*.055,size*.51,y+size*.17),fill=cyan)
    d.ellipse((size*.39,y-size*.12,size*.68,y+size*.18),fill=cyan)
    d.ellipse((size*.59,y-size*.025,size*.78,y+size*.17),fill=cyan)
    d.rounded_rectangle((size*.25,y+size*.055,size*.78,y+size*.17),radius=size*.065,fill=cyan)
    d.rounded_rectangle((size*.295,y+size*.072,size*.735,y+size*.155),radius=size*.03,fill=(16,29,51,255))
    title="Z3"
    big=font(round(size*.27),True)
    bb=d.textbbox((0,0),title,font=big)
    d.text(((size-(bb[2]-bb[0]))/2,size*.47),title,font=big,fill=(245,250,255,255))
    small=font(round(size*.064),True)
    caption="CLOUDPLAY"
    bb=d.textbbox((0,0),caption,font=small)
    d.text(((size-(bb[2]-bb[0]))/2,size*.815),caption,font=small,fill=(135,225,240,255))
    return image

for relative,size in [
    ("sce_sys/icon0.png",512),
    ("docs/images/icon.png",256),
    ("assets/branding/generic-title-icon.png",512)
]:
    target=ROOT/relative
    target.parent.mkdir(parents=True,exist_ok=True)
    asset(size).save(target,optimize=True)
    print(relative)
