#!/usr/bin/env python3
"""Render the launch film using artwork and music from a local Peggle copy.

Requires Python 3, Pillow, NumPy, and ffmpeg. No game assets are embedded here.
Example: python3 tools/render_launch_video.py '/path/to/Peggle Deluxe.app'
"""

import argparse
from functools import lru_cache
import json
import math
from pathlib import Path
import random
import shutil
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

from launch_game_assets import GameAssets, BallFlight


W, H = 1920, 1080
DURATION = 27.2
WHITE = (255, 244, 201)
MUTED = (205, 224, 240)
ORANGE = (255, 176, 36)
LILAC = (130, 206, 255)
GREEN = (109, 247, 165)
FONT = '/System/Library/Fonts/SFNS.ttf'
MONO = '/System/Library/Fonts/SFNSMono.ttf'
ASSETS = None


def clamp(x):
    return max(0.0, min(1.0, x))


def ease(x):
    x = clamp(x)
    return x * x * x * (x * (6 * x - 15) + 10)


def out(x):
    return 1 - (1 - clamp(x)) ** 3


def phase(t, start, end):
    return clamp((t - start) / (end - start))


def mix(a, b, p):
    return a + (b - a) * p


def blank():
    return Image.new('RGBA', (W, H))


def over(canvas, layer, opacity=1, xy=(0, 0)):
    if opacity <= 0:
        return
    if opacity < .999:
        layer = layer.copy()
        layer.putalpha(layer.getchannel('A').point(lambda x: int(x * opacity)))
    canvas.alpha_composite(layer, (int(xy[0]), int(xy[1])))


@lru_cache(maxsize=700)
def text_image(value, size, color=WHITE, weight=0, mono=False, ui=False):
    if not ui:
        display = size >= 50 or (weight in (0, 8) and size >= 24)
        return ASSETS.fonts['display' if display else 'body'].render(value, size, color, display)
    font = ImageFont.truetype(MONO if mono else FONT, int(size))
    if not mono:
        # SF's optical-size axis selects the system's display/text rendering.
        font.set_variation_by_axes([100, min(96, max(17, size)), 400, 700 if weight in (0,8) else 500 if weight==2 else 400])
    box = font.getbbox(value)
    im = Image.new('RGBA', (max(1, box[2] - box[0] + 4), box[3] - box[1] + 4))
    ImageDraw.Draw(im).text((2 - box[0], 2 - box[1]), value, font=font, fill=color)
    return im


def text(canvas, value, xy, size, color=WHITE, weight=0, opacity=1, anchor='left', mono=False, ui=False):
    im = text_image(value, size, color, weight, mono, ui)
    x, y = xy
    if anchor == 'center':
        x -= im.width / 2
    elif anchor == 'right':
        x -= im.width
    over(canvas, im, opacity, (x, y))


def ui_text(canvas, value, xy, size, color=(245,245,247), weight=0, opacity=1, anchor='left', mono=False):
    if color == WHITE: color=(245,245,247)
    if color == MUTED: color=(176,176,184)
    text(canvas,value,xy,size,color,weight,opacity,anchor,mono,ui=True)


def reveal(canvas, value, xy, size, t, start, color=WHITE, weight=0, anchor='left'):
    p = out(phase(t, start, start + .65))
    bounce = math.sin(phase(t,start,start+.85)*math.pi*2)*10*(1-phase(t,start,start+.85)) if size>=50 else 0
    text(canvas, value, (xy[0], xy[1] + 45 * (1 - p) + bounce), size, color, weight, p, anchor)


@lru_cache(maxsize=80)
def glow(size, color):
    yy, xx = np.mgrid[-1:1:complex(size), -1:1:complex(size)]
    a = np.maximum(0, 1 - np.sqrt(xx * xx + yy * yy)) ** 2.5
    pixels = np.zeros((size, size, 4), dtype=np.uint8)
    pixels[:, :, :3] = color
    pixels[:, :, 3] = (a * 170).astype(np.uint8)
    return Image.fromarray(pixels)


