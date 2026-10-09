import queue
import shutil
import tempfile
import unittest

from overviewer_core import dispatcher, tileset, world
from . import test_tileset


class FakeTileset(object):
    """Work items are (strip, n) tuples; strip None means ungrouped."""
    def get_work_group(self, workitem):
        return workitem[0]


class FakeManager(object):
    tileset_version = 1

    def __init__(self, tilesets):
        self.tilesets = tilesets


class BundlingTest(unittest.TestCase):
    def setUp(self):
        # a dispatcher without worker processes, recording the job messages
        self.tileset = FakeTileset()
        d = dispatcher.MultiprocessingDispatcher.__new__(dispatcher.MultiprocessingDispatcher)
        d.manager = FakeManager([self.tileset])
        d.job_queue = queue.Queue()
        d.result_queue = queue.Queue()
        d.signal_queue = queue.Queue()
        d.outstanding_jobs = 0
        d.num_workers = 4
        d._bundle = None
        self.dispatcher = d

    def jobs(self):
        out = []
        while not self.dispatcher.job_queue.empty():
            out.append(self.dispatcher.job_queue.get()[2])
        return out

    def test_consecutive_items_of_a_group_share_a_job(self):
        for item in [("a", 1), ("a", 2), (None, 3), ("a", 4), ("b", 5)]:
            self.dispatcher.dispatch(self.tileset, item)
        self.dispatcher.dispatch(None, None)
        # the ungrouped item is sent straight away without splitting strip "a"
        self.assertEqual(self.jobs(), [[(None, 3)], [("a", 1), ("a", 2), ("a", 4)], [("b", 5)]])
        self.assertEqual(self.dispatcher.outstanding_jobs, 3)

    def test_bundles_are_capped(self):
        self.dispatcher.max_bundle = 2
        for n in range(5):
            self.dispatcher.dispatch(self.tileset, ("a", n))
        self.dispatcher.dispatch(None, None)
        self.assertEqual([len(job) for job in self.jobs()], [2, 2, 1])

    def test_finished_job_reports_every_item(self):
        self.dispatcher.dispatch(self.tileset, ("a", 1))
        self.dispatcher.dispatch(self.tileset, ("a", 2))
        self.dispatcher.dispatch(None, None)
        job = self.dispatcher.job_queue.get()
        self.dispatcher.result_queue.put((job[1], job[2], [None, None]))
        finished = self.dispatcher._handle_messages(timeout=0.0)
        self.assertEqual(finished, [(self.tileset, ("a", 1)), (self.tileset, ("a", 2))])
        self.assertEqual(self.dispatcher.outstanding_jobs, 0)


class RecordingDispatcher(dispatcher.Dispatcher):
    """Runs nothing; records the order work is dispatched in. Jobs only
    finish when the dispatcher has nothing else it can start, and starting
    one before the work it depends on has finished is recorded."""
    def __init__(self):
        super(RecordingDispatcher, self).__init__()
        self.order = []
        self.dispatched = set()
        self.running = []
        self.finished = set()
        self.started_early = []

    def dispatch(self, tileset, workitem):
        if tileset is None:
            finished, self.running = self.running, []
            self.finished.update(finished)
            return finished
        for child in range(4):
            job = (tileset, workitem + (child,))
            if job in self.dispatched and job not in self.finished:
                self.started_early.append((tileset, workitem))
        self.order.append((tileset, workitem))
        self.dispatched.add((tileset, workitem))
        self.running.append((tileset, workitem))
        return []


class NullObserver(object):
    def start(self, total):
        pass

    def add(self, amount):
        pass

    def finish(self):
        pass


