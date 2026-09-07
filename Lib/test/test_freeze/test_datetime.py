import unittest
from datetime import datetime, timedelta

from .test_common import BaseObjectTest


class TestDatetime(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=datetime.now(), **kwargs)


class TestDatetimeTimeDelta(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, obj=timedelta(days=1), **kwargs)


if __name__ == "__main__":
    unittest.main()
