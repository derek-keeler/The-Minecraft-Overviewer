import os
from pathlib import Path
import pickle
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from PIL import Image

from overviewer_core import textures


class TextureCacheTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        # keep any real ~/.minecraft jars out of the texture sources
        self.environment = patch.dict(os.environ, {
            "APPDATA": str(self.root), "HOME": str(self.root),
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.cache_dir = self.root / "cache"
        self.cache_dir.mkdir()
        self.jar = self.jar_with(b"textures")
        self.generate_calls = 0

    def jar_with(self, contents, name="client.jar"):
        path = self.root / name
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("contents", contents)
        return path

    def texture_object(self, texturepath=None, **options):
        result = textures.Textures(texturepath=str(texturepath or self.jar), **options)
        self.addCleanup(lambda: [jar.close() for jar in result.jars.values()])

        def fake_generate():
            # stands in for the 20+ second real generate()
            self.generate_calls += 1
            result.blockmap = [None, (Image.new("RGBA", (24, 24), "red"),
                                      Image.new("RGBA", (24, 24), "blue"))]
            result.biome_grass_texture = Image.new("RGBA", (24, 24), "green")
            result.generated = True
        result.generate = fake_generate
        return result

    def cache_entries(self):
        return sorted(p.name for p in self.cache_dir.iterdir())

    def test_key_is_stable_for_identical_inputs(self):
        self.assertEqual(self.texture_object().cache_key("26.3 (abc)"),
                         self.texture_object().cache_key("26.3 (abc)"))

    def test_key_changes_with_each_input(self):
        base = self.texture_object().cache_key("26.3 (abc)")
        self.assertNotEqual(base, self.texture_object().cache_key("26.3 (def)"))
        self.assertNotEqual(base, self.texture_object(northdirection=1).cache_key("26.3 (abc)"))
        self.assertNotEqual(base, self.texture_object(bgcolor=(0, 0, 0, 0)).cache_key("26.3 (abc)"))
        self.jar_with(b"other textures")  # same path, new contents
        self.assertNotEqual(base, self.texture_object().cache_key("26.3 (abc)"))

    def test_key_follows_jar_contents_not_path(self):
        moved = self.jar_with(b"textures", name="moved.jar")
        self.assertEqual(self.texture_object().cache_key("26.3"),
                         self.texture_object(moved).cache_key("26.3"))

    def test_fallback_client_jars_are_part_of_the_key(self):
        base = self.texture_object().cache_key("26.3")
        versions = self.root / ".minecraft" / "versions" / "26.3"
        versions.mkdir(parents=True)
        with zipfile.ZipFile(versions / "26.3.jar", "w") as archive:
            archive.writestr("contents", b"client")
        self.assertNotEqual(base, self.texture_object().cache_key("26.3"))

    def test_texture_directory_is_not_cached(self):
        pack = self.root / "pack"
        pack.mkdir()
        tex = self.texture_object(pack)
        self.assertIsNone(tex.cache_key("26.3"))
        tex.generate_cached(str(self.cache_dir), "26.3")
        self.assertEqual(self.generate_calls, 1)
        self.assertEqual(self.cache_entries(), [])

    def test_second_run_loads_instead_of_generating(self):
        first = self.texture_object()
        first.generate_cached(str(self.cache_dir), "26.3")
        self.assertEqual(self.generate_calls, 1)
        self.assertEqual(len(self.cache_entries()), 1)

        second = self.texture_object()
        second.generate_cached(str(self.cache_dir), "26.3")
        self.assertEqual(self.generate_calls, 1)
        self.assertTrue(second.generated)
        self.assertEqual(second.generated_path, first.generated_path)
        self.assertEqual(second.blockmap[1][0].tobytes(), first.blockmap[1][0].tobytes())

    def test_worker_copy_loads_cached_file(self):
        parent = self.texture_object()
        parent.generate_cached(str(self.cache_dir), "26.3")
        del parent.generate  # the fake can't be pickled
        with patch.object(textures.Textures, "generate",
                          side_effect=AssertionError("worker regenerated")):
            worker = pickle.loads(pickle.dumps(parent))
        self.assertEqual(worker.blockmap[1][1].tobytes(), parent.blockmap[1][1].tobytes())

    def test_changed_inputs_regenerate(self):
        self.texture_object().generate_cached(str(self.cache_dir), "26.3")
        self.texture_object().generate_cached(str(self.cache_dir), "26.4")
        self.assertEqual(self.generate_calls, 2)
        self.assertEqual(len(self.cache_entries()), 2)

    def test_corrupt_entry_is_regenerated_and_replaced(self):
        tex = self.texture_object()
        tex.generate_cached(str(self.cache_dir), "26.3")
        Path(tex.generated_path).write_bytes(b"not a pickle")
        again = self.texture_object()
        again.generate_cached(str(self.cache_dir), "26.3")
        self.assertEqual(self.generate_calls, 2)
        self.texture_object().generate_cached(str(self.cache_dir), "26.3")
        self.assertEqual(self.generate_calls, 2)

    def test_old_entries_are_pruned(self):
        for i in range(textures.Textures._cache_entries + 2):
            tex = self.texture_object()
            tex.generate_cached(str(self.cache_dir), "26.%d" % i)
            # distinct mtimes, oldest first
            os.utime(tex.generated_path, (i, i))
        self.assertEqual(len(self.cache_entries()), textures.Textures._cache_entries)
