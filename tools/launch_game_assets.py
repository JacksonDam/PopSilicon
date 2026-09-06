"""Game artwork, bitmap type, and collision simulation for the launch film.

Assets are read in memory from the user's main.pak. Collision constants and
sprite ordering were checked against the local PeggleDeluxe-mac transcription:
Ball::DoBallPegCollision, Ball::Update, PegInfo::DrawBall/DrawBigPulse.
"""

import ast
from functools import lru_cache
import io
import math
import re
import struct

import numpy as np
from PIL import Image, ImageChops, ImageFilter


class GameAssets:
    def __init__(self, resources):
        packed = (resources / 'main.pak').read_bytes()
        data = packed.translate(bytes.maketrans(bytes(range(256)), bytes(x ^ 247 for x in range(256))))
        if data[:4] != bytes.fromhex('c04ac0ba'):
            raise ValueError('Invalid Peggle PAK archive')
        offset = 8
        entries = []
        while offset < len(data) and data[offset] != 128:
            offset += 1
            length = data[offset]
            offset += 1
            name = data[offset:offset + length].decode().replace('\\', '/').lower()
            offset += length
            size = struct.unpack_from('<I', data, offset)[0]
            offset += 12
            entries.append((name, size))
        offset += 1
        self.files = {}
        for name, size in entries:
            if offset + size > len(data):
                raise ValueError('Truncated Peggle PAK archive')
            self.files[name] = data[offset:offset + size]
            offset += size
        self.pegs = self.image('images/game/ballpeg')
        self.ball = self.image('images/game/ball')
        self.sparkles = self.image('images/game/sparkle_medium', alpha=False)
        self.peg_glow = self.image('images/game/pegglow', alpha=False)
        self.pulses = [self.image('images/game/pulse_' + name, alpha=False)
                       for name in ('normal', 'goal', 'score', 'powerup')]
        self.fonts = {
            'display': BitmapFont(self, 'Overload24'),
            'body': BitmapFont(self, 'NeoSansMedium14'),
        }

    def image(self, stem, alpha=True):
        stem = stem.lower()
        for ext in ('.png', '.jpg', '.gif'):
            if stem + ext in self.files:
                result = Image.open(io.BytesIO(self.files[stem + ext])).convert('RGBA')
                break
        else:
            raise ValueError('Missing game image: ' + stem)
        if alpha:
            parent, _, filename = stem.rpartition('/')
            for maskstem in (stem + '_', parent + '/_' + filename):
                for ext in ('.png', '.gif', '.jpg'):
                    if maskstem + ext in self.files:
                        mask = Image.open(io.BytesIO(self.files[maskstem + ext])).convert('L')
                        if mask.size == result.size:
                            result.putalpha(mask)
                            return result
        return result

    @lru_cache(maxsize=1024)
    def peg(self, diameter, kind=0, lit=False):
        cel = kind + (4 if lit else 0)
        # The game crops one pixel off each edge for a static peg.
        image = self.pegs.crop((1, cel * 20 + 1, 19, cel * 20 + 19))
        return image.resize((diameter, diameter), Image.Resampling.LANCZOS)

    @lru_cache(maxsize=50)
    def ball_at_size(self, diameter):
        return self.ball.resize((diameter, diameter), Image.Resampling.LANCZOS)

    @lru_cache(maxsize=512)
    def pulse(self, kind, cel, scale=2):
        sheet = self.pulses[kind]
        if cel >= sheet.height // sheet.width:
            return None
        image = sheet.crop((0, cel * sheet.width, sheet.width, (cel + 1) * sheet.width))
        return image.resize((round(sheet.width * scale), round(sheet.width * scale)), Image.Resampling.LANCZOS)

    @lru_cache(maxsize=100)
    def sparkle(self, cel, size):
        return self.sparkles.crop((cel * 20, 0, cel * 20 + 20, 20)).resize((size, size), Image.Resampling.LANCZOS)