class MergedWorkTest(unittest.TestCase):
    def setUp(self):
        self.tempdirs = []
        self.chunks = dict(((x, z), 5) for x in range(-20, 20) for z in range(-20, 20))
        self.overworld = world.CachedRegionSet(test_tileset.FakeRegionset(self.chunks), [])

    def tearDown(self):
        for d in self.tempdirs:
            shutil.rmtree(d)

    def tileset(self, rset=None, preprocess=None, **options):
        defoptions = {'name': 'world name', 'bgcolor': '#000000', 'imgformat': 'png',
                      'optimizeimg': 0, 'rendermode': 'normal', 'rerenderprob': 0,
                      'renderchecks': 2}
        defoptions.update(options)
        outputdir = tempfile.mkdtemp(prefix="OVTEST")
        self.tempdirs.append(outputdir)
        ts = tileset.TileSet(None, rset or self.overworld, test_tileset.FakeAssetmanager(0),
                             None, defoptions, outputdir)
        if preprocess:
            preprocess(ts)
        ts.do_preprocessing()
        return ts

    def assert_complete_and_ordered(self, tilesets, order):
        """Every tileset's work is given exactly once, children before
        parents."""
        for ts in tilesets:
            mine = [item for t, item in order if t is ts]
            self.assertEqual(len(mine), len(set(mine)))
            self.assertEqual(set(mine), set(item for item, _ in ts.iterate_work_items(0)))
            position = dict((item, i) for i, item in enumerate(mine))
            for item, i in position.items():
                for child in range(4):
                    if item + (child,) in position:
                        self.assertLess(position[item + (child,)], i)

    def merged(self, tilesets):
        return [(ts, item) for ts, item, _ in dispatcher.iterate_merged_work(tilesets, 0)]

    def test_one_tileset_keeps_its_order(self):
        ts = self.tileset()
        self.assertEqual([item for _, item in self.merged([ts])],
                         [item for item, _ in ts.iterate_work_items(0)])

    def test_tilesets_sharing_chunks_render_strips_together(self):
        for count in (2, 5):
            tilesets = [self.tileset() for _ in range(count)]
            order = self.merged(tilesets)
            self.assert_complete_and_ordered(tilesets, order)

            # all of a strip's render-tiles, for every tileset, in one run
            strips = [ts.get_work_group(item) for ts, item in order]
            runs = [s for previous, s in zip([None] + strips, strips) if s != previous]
            runs = [s for s in runs if s is not None]
            self.assertGreater(len(runs), 1)
            self.assertEqual(len(runs), len(set(runs)))
            members = {}
            for ts, item in order:
                members.setdefault(ts.get_work_group(item), set()).add(ts)
            for strip in runs:
                self.assertEqual(len(members[strip]), count)

    def test_mixed_tilesets(self):
        """Other dimensions, north directions and tile-checking tilesets
        aren't grouped, and a tileset with fewer dirty tiles shares the strips
        it has."""
        nether = world.CachedRegionSet(test_tileset.FakeRegionset(self.chunks, "world/DIM-1/region"), [])
        # the same world, with one chunk changed since the last render
        changed = dict(self.chunks)
        changed[(3, 3)] = 6
        updated = self.tileset(world.CachedRegionSet(test_tileset.FakeRegionset(changed), []),
                               renderchecks=0,
                               preprocess=lambda ts: setattr(ts, 'last_rendertime', 5))
        self.assertTrue(list(updated.iterate_work_items(0)))
        tilesets = [self.tileset(), updated, self.tileset(nether),
                    self.tileset(world.RotatedRegionSet(self.overworld, 1)),
                    self.tileset(renderchecks=1)]
        order = self.merged(tilesets)
        self.assert_complete_and_ordered(tilesets, order)

        # the updated tileset's few render-tiles come with the full one's
        full = tilesets[0]
        for i, (ts, item) in enumerate(order):
            if ts is updated and len(item) == ts.treedepth:
                strip = ts.get_work_group(item)
                self.assertTrue(any(t is full and full.get_work_group(it) == strip
                                    for t, it in order[max(0, i - 40):i + 40]))
        # other groups are interleaved, not run one after another
        firsts = [next(i for i, (t, _) in enumerate(order) if t is ts) for ts in tilesets]
        self.assertLess(max(firsts), len(order) // 4)

    def test_changelist_names_each_tile_once(self):
        with tempfile.TemporaryFile() as changelist:
            tilesets = [self.tileset(changelist=changelist.fileno()) for _ in range(3)]
            order = self.merged(tilesets)
            changelist.seek(0)
            lines = changelist.read().decode().splitlines()
        self.assertEqual(len(lines), len(order))
        self.assertEqual(len(set(lines)), len(lines))

    def test_dispatcher_runs_children_first(self):
        tilesets = [self.tileset() for _ in range(3)]
        d = RecordingDispatcher()
        d.render_all(tilesets, NullObserver())
        self.assert_complete_and_ordered(tilesets, d.order)
        self.assertEqual(d.started_early, [])
