"""Exercise the production logger against real files with shifted mtimes."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Policy(ctypes.Structure):
    _fields_ = [('directory', ctypes.c_char_p), ('file_bytes', ctypes.c_uint64),
                ('retention_days', ctypes.c_uint), ('total_bytes', ctypes.c_uint64),
                ('minimum_level', ctypes.c_int)]


class RetentionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        lib = Path(cls.build.name)/'logger.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-pthread',
            '-DCARPLAY_LOG_TEST_NO_ANDROID', '-I'+str(ROOT/'src'),
            str(ROOT/'src/carplay_log.c'), '-o', str(lib)], check=True)
        cls.log = ctypes.CDLL(str(lib))
        cls.log.carplay_log_init.argtypes = [ctypes.POINTER(Policy)]
        cls.log.carplay_log_write.argtypes = [ctypes.c_int, ctypes.c_char_p]

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.log.carplay_log_shutdown()
        self.tmp.cleanup()

    def saved_log(self, name, age_seconds, size=100):
        path = self.root/('carplay-'+name+'.log')
        path.write_bytes(b'x'*size)
        stamp = time.time()-age_seconds
        os.utime(path, (stamp, stamp))
        return path

    def start(self, quota=65536):
        self.policy = Policy(os.fsencode(self.root), 16384, 7, quota, 1)
        self.assertEqual(self.log.carplay_log_init(ctypes.byref(self.policy)), 0)

    def test_info_writes_do_not_scan_the_log_directory_each_line(self):
        self.start()
        expired = self.saved_log('late-old', 9*86400)
        for i in range(200):
            self.log.carplay_log_write(1, f'normal traffic {i}'.encode())
        self.assertTrue(expired.exists(), 'ordinary writes rescanned logs and removed retention evidence')

    def test_clock_rollback_does_not_delete_saved_evidence(self):
        future = self.saved_log('prior-boot', -86400)
        self.start()
        self.assertTrue(future.exists(), 'clock rollback deleted future-dated evidence')

    def test_real_expiry_still_removes_old_logs(self):
        expired = self.saved_log('old', 9*86400)
        fresh = self.saved_log('recent', 60)
        self.start()
        self.assertFalse(expired.exists())
        self.assertTrue(fresh.exists())

    def test_quota_still_bounds_future_dated_logs(self):
        saved = [self.saved_log(str(i), -(i+1)*86400, 10000) for i in range(3)]
        self.start(quota=16384)
        surviving = [p for p in saved if p.exists()]
        self.assertEqual(surviving, [saved[-1]])
        self.assertLessEqual(sum(p.stat().st_size for p in self.root.iterdir()), 16384)


if __name__ == '__main__':
    unittest.main()