def halo(canvas, center, radius, color, opacity=1):
    size = int(radius * 2)
    over(canvas, glow(size, color), opacity, (center[0] - radius, center[1] - radius))


def peg(canvas, x, y, radius=18, color=ORANGE, opacity=1, lit=False):
    kind = 1 if color == ORANGE else 3 if color == GREEN else 2 if color[0] > color[2] * .8 else 0
    im = ASSETS.peg(round(radius*2), kind, lit)
    over(canvas, im, opacity, (x - im.width / 2, y - im.height / 2))


def additive(canvas, sprite, x, y, opacity=1, color=(255,255,255)):
    """The game's additive particle blend, cropped to the visible canvas."""
    left, top = round(x-sprite.width/2), round(y-sprite.height/2)
    box = (max(0,left),max(0,top),min(canvas.width,left+sprite.width),min(canvas.height,top+sprite.height))
    if box[0]>=box[2] or box[1]>=box[3] or opacity<=0:
        return
    sprite = sprite.crop((box[0]-left,box[1]-top,box[2]-left,box[3]-top)).convert('RGB')
    if color != (255,255,255): sprite=ImageChops.multiply(sprite,Image.new('RGB',sprite.size,color))
    if opacity<1: sprite=sprite.point(lambda x: round(x*opacity))
    target=canvas.crop(box).convert('RGB')
    canvas.paste(ImageChops.add(target,sprite).convert('RGBA'),box)


def polyline(canvas, points, color, width, progress=1):
    lengths = [math.dist(a, b) for a, b in zip(points, points[1:])]
    left = sum(lengths) * clamp(progress)
    draw = ImageDraw.Draw(canvas)
    for a, b, length in zip(points, points[1:], lengths):
        if left <= 0:
            break
        p = min(1, left / max(length, .001))
        end = (mix(a[0], b[0], p), mix(a[1], b[1], p))
        draw.line([a, end], fill=color, width=width)
        rr = width / 2
        for x, y in (a, end):
            draw.ellipse((x - rr, y - rr, x + rr, y + rr), fill=color)
        left -= length


def bezier(a, b, c, d, p):
    return tuple((1-p)**3*a[i] + 3*(1-p)**2*p*b[i] + 3*(1-p)*p*p*c[i] + p**3*d[i] for i in range(2))


def ring(canvas, center, radius, color, width=3, progress=1, start=-90):
    x, y = center
    if progress <= 0:
        return
    ImageDraw.Draw(canvas).arc((x-radius,y-radius,x+radius,y+radius), start, start+360*progress, fill=color, width=width)


def cursor(canvas, x, y, scale=1, opacity=1, pressed=False):
    # A graphic macOS arrow; drawn at 3x for clean small edges.
    im = Image.new('RGBA', (150, 180))
    d = ImageDraw.Draw(im)
    points = [(15,9),(15,112),(43,87),(64,139),(85,130),(63,81),(106,80)]
    d.polygon(points, fill=(255,255,255), outline=(20,17,31), width=6)
    im = im.resize((int(50*scale),int(60*scale)),Image.Resampling.LANCZOS)
    over(canvas, im, opacity, (x-5*scale,y-3*scale))
    if pressed:
        ring(canvas, (x,y), 25, (255,255,255,140), 2)