class BitmapFont:
    def __init__(self, assets, name):
        desc = assets.files[f'fonts/{name.lower()}.txt'].decode('latin1')

        def definition(key):
            match = re.search(r'Define\s+' + key + r'\s+(\(.*?\));', desc, re.S)
            return ast.literal_eval(match[1]) if match else ()

        chars = definition('CharList')
        self.widths = dict(zip(chars, definition('WidthList')))
        self.rects = dict(zip(chars, definition('RectList')))
        self.offsets = dict(zip(chars, definition('OffsetList')))
        self.kern = dict(zip(definition('KerningPairs'), definition('KerningValues')))
        self.point_size = int(re.search(r'SetDefaultPointSize\s+(\d+)', desc)[1])
        self.space = int(re.search(r"LayerSetCharWidths\s+Main\s+\(' '\)\s+\((\d+)\)", desc)[1])
        for extension in ('.png', '.gif'):
            key = f'fonts/_{name.lower()}{extension}'
            if key in assets.files:
                self.mask = Image.open(io.BytesIO(assets.files[key])).convert('L')
                break
        else:
            raise ValueError('Missing font atlas: ' + name)

    @lru_cache(maxsize=500)
    def render(self, text, size, color, display=False):
        text = text.replace('…', '...').replace('•', '-').replace('’', "'")
        width = 24 + sum(self.widths.get(ch, self.space) for ch in text)
        mask = Image.new('L', (width, self.mask.height + 24))
        x = 12
        previous = ''
        for ch in text:
            x += self.kern.get(previous + ch, 0)
            if ch in self.rects:
                gx, gy, gw, gh = self.rects[ch]
                dx, dy = self.offsets[ch]
                glyph = self.mask.crop((gx, gy, gx + gw, gy + gh))
                box = (x + dx, 12 + dy)
                # Lighter preserves overlapping kerned glyphs.
                target = mask.crop((box[0], box[1], box[0] + gw, box[1] + gh))
                mask.paste(ImageChops.lighter(target, glyph), box)
            x += self.widths.get(ch, self.space)
            previous = ch
        box = mask.getbbox()
        if box is None:
            return Image.new('RGBA', (max(1, x), 2))
        mask = mask.crop(box)
        ratio = size / self.point_size * (1 if display else .68)
        mask = mask.resize((max(1, round(mask.width * ratio)), max(1, round(mask.height * ratio))), Image.Resampling.LANCZOS)
        if ratio > 1.5:
            # Smooth the binary atlas's enlarged edges without replacing glyphs.
            mask = mask.filter(ImageFilter.GaussianBlur(ratio * .25))
            mask = mask.point(lambda x: round(255 * max(0, min(1, (x - 40) / 175))))
        if display:
            # The game's font2 treatment: dark outline and warm bevel shading.
            pad = max(3, round(size * .043))
            expanded = Image.new('L', (mask.width + pad * 4, mask.height + pad * 4))
            expanded.paste(mask, (pad * 2, pad * 2))
            outline = expanded.filter(ImageFilter.MaxFilter(pad * 2 + 1))
            result = Image.new('RGBA', expanded.size, (32, 15, 58))
            result.putalpha(outline)
            yy = np.linspace(0, 1, expanded.height)[:, None, None]
            top = np.array((255, 255, 212) if color[0] > 235 else color)
            bottom = np.array(color)
            rgb = np.repeat(np.clip(top * (1 - yy) + bottom * yy, 0, 255), expanded.width, axis=1).astype(np.uint8)
            fill = Image.fromarray(rgb).convert('RGBA')
            fill.putalpha(expanded)
            result.alpha_composite(fill)
            return result
        result = Image.new('RGBA', mask.size, color)
        result.putalpha(mask)
        return result


class BallFlight:
    """Swept-circle collisions; collision time, rather than a keyframed path.

    Game defaults: 6px ball, 9px peg, gravity .05px/tick² at 100Hz,
    normal impulse .9 (doubled for solid impacts). Film uses 3x artwork and
    time scaled to 55% so each bounce reads clearly in the short opener.
    """
    hz = 600
    scale = 3
    time_scale = .55
    radius = 18
    peg_radius = 27
    gravity = .05 * 100**2 * scale * time_scale**2

    def __init__(self, start=(1295.8009210275713, 121.4778812695824),
                 velocity=(-328.73512188180496, 158.40022731963126), duration=3.8):
        self.pegs = np.array([(1070 + col * 131 + (row % 2) * 48, 270 + row * 126)
                              for row in range(6) for col in range(6)], dtype=float)
        self.kinds = [1 if (row * 3 + col) % 4 == 0 else 0 for row in range(6) for col in range(6)]
        self.events = []
        self.states = []
        self.contacts = []
        self.min_clearance = float('inf')
        pos = np.array(start, dtype=float)
        vel = np.array(velocity, dtype=float)
        hit = set()
        dt = 1 / self.hz
        total_radius = self.radius + self.peg_radius
        for tick in range(round(duration * self.hz) + 1):
            now = tick * dt
            self.states.append(pos.copy())
            vel[1] += self.gravity * dt
            speed = np.linalg.norm(vel)
            cap = 15 * 100 * self.scale * self.time_scale
            if speed > cap:
                vel *= cap / speed
            remaining = dt
            for _ in range(8):
                # Quadratic ray/circle intersection selects the earliest hit.
                delta = pos - self.pegs
                a = float(vel @ vel)
                b = 2 * (delta @ vel)
                c = np.sum(delta * delta, axis=1) - total_radius**2
                discr = b * b - 4 * a * c
                roots = np.full(len(self.pegs), np.inf)
                valid = (discr >= 0) & (b < 0)
                roots[valid] = (-b[valid] - np.sqrt(discr[valid])) / (2 * max(a, 1e-10))
                roots[(roots < -1e-8) | (roots > remaining)] = np.inf
                index = int(np.argmin(roots))
                travel = roots[index]
                if not math.isfinite(travel):
                    pos += vel * remaining
                    break
                pos += vel * max(0, travel)
                normal = (pos - self.pegs[index]) / total_radius
                normal /= np.linalg.norm(normal)
                inward = float(vel @ normal)
                impulse = inward * .9
                if abs(impulse) > 100 * self.scale * self.time_scale:
                    impulse *= 2
                before = vel.copy()
                vel -= normal * impulse
                pos = self.pegs[index] + normal * (total_radius + .002)
                contact_time = now + dt - remaining + travel
                self.contacts.append((contact_time, index, before.tolist(), vel.tolist()))
                if index not in hit:
                    hit.add(index)
                    self.events.append((contact_time, index))
                remaining -= max(travel, 1e-8)
            clearance = float(np.min(np.linalg.norm(self.pegs - pos, axis=1)) - total_radius)
            self.min_clearance = min(self.min_clearance, clearance)
        self.states = np.array(self.states)

    def at(self, t):
        frame = max(0, min(len(self.states) - 1.001, t * self.hz))
        i = int(frame)
        return self.states[i] * (1 - (frame - i)) + self.states[i + 1] * (frame - i)

    def report(self):
        return {'unique_pegs_hit': len(self.events), 'collisions': len(self.contacts),
                'minimum_clearance_px': round(self.min_clearance, 5),
                'hits': [{'time': round(t, 4), 'peg': i} for t, i in self.events]}
