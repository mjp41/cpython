from xml.etree.ElementTree import Element, XMLParser
import unittest


from .test_common import BaseNotFreezableTest, BaseObjectTest

# TODO(Immutable): Should this be true?  Review later.
# class TestElementTree(BaseNotFreezableTest):
#     def __init__(self, *args, **kwargs):
#         super().__init__(*args, obj=ElementTree(), **kwargs)

class TestXMLParser(BaseNotFreezableTest):
    def test_not_freezable(self):
        self.check_not_freezable(XMLParser())


class TestElement(BaseObjectTest):
    def __init__(self, *args, **kwargs):
        obj = Element("tag", {"key": "value"})
        super().__init__(*args, obj=obj, **kwargs)

    def test_set(self):
        with self.assertRaises(TypeError):
            self.obj.set("key", "value")

    def test_setitem(self):
        with self.assertRaises(TypeError):
            self.obj["key"] = "value"

    def test_delitem(self):
        with self.assertRaises(TypeError):
            del self.obj["key"]

    def test_clear(self):
        with self.assertRaises(TypeError):
            self.obj.clear()

    def test_append(self):
        with self.assertRaises(TypeError):
            self.obj.append(Element("child"))

    def test_insert(self):
        with self.assertRaises(TypeError):
            self.obj.insert(0, Element("child"))

    def test_remove(self):
        with self.assertRaises(TypeError):
            self.obj.remove(Element("child"))


if __name__ == '__main__':
    unittest.main()