class Film:
    def __init__(self, resources):
        global ASSETS
        ASSETS = self.assets = GameAssets(resources)
        text_image.cache_clear()
        self.flight = BallFlight()
        self.icon = Image.open(resources / 'Peggle.icns').convert('RGBA')
        rng = random.Random(418)
        self.stars = [(rng.randrange(W), rng.randrange(H), rng.random()) for _ in range(90)]
        self.confetti = [(rng.uniform(-1,1),rng.uniform(-1,1),rng.uniform(.4,1.1),rng.random()) for _ in range(75)]
        yy, xx = np.mgrid[0:H,0:W]
        pixels = np.zeros((H,W,4),dtype=np.uint8)
        channels = [np.full((H,W),v,dtype=np.float32) for v in (9,19,45)]
        for cx, cy, rad, color in [(1590,270,760,(14,72,104)),(130,890,630,(38,23,72)),(920,1080,950,(8,36,38))]:
            fall = np.exp(-((xx-cx)**2+(yy-cy)**2)/(rad*rad))
            for i in range(3): channels[i] += fall*color[i]
        noise = np.random.default_rng(418).normal(0,.6,(H,W))
        for i in range(3): pixels[:,:,i] = np.clip(channels[i]+noise,0,255)
        pixels[:,:,3] = 255
        self.background = Image.fromarray(pixels)
        self.panel = self.make_panel()
        self.icon_cache = {}

    def icon_at(self, canvas, x, y, size, opacity=1, tilt=0):
        key = (int(size), round(tilt,1))
        if key not in self.icon_cache:
            im = self.icon.resize((int(size),int(size)),Image.Resampling.LANCZOS)
            if tilt:
                im = im.rotate(tilt,Image.Resampling.BICUBIC,expand=True)
            self.icon_cache[key] = im
        im = self.icon_cache[key]
        over(canvas, im, opacity, (x-im.width/2,y-im.height/2))

    def base(self,t):
        return self.background.copy()

    def scene_intro(self,t):
        im=self.base(t)
        hits={index:when for when,index in self.flight.events if when<=t}
        for i,(x,y) in enumerate(self.flight.pegs):
            kind=self.flight.kinds[i]
            if i in hits and kind != 0:
                sprite=self.assets.peg_glow.resize((144,144),Image.Resampling.LANCZOS)
                additive(im,sprite,x,y,.5,(255,136,0))
            sprite=self.assets.peg(54,kind,i in hits)
            over(im,sprite,1,(x-27,y-27))
        for index,when in hits.items():
            age=t-when; kind=self.flight.kinds[index]
            # Original sprite strips: 2 game ticks per cel, at 100Hz.
            cel=int(age*100*self.flight.time_scale/2)
            pulse=self.assets.pulse(kind,cel,3)
            if pulse is not None:
                additive(im,pulse,*self.flight.pegs[index],.8)
        bx,by=self.flight.at(t)
        # The original ball art includes a one-pixel antialias margin.
        ball=self.assets.ball_at_size(42)
        over(im,ball,1,(bx-21,by-21))
        reveal(im,'One more',(111,240),93,t,.05,WHITE,8)
        reveal(im,'shot!',(95,362),223,t,.25,ORANGE,8)
        reveal(im,'That feeling never gets old.',(130,639),34,t,.75,MUTED,5)
        return im

    def scene_brand(self,t):
        im=self.base(t); p=out(phase(t,3.5,4.35))
        halo(im,(960,430),390,(139,93,255),.8)
        for i in range(18):
            a=i*math.tau/18+t*.16
            r=320+(1-p)*120
            peg(im,960+math.cos(a)*r,410+math.sin(a)*r*.67,17,ORANGE if i%3==0 else (70,137,245),.92)
        self.icon_at(im,960,344,170+25*p,p, -8*(1-p))
        reveal(im,'PeggleSilicon',(960,532),147,t,3.75,ORANGE,8,'center')
        reveal(im,'Run Peggle Deluxe on Apple silicon.',(960,727),35,t,4.05,MUTED,5,'center')
        return im

    @staticmethod
    def make_panel():
        im=Image.new('RGBA',(1000,800))
        shadow=Image.new('RGBA',im.size)
        ImageDraw.Draw(shadow).rounded_rectangle((30,30,970,752),26,fill=(0,0,0,130))
        im.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14)))
        d=ImageDraw.Draw(im)
        d.rounded_rectangle((16,8,984,730),24,fill=(31,29,42),outline=(115,105,144,190),width=2)
        d.rounded_rectangle((17,9,983,64),23,fill=(43,40,55))
        d.rectangle((17,38,983,64),fill=(43,40,55))
        d.line((17,64,983,64),fill=(81,75,99),width=1)
        for i,c in enumerate([(250,99,93),(249,192,78),(78,194,103)]):
            d.ellipse((40+i*28,27,55+i*28,42),fill=c)
        ui_text(im,'Install PeggleSilicon',(500,25),20,(210,204,224),5,anchor='center')
        ui_text(im,'PeggleSilicon Installer',(60,103),39,WHITE,0)
        ui_text(im,'Run Peggle Deluxe on Apple silicon',(61,164),25,MUTED,5)
        d.rounded_rectangle((58,224,942,474),16,fill=(40,37,53))
        for x in range(79,920,22):
            d.line((x,224,x+11,224),fill=(104,97,122),width=2)
            d.line((x,474,x+11,474),fill=(104,97,122),width=2)
        for y in range(246,450,22):
            d.line((58,y,58,y+11),fill=(104,97,122),width=2)
            d.line((942,y,942,y+11),fill=(104,97,122),width=2)
        ui_text(im,'Export location',(60,522),24,(208,202,222),5)
        ui_text(im,'/Applications',(295,522),24,MUTED,5)
        d.rounded_rectangle((780,510,941,554),9,fill=(71,65,89),outline=(92,84,110),width=1)
        ui_text(im,'Choose…',(860,520),22,WHITE,5,anchor='center')
        ui_text(im,'The app will be saved as PeggleSilicon.app.',(60,575),20,MUTED,5)
        return im

    def scene_demo(self,t):
        im=self.base(t); entrance=out(phase(t,6.15,6.9))
        reveal(im,'DRAG. DROP. PLAY.',(125,209),22,t,6.25,LILAC,2)
        reveal(im,'Your game.',(116,270),82,t,6.32,WHITE,8)
        reveal(im,'Your Mac.',(116,370),82,t,6.43,WHITE,8)
        reveal(im,'One little drop.',(125,491),30,t,6.65,MUTED,5)
        reveal(im,'A whole lot of joy.',(125,534),30,t,6.75,MUTED,5)
        panel=self.panel.copy(); d=ImageDraw.Draw(panel)
        dropped=t>=10.45
        installing=t>=12.2
        success=t>=15.05
        hovering=9.85<t<10.45
        if hovering:
            d.rounded_rectangle((58,224,942,474),16,fill=(50,43,75),outline=LILAC,width=3)
        if success:
            # Same circle and drawn checkmark as the installer, at demo scale.
            sp=out(phase(t,15.05,15.4))
            halo(panel,(500,314),110,(90,237,150),.32*sp)
            ring(panel,(500,311),43,GREEN,5,sp)
            polyline(panel,[(480,313),(494,327),(522,297)],GREEN,6,out(phase(t,15.18,15.87)))
            ui_text(panel,'Installed PeggleSilicon successfully!',(500,386),29,WHITE,0,sp,'center')
        elif dropped:
            self.icon_at(panel,500,306,74)
            ui_text(panel,'Peggle Deluxe.app',(500,370),29,WHITE,0,anchor='center')
            ui_text(panel,'Ready to install',(500,418),21,MUTED,5,anchor='center')
        else:
            polyline(panel,[(500,272),(500,322)],LILAC,4)
            polyline(panel,[(483,306),(500,323),(517,306)],LILAC,4)
            ui_text(panel,'Drag Peggle Deluxe.app here',(500,363),29,WHITE,0,anchor='center')
            ui_text(panel,'The original Peggle Deluxe 1.0.5 application is required.',(500,415),20,MUTED,5,anchor='center')
        if installing and not success:
            progress=ease(phase(t,12.2,15.05))
            d.rounded_rectangle((60,642,375,649),3,fill=(71,64,88))
            if progress>.01: d.rounded_rectangle((60,642,60+315*progress,649),3,fill=LILAC)
            ui_text(panel,'Building PeggleSilicon…',(60,668),19,MUTED,5)
        else:
            ui_text(panel,'Installation complete.' if success else 'Ready to install.' if dropped else 'Drop the game app to begin.',(60,655),20,MUTED,5)
        button=(118,92,218) if dropped else (67,59,92)
        d.rounded_rectangle((582,630,941,691),12,fill=button)
        ui_text(panel,'Install PeggleSilicon',(760,649),24,WHITE if dropped else MUTED,0,anchor='center')
        px,py=785+80*(1-entrance),171
        over(im,panel,entrance,(px,py))
        # Source app lifts out of its card and follows the pointer into the drop zone.
        source_x,source_y=365,712
        if t<10.45:
            move=ease(phase(t,8.6,10.45))
            ix,iy=bezier((source_x,source_y),(510,490),(1060,395),(1285,477),move)
            fade=1-out(phase(t,10.32,10.49))
            halo(im,(ix,iy+28),135,(145,96,248),.52*entrance*fade)
            self.icon_at(im,ix,iy,mix(152,116,move),entrance*fade, -8*math.sin(move*math.pi))
            if move<.03:
                ui_text(im,'Peggle Deluxe.app',(source_x,811),24,WHITE,5,entrance,'center')
            if t>=8.0:
                cp=out(phase(t,8.0,8.55))
                cx,cy=(mix(538,source_x+47,cp),mix(833,source_y+42,cp)) if t<8.6 else (ix+47,iy+42)
                cursor(im,cx,cy,1.12,out(phase(t,8.0,8.2)),8.50<t<8.66)
        if 10.45<=t<12.5:
            cp=ease(phase(t,10.8,11.88))
            cx,cy=bezier((1332,519),(1450,528),(1520,717),(1595,834),cp)
            cursor(im,cx,cy,1.12,1-out(phase(t,12.22,12.5)),12.13<t<12.3)
        if 10.35<t<11.03:
            q=phase(t,10.35,11.03)
            ring(im,(1285,486),52+q*100,(*LILAC,int((1-q)*190)),3)
        if 12.13<t<12.7:
            q=phase(t,12.13,12.7)
            ring(im,(1595,834),16+q*65,(*LILAC,int((1-q)*220)),3)
        if t>=10.45:
            n=out(phase(t,10.5,11.2))
            text(im,'01',(130,690),19,LILAC,5,n,mono=True)
            text(im,'Drop your app',(186,684),29,WHITE,2,n)
            n=out(phase(t,12.2,12.8))
            text(im,'02',(130,752),19,LILAC,5,n,mono=True)
            text(im,'Install PeggleSilicon',(186,746),29,WHITE,2,n)
            n=out(phase(t,15.05,15.65))
            text(im,'03',(130,814),19,GREEN,5,n,mono=True)
            text(im,'Take another shot',(186,808),29,WHITE,2,n)
        ui_text(im,'Illustrated installer • timing condensed',(1828,958),17,MUTED,5,entrance,'right')
        return im

    def scene_celebrate(self,t):
        im=self.base(t); q=t-16.55
        for vx,vy,s,seed in self.confetti:
            travel=out(phase(t,16.6,19.0))
            x=960+vx*980*travel
            y=530+vy*620*travel+q*q*13
            opacity=(1-clamp((q-1.0)/2.9))*.85
            color=ORANGE if seed<.45 else LILAC if seed<.8 else GREEN
            if seed<.7:
                peg(im,x,y,int(5+12*s),color,opacity)
            else:
                cel=int(q*27.5+seed*14)%14
                additive(im,self.assets.sparkle(cel,int(30+40*s)),x,y,opacity)
        halo(im,(960,450),380,(146,92,245),.55)
        # A green check turns into the signature orange ball's orbit.
        ring(im,(960,301),63,GREEN,6,out(phase(t,16.65,17.12)))
        polyline(im,[(930,302),(953,325),(995,279)],GREEN,8,out(phase(t,16.85,17.5)))
        reveal(im,'Oh, the joy!',(960,449),168,t,16.85,ORANGE,8,'center')
        reveal(im,'Peggle Deluxe. Back on your Mac.',(960,661),37,t,17.12,MUTED,5,'center')
        p=out(phase(t,17.75,18.4))
        left=text_image('Same pegs.',30,ORANGE,0)
        right=text_image('New possibilities.',30,LILAC,0)
        combined_width=left.width+30+right.width
        start=(W-combined_width)/2
        over(im,left,p,(start,825))
        over(im,right,p,(start+left.width+30,825))
        return im

    def scene_outro(self,t):
        im=self.base(t); p=out(phase(t,20.25,21.1))
        halo(im,(960,386),350,(138,99,245),.63)
        for i in range(14):
            a=i*math.tau/14+t*.12
            radius=360+18*math.sin(t*.3+i)
            peg(im,960+math.cos(a)*radius,423+math.sin(a)*radius*.66,15,ORANGE if i%3==0 else (60,140,245),.9*p)
        self.icon_at(im,960,318,157,p)
        reveal(im,'PeggleSilicon',(960,476),152,t,20.45,ORANGE,8,'center')
        reveal(im,'Run Peggle Deluxe on Apple silicon',(960,670),34,t,20.7,MUTED,5,'center')
        cta=out(phase(t,21.1,21.9))
        text(im,'Take another shot.',(960,789),47,WHITE,0,cta,'center')
        ui_text(im,'github.com/JacksonDam/PeggleSilicon',(960,912),24,WHITE,5,out(phase(t,21.45,22.15)),'center')
        # A few original fever sparkle cels finish the musical phrase.
        for i in range(5):
            q=t-23.1-i*.16
            if 0<q<.65:
                cel=min(13,int(q*22))
                additive(im,self.assets.sparkle(cel,46),742+i*108,979,.8)
        return im

    def sharp_frame(self,t):
        im=self.base(t)
        scenes=[(0,3.75,self.scene_intro),(3.4,6.5,self.scene_brand),(6.05,17.05,self.scene_demo),(16.55,20.7,self.scene_celebrate),(20.25,DURATION,self.scene_outro)]
        for start,end,draw in scenes:
            if start<=t<end:
                a=1 if start==0 else out(phase(t,start,start+.38))
                if end<DURATION: a*=1-ease(phase(t,end-.4,end))
                over(im,draw(t),a)
        # Half-second bookends keep the music and picture ending together.
        fade=min(out(phase(t,0,.22)),1-ease(phase(t,26.45,DURATION)))
        if fade<1: im=Image.blend(Image.new('RGBA',(W,H),(9,8,20,255)),im,fade)
        return im.convert('RGB')

    def frame(self,t, samples=4):
        # Four temporal samples across a 180-degree shutter at 60 fps.
        # Stationary UI stays sharp; only moving sprites and type blur.
        shutter=1/120
        result=None
        for i in range(samples):
            sample=max(0,min(DURATION-1e-6,t+(i/(samples-1)-.5)*shutter)) if samples>1 else t
            image=self.sharp_frame(sample)
            result=image if result is None else Image.blend(result,image,1/(i+1))
        return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game',type=Path,help='Local original Peggle Deluxe.app')
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parent.parent/'launch-video/PeggleSilicon-Launch.mp4')
    parser.add_argument('--fps',type=int,default=60)
    parser.add_argument('--stills-only',action='store_true')
    args=parser.parse_args()
    if shutil.which('ffmpeg') is None: parser.error('ffmpeg must be installed')
    resources=args.game.expanduser()/'Contents/Resources'
    audio=resources/'music/odetojoy.ogg'
    if not audio.is_file(): parser.error(f'Game music is missing: {audio}')
    output=args.output.expanduser().resolve(); output.parent.mkdir(parents=True,exist_ok=True)
    film=Film(resources)
    report=film.flight.report()
    if report['minimum_clearance_px'] < -.01:
        raise RuntimeError('Ball simulation intersects a peg')
    (output.parent/'physics-check.json').write_text(json.dumps(report,indent=2))
    moments=[2.0,4.8,7.8,9.65,11.3,13.8,15.95,18.2,23.0]
    thumbs=[]
    for t in moments:
        frame=film.frame(t)
        frame.save(output.parent/f'frame-{t:05.2f}.jpg',quality=94)
        thumb=frame.resize((640,360),Image.Resampling.LANCZOS)
        ImageDraw.Draw(thumb).text((12,338),f'{t:.2f}s',fill='white')
        thumbs.append(thumb)
    board=Image.new('RGB',(1920,1080))
    for i,thumb in enumerate(thumbs): board.paste(thumb,((i%3)*640,(i//3)*360))
    board.save(output.parent/'storyboard.jpg',quality=94)
    film.frame(23.0).save(output.parent/'poster.jpg',quality=95)
    if args.stills_only:
        print(output.parent/'storyboard.jpg'); return
    with tempfile.TemporaryDirectory(prefix='pegglesilicon-film-') as temporary:
        silent=Path(temporary)/'picture.mp4'
        encoder=subprocess.Popen([
            'ffmpeg','-hide_banner','-loglevel','error','-y','-f','rawvideo','-pix_fmt','rgb24',
            '-s',f'{W}x{H}','-r',str(args.fps),'-i','pipe:0','-an',
            '-c:v','libx264','-preset','fast','-crf','18','-pix_fmt','yuv420p',
            '-color_primaries','bt709','-color_trc','bt709','-colorspace','bt709',
            '-movflags','+faststart',str(silent)],stdin=subprocess.PIPE)
        total=round(DURATION*args.fps)
        try:
            for n in range(total):
                encoder.stdin.write(film.frame(n/args.fps).tobytes())
                if n%(args.fps*2)==0: print(f'Rendering {n/args.fps:4.1f}s / {DURATION:.1f}s',flush=True)
        finally:
            encoder.stdin.close()
        if encoder.wait()!=0: raise SystemExit('Video encoder failed')
        # Music and subtle collision sounds both come from the user's game.
        peg_hit=Path(temporary)/'peghit.ogg'
        peg_hit.write_bytes(film.assets.files['sounds/peghit.ogg'])
        events=[(t,i) for t,i in film.flight.events if t<3.4]
        filters=['[1:a]volume=1.4,afade=t=in:d=0.08,afade=t=out:st=26.45:d=0.72,apad[music]']
        filters.append('[2:a]asplit='+str(len(events))+''.join(f'[h{i}]' for i in range(len(events))))
        for i,(t,_) in enumerate(events):
            rate=round(44100*(1+i*.025))
            filters.append(f'[h{i}]asetrate={rate},aresample=44100,volume=0.3,adelay={round(t*1000)}:all=1[s{i}]')
        filters.append('[music]'+''.join(f'[s{i}]' for i in range(len(events)))+
                       f'amix=inputs={len(events)+1}:duration=first:normalize=0[audio]')
        subprocess.run([
            'ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(silent),'-i',str(audio),'-i',str(peg_hit),
            '-filter_complex',';'.join(filters),'-map','0:v:0','-map','[audio]',
            '-c:v','copy','-c:a','aac','-b:a','256k',
            '-t',str(DURATION),'-movflags','+faststart',
            '-metadata','title=PeggleSilicon — One more shot',str(output)],check=True)
    print(output)
    print(json.dumps({'duration':DURATION,'width':W,'height':H,'fps':args.fps}))


if __name__=='__main__': main()
