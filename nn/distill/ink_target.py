#!/usr/bin/env python3
"""Thick-line targets, made from the MIT teacher by our own post-processing.

The AnimeGANv3 sketch models are non-commercial (design doc §2), so an ink
style cannot be distilled from them and stay MIT. Instead we build the
target ourselves: Informative Drawings gives line *intensity*, and thickness
is a morphology choice on top of it — our code, so the result stays ours.

Convention everywhere: NCHW float32, 1 = paper, 0 = ink (same as the teacher).
Morphology is done on the ink representation (1 = ink), where dilating a
stroke is just max_pool2d.

Two characters (design §7 "インク風"):
  gpen  — binary bold: only strong lines survive, then they fatten
  brush — pressure-varying: thickness follows line strength
"""
import torch
import torch.nn.functional as F


def _dilate(ink, k):
    return F.max_pool2d(ink, k, stride=1, padding=k // 2)


def _support(ink, k):
    """How much ink sits in the neighbourhood — a stroke has company, a speck doesn't."""
    return F.avg_pool2d(ink, k, stride=1, padding=k // 2)


def gpen(y, strong=0.40, weak=0.58, grow=4, width=3, support=7,
         min_support=0.05, soft=3):
    """Binary bold strokes — manga G-pen. Faint hatching is dropped entirely.

    Plain thresholding breaks a stroke into dashes wherever the teacher's
    intensity dips across the threshold, so this uses hysteresis: pixels
    darker than `strong` seed a stroke, then the seed grows along anything
    darker than `weak`. Ink that is only ever weak (foliage, stubble) is
    never seeded, which is also what keeps the confetti out — noise targets
    are unlearnable and would train the student into grey mush.

    width : dilation kernel, the actual thickness knob
    soft  : anti-alias window; a pure step is unfittable for a 105k net
    """
    seed = (y < strong).to(y.dtype)
    faint = (y < weak).to(y.dtype)
    for _ in range(grow):                # walk the seed along faint ink
        seed = _dilate(seed, 3) * faint
    seed = seed * (_support(seed, support) >= min_support).to(y.dtype)
    ink = _dilate(seed, width)           # thicken
    if soft > 1:
        ink = F.avg_pool2d(ink, soft, stride=1, padding=soft // 2)
    return (1.0 - ink).clamp(0, 1)


def brush(y, gain=1.5, width=3, bias=0.45):
    """Pressure-varying strokes — strong contours fatten, fine detail stays thin."""
    ink = ((1.0 - y) * gain).clamp(0, 1)          # amplified line strength
    fat = _dilate(ink, width)                      # how much ink is nearby
    spread = (fat - bias).clamp(min=0) / max(1 - bias, 1e-6)   # only strong lines spread
    return (1.0 - torch.maximum(ink, fat * spread)).clamp(0, 1)


STYLES = {'gpen': gpen, 'brush': brush}


def _box(x, r):
    k = 2 * r + 1
    return F.avg_pool2d(x, k, stride=1, padding=r, count_include_pad=False)


def simplify(rgb, r=6, eps=0.05):
    """Edge-preserving smoothing (guided filter, self-guided) on the *input photo*.

    This is what buys the illustration look. Informative Drawings draws a line
    wherever there is contrast, so skin pores, hair strands and foliage all
    come back as scribble. Killing that texture before the teacher sees it
    means the teacher never draws it — the simplification happens for free,
    and the strokes that survive are the ones a person would have drawn.
    eps is the texture/edge boundary: bigger = more gets flattened.
    """
    mean = _box(rgb, r)
    var = (_box(rgb * rgb, r) - mean * mean).clamp(min=0)
    a = var / (var + eps)
    return _box(a, r) * rgb + _box((1 - a) * mean, r)


def portrait_ink(y, rgb=None, width=3, strong=0.50, weak=0.70, grow=8, soft=3):
    """Bold simplified ink — a self-made stand-in for the non-commercial
    PortraitSketch. Expects `y` computed from a simplify()'d photo, which is
    where the illustration look comes from: with the texture gone before the
    teacher sees it, what is left to draw is the outline a person would draw.

    No solid fills. Filling by luminance was tried and dropped: it blacks out
    *dark regions* (a navy bag, a wooden table) rather than *shadows*, which
    reads as a posterised photo instead of a drawing. PortraitSketch decides
    that semantically; a threshold cannot stand in for it.
    """
    return gpen(y, strong=strong, weak=weak, grow=grow, width=width,
                min_support=0.03, soft=soft)


STYLES['portrait_ink'] = portrait_ink
