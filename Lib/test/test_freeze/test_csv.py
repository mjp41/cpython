import csv
from io import BytesIO

from .test_common import BaseNotFreezableTest


class TestCSVReader(BaseNotFreezableTest):
    def test_not_freezable(self):
        self.check_not_freezable(csv.reader([]))


class TestCSVWriter(BaseNotFreezableTest):
    def test_not_freezable(self):
        self.buffer = BytesIO()
        self.check_not_freezable(csv.writer(self.buffer))
