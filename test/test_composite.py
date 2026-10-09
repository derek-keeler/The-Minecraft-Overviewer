import random
import unittest

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
