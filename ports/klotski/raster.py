#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline baker for the pinned rect/polygon GNOME atlas, using only stdlib.

This deliberately supports this asset's SVG subset, not arbitrary SVG. Unknown
elements/attributes fail. Gaussian SourceAlpha shadows are sampled off-line;
the guest receives opaque RGB565, with no vector/filter dependencies.
"""
import math
import re
import xml.etree.ElementTree as ET


def bake(path):
    scale = 3
    w, h = 918*scale, 68*scale
    bg = (26, 33, 39)
    pixels = [bg] * (w*h)

    def polygon(points, color, target):
        points = [(x*scale, y*scale) for x, y in points]
        for y in range(max(0, math.floor(min(p[1] for p in points))), min(h, math.ceil(max(p[1] for p in points)))):
            cy = y+.5
            xs = []
            for (x1, y1), (x2, y2) in zip(points, points[1:]+points[:1]):
                if (y1 <= cy < y2) or (y2 <= cy < y1):
                    xs.append(x1+(cy-y1)*(x2-x1)/(y2-y1))
            xs.sort()
            for left, right in zip(xs[::2], xs[1::2]):
                lo, hi = max(0, math.ceil(left-.5)), min(w, math.ceil(right-.5))
                target[y*w+lo:y*w+hi] = [color] * (hi-lo)

    def rect(x, y, width, height, color, target):
        polygon([(x,y),(x+width,y),(x+width,y+height),(x,y+height)],color,target)

    def rgb(value):
        if not re.fullmatch(r'#[0-9a-fA-F]{6}', value):
            raise ValueError('unsupported colour: '+value)
        return tuple(int(value[i:i+2],16) for i in (1,3,5))

    def render(node, target, tx=0., ty=0., alpha=False):
        tag = node.tag.split('}')[-1]
        if tag == 'defs':
            return
        if tag in ('svg','g'):
            if 'transform' in node.attrib:
                m = re.fullmatch(r'translate\(([\d.]+),([\d.]+)\)',node.attrib['transform'])
                if not m: raise ValueError('unsupported transform')
                tx+=float(m[1]);ty+=float(m[2])
            if 'filter' in node.attrib:
                if node.attrib['filter'] != 'url(#shadow)': raise ValueError('unknown filter')
                shadow = [0]*(w*h)
                for child in node: render(child,shadow,tx,ty,True)
                # Separable Gaussian, sigma=1.4 logical pixels, clipped to the
                # original filter rectangle. Only used in asset regeneration.
                radius=math.ceil(1.4*scale*3)
                kernel=[math.exp(-.5*(i/(1.4*scale))**2) for i in range(-radius,radius+1)]
                total=sum(kernel);kernel=[v/total for v in kernel]
                temp=[0.]*(w*h)
                for y in range(h):
                    for x in range(w):
                        if shadow[y*w+x]:
                            for k, weight in enumerate(kernel):
                                xx=x+k-radius
                                if 0<=xx<w: temp[y*w+xx]+=weight
                for y in range(10*scale,58*scale):
                    for x in range(894*scale):
                        a=sum(temp[yy*w+x]*weight for yy,weight in zip(range(y-radius,y+radius+1),kernel) if 0<=yy<h)
                        if a: target[y*w+x]=tuple(round(v*(1-min(1.,a))) for v in target[y*w+x])
            else:
                for child in node: render(child,target,tx,ty,alpha)
            return
        allowed = {'x','y','width','height','fill','stroke'} if tag=='rect' else {'fill','points'}
        if tag not in ('rect','polygon') or set(node.attrib)-allowed:
            raise ValueError('unsupported SVG element: '+tag)
        color=1 if alpha else rgb(node.get('fill','#000000'))
        if tag=='rect':
            x=float(node.get('x','0'))+tx;y=float(node.get('y','0'))+ty
            width=float(node.get('width'));height=float(node.get('height'))
            rect(x,y,width,height,color,target)
            if 'stroke' in node.attrib:
                color=1 if alpha else rgb(node.get('stroke'))
                for r in [(x-.5,y-.5,width+1,1),(x-.5,y+height-.5,width+1,1),(x-.5,y+.5,1,height-1),(x+width-.5,y+.5,1,height-1)]:rect(*r,color,target)
        else:
            values=[float(v) for v in re.split(r'[\s,]+',node.get('points').strip())]
            polygon([(x+tx,y+ty) for x,y in zip(values[::2],values[1::2])],color,target)
    render(ET.parse(path).getroot(),pixels)
    result=[]
    for y in range(17,51):
        for x in range(918):
            samples=[pixels[(y*scale+dy)*w+x*scale+dx] for dy in range(scale) for dx in range(scale)]
            result.append(tuple(round(sum(p[c] for p in samples)/(scale*scale)) for c in range(3)))
    return result
