from _multiprocessing import SemLock

from .test_common import BaseNotFreezableTest

SEMAPHORE = 1
SEM_VALUE_MAX = SemLock.SEM_VALUE_MAX

class TestSemLock(BaseNotFreezableTest):
    def test_not_freezable(self):
        self.check_not_freezable(SemLock(SEMAPHORE, 0, SEM_VALUE_MAX, "mock", True))
