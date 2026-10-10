import platform
import random
import unittest

import numpy

from PIL import Image

from overviewer_core import c_overviewer


def muldiv255(a, b):
    tmp = a * b + 128
    return ((tmp >> 8) + tmp) >> 8


def reference_alpha_over(dest, src, pos, mask):
    """The C blend, one pixel at a time, for images placed fully inside dest."""
    out = dest.load()
    source = src.load()
    alphas = mask.load()
    dx, dy = pos
    for y in range(src.size[1]):
        for x in range(src.size[0]):
            in_alpha = alphas[x, y]
            if mask.mode == "RGBA":
                in_alpha = in_alpha[3]
            r, g, b, out_alpha = out[dx + x, dy + y]
            colour = source[x, y][:3]
            if in_alpha == 255 or (out_alpha == 0 and in_alpha > 0):
                out[dx + x, dy + y] = colour + (in_alpha,)
            elif in_alpha != 0:
                alpha = in_alpha + muldiv255(out_alpha, 255 - in_alpha)
                blended = []
                for i, o in zip(colour, (r, g, b)):
                    value = (muldiv255(i, in_alpha) +
                             muldiv255(muldiv255(o, out_alpha), 255 - in_alpha)) & 255
                    blended.append(value * 255 // alpha)
                out[dx + x, dy + y] = tuple(blended) + (alpha,)


def reference_tint_with_mask(dest, colour, mask, pos):
    """The C tint, one pixel at a time, for masks placed fully inside dest."""
    out = dest.load()
    alphas = mask.load()
    dx, dy = pos
    for y in range(mask.size[1]):
        for x in range(mask.size[0]):
            m = alphas[x, y]
            if mask.mode == "RGBA":
                m = m[3]
            if m == 0:
                continue
            pixel = out[dx + x, dy + y]
            if m == 255:
                out[dx + x, dy + y] = tuple(muldiv255(p, c) for p, c in zip(pixel, colour))
            else:
                out[dx + x, dy + y] = tuple(muldiv255(p, (255 - m) + muldiv255(c, m))
                                            for p, c in zip(pixel, colour))


def reference_draw_triangle(dest, inclusive, p0, p1, p2, tu, touchups):
    """The C triangle, one pixel at a time, with its float32 arithmetic."""
    f = numpy.float32
    out = dest.load()
    (x0, y0, r0, g0, b0), (x1, y1, r1, g1, b1), (x2, y2, r2, g2, b2) = p0, p1, p2
    a12, b12, c12 = y1 - y2, x2 - x1, x1 * y2 - x2 * y1
    a20, b20, c20 = y2 - y0, x0 - x2, x2 * y0 - x0 * y2
    a01, b01, c01 = y0 - y1, x1 - x0, x0 * y1 - x1 * y0
    with numpy.errstate(divide="ignore", invalid="ignore"):
        alpha_norm = f(1) / f(a12 * x0 + b12 * y0 + c12)
        beta_norm = f(1) / f(a20 * x1 + b20 * y1 + c20)
        gamma_norm = f(1) / f(a01 * x2 + b01 * y2 + c01)

    def shade(x, y, test):
        with numpy.errstate(invalid="ignore", over="ignore"):
            alpha = alpha_norm * f(a12 * x + b12 * y + c12)
            beta = beta_norm * f(a20 * x + b20 * y + c20)
            gamma = gamma_norm * f(a01 * x + b01 * y + c01)
            if test and not (alpha >= 0 and beta >= 0 and gamma >= 0 and
                             (inclusive or alpha * beta * gamma > 0)):
                return
            pixel = out[x, y]
            colour = [int(alpha * f(c0) + beta * f(c1) + gamma * f(c2))
                      for c0, c1, c2 in ((r0, r1, r2), (g0, g1, g2), (b0, b1, b2))]
        out[x, y] = tuple(muldiv255(p, c) & 255 for p, c in zip(pixel[:3], colour)) + (pixel[3],)

    width, height = dest.size
    for y in range(max(min(y0, y1, y2), 0), min(max(y0, y1, y2) + 1, height)):
        for x in range(max(min(x0, x1, x2), 0), min(max(x0, x1, x2) + 1, width)):
            shade(x, y, True)
    for i in range(0, len(touchups), 2):
        x, y = touchups[i] + tu[0], touchups[i + 1] + tu[1]
        if 0 <= x < width and 0 <= y < height:
            shade(x, y, False)


class AlphaOverTests(unittest.TestCase):
    def random_image(self, rng, mode, size):
        def alpha():
            # mostly the fully opaque and fully transparent runs of real textures
            choice = rng.random()
            return 0 if choice < 0.35 else 255 if choice < 0.8 else rng.randrange(256)

        pixels = bytearray()
        for _ in range(size[0] * size[1]):
            pixel = [rng.randrange(256) for _ in mode]
            if mode in ("RGBA", "L"):
                pixel[-1] = alpha()
            pixels += bytes(pixel)
        return Image.frombytes(mode, size, bytes(pixels))

    def test_matches_reference_blend(self):
        rng = random.Random(26)
        for case in range(200):
            size = (rng.randrange(1, 30), rng.randrange(1, 30))
            dest = self.random_image(rng, "RGBA", (32, 32))
            src = self.random_image(rng, rng.choice(["RGBA", "RGB"]), size)
            if src.mode == "RGBA" and rng.random() < 0.5:
                mask = src
            else:
                mask = self.random_image(rng, rng.choice(["RGBA", "L"]), size)
            pos = (rng.randrange(0, 33 - size[0]), rng.randrange(0, 33 - size[1]))

            expected = dest.copy()
            reference_alpha_over(expected, src, pos, mask)
            c_overviewer.alpha_over(dest, src, pos, mask)

            self.assertEqual(dest.tobytes(), expected.tobytes(), "case %d" % case)

    def test_clips_to_destination(self):
        dest = Image.new("RGBA", (8, 8), (10, 20, 30, 255))
        src = Image.new("RGBA", (6, 6), (200, 100, 50, 255))

        c_overviewer.alpha_over(dest, src, (-3, 5), src)

        self.assertEqual(dest.getpixel((0, 5)), (200, 100, 50, 255))
        self.assertEqual(dest.getpixel((2, 7)), (200, 100, 50, 255))
        self.assertEqual(dest.getpixel((3, 5)), (10, 20, 30, 255))
        self.assertEqual(dest.getpixel((0, 4)), (10, 20, 30, 255))

    def test_reads_closed_image_error(self):
        dest = Image.new("RGBA", (4, 4))
        src = Image.new("RGBA", (4, 4))
        src.close()

        with self.assertRaises(ValueError):
            c_overviewer.alpha_over(dest, src, (0, 0), dest)


if __name__ == "__main__":
    unittest.main()


class TintWithMaskTests(unittest.TestCase):
    random_image = AlphaOverTests.random_image

    def test_matches_reference_tint(self):
        rng = random.Random(27)
        colours = [(255, 255, 255, 0), (255, 255, 255, 255), (255, 255, 255, 128)]
        for case in range(300):
            if rng.random() < 0.5:
                colour = rng.choice(colours)
            else:
                # lighting and biome tints keep alpha; some leave channels at 255
                colour = tuple(rng.choice([255, rng.randrange(256)]) for _ in range(3))
                colour += (rng.choice([255, rng.randrange(256)]),)
            size = (rng.randrange(1, 30), rng.randrange(1, 30))
            dest = self.random_image(rng, "RGBA", (32, 32))
            mask = self.random_image(rng, rng.choice(["RGBA", "L"]), size)
            pos = (rng.randrange(0, 33 - size[0]), rng.randrange(0, 33 - size[1]))

            expected = dest.copy()
            reference_tint_with_mask(expected, colour, mask, pos)
            c_overviewer._tint_with_mask(dest, colour, mask, pos)
            self.assertEqual(dest.tobytes(), expected.tobytes(),
                             "case %d: colour %r, mask %s %r at %r" % (case, colour, mask.mode, size, pos))

    def test_clips_to_destination(self):
        rng = random.Random(28)
        for pos in [(-5, -5), (20, 20), (-30, 0), (31, 31)]:
            dest = self.random_image(rng, "RGBA", (32, 32))
            mask = self.random_image(rng, "RGBA", (24, 24))
            expected = dest.copy()
            # the reference on a large canvas, cropped back to dest
            canvas = Image.new("RGBA", (96, 96))
            canvas.paste(dest, (32, 32))
            reference_tint_with_mask(canvas, (255, 255, 255, 0), mask, (pos[0] + 32, pos[1] + 32))
            expected = canvas.crop((32, 32, 64, 64))
            c_overviewer._tint_with_mask(dest, (255, 255, 255, 0), mask, pos)
            self.assertEqual(dest.tobytes(), expected.tobytes(), "at %r" % (pos,))


class DrawTriangleTests(unittest.TestCase):
    random_image = AlphaOverTests.random_image

    def test_matches_reference_triangle(self):
        # The float colour sums may be fused into multiply-adds on other
        # architectures, so allow those to differ by one.
        tolerance = 0 if platform.machine().lower() in ("x86_64", "amd64") else 1
        rng = random.Random(29)
        # the smooth lighting faces, and random ones (some degenerate or clipped)
        faces = [((0, 6), (12, 0), (24, 6), (12, 12)), ((0, 18), (0, 6), (12, 12), (12, 24)),
                 ((24, 6), (12, 12), (12, 24), (24, 18))]
        for case in range(300):
            dest = self.random_image(rng, "RGBA", (40, 40))
            if case % 2:
                corners = rng.choice(faces)
                ox, oy = rng.randrange(-8, 24), rng.randrange(-8, 24)
                points = [(x + ox, y + oy) for x, y in rng.choice([corners[:3], (corners[0], corners[2], corners[3])])]
            else:
                points = [(rng.randrange(-10, 50), rng.randrange(-10, 50)) for _ in range(3)]
                if case % 10 == 0:
                    points[2] = points[1]
            vertices = [p + tuple(rng.randrange(256) for _ in range(3)) for p in points]
            inclusive = rng.randrange(2)
            # touch-ups only come with the (never degenerate) lighting faces
            touchups = [1, 5, 3, 4, 5, 3, 7, 2, 9, 1, 11, 0] if case % 2 and rng.random() < 0.5 else []
            tu = (rng.randrange(0, 20), rng.randrange(0, 20))

            expected = dest.copy()
            reference_draw_triangle(expected, inclusive, *vertices, tu, touchups)
            c_overviewer._draw_triangle(dest, inclusive, *vertices, tu, touchups)
            got = numpy.asarray(dest, dtype=int)
            want = numpy.asarray(expected, dtype=int)
            self.assertLessEqual(int(numpy.abs(got - want).max()), tolerance,
                                 "case %d: %r inclusive %d" % (case, vertices, inclusive))
