import queue
import unittest

from overviewer_core import dispatcher


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
