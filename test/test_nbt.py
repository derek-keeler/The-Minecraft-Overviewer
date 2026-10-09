from io import BytesIO
import struct
import unittest
import zlib

from overviewer_core import nbt


class NBTReaderTests(unittest.TestCase):
    def test_repeated_strings_numeric_lists_and_arrays(self):
        payload = b"".join((
            b"\x0a\x00\x00",
            b"\x08\x00\x04Name\x00\x05stone",
            b"\x08\x00\x05Other\x00\x05stone",
            b"\x09\x00\x04Ints\x03\x00\x00\x00\x03",
            struct.pack(">3i", -1, 0, 42),
            b"\x0b\x00\x05Array\x00\x00\x00\x02",
            struct.pack(">2i", 100, -100),
            b"\x00",
        ))

        name, result = nbt.NBTFileReader(
            zlib.compress(payload), is_gzip=False).read_all()

        self.assertEqual(name, "")
        self.assertEqual(result["Name"], "stone")
        self.assertIs(result["Name"], result["Other"])
        self.assertEqual(result["Ints"], [-1, 0, 42])
        # Int arrays (spawn pos, UUIDs) reach JSON output and POI filters,
        # so they stay plain Python ints rather than NumPy scalars.
        self.assertEqual(result["Array"], (100, -100))

    def test_surrogate_encoded_strings_are_recombined(self):
        reader = nbt.NBTFileReader.__new__(nbt.NBTFileReader)
        encoded = b"\xed\xa0\xbd\xed\xb8\x80"
        reader._file = BytesIO(struct.pack(">H", len(encoded)) + encoded)
        self.assertEqual(reader._read_tag_string(), "\U0001f600")
