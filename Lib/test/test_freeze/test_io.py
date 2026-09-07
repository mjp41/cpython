import io

from .test_common import BaseNotFreezableTest


class BytesIOTest(BaseNotFreezableTest):
    def test_not_freezable(self):
        obj = io.BytesIO()
        self.check_not_freezable(obj)
        obj.close()


class StringIOTest(BaseNotFreezableTest):
    def test_not_freezable(self):
        obj = io.StringIO()
        self.check_not_freezable(obj)
        obj.close()


class TextWrapperTest(BaseNotFreezableTest):
    def test_not_freezable(self):
        handle = open(__file__, 'r')
        self.check_not_freezable(handle)
        handle.close()


class RawWrapperTest(BaseNotFreezableTest):
    def test_not_freezable(self):
        handle = open(__file__, 'rb')
        self.check_not_freezable(handle)
        handle.close()
