# Simple library that defines a mutable cell class
class Cell(object):
    def __init__(self, value):
        self.set(value)

    def set(self, value):
        self.value = value

    def get(self):
        return self.value

    def __repr__(self):
        return "Cell(" + repr(self.value) + ")"

# Package an object into a cown
def pack(*obj):
    r = Region()
    r.f = obj
    c = Cown(r)
    del cell
    del r
    return c

# Get the contents of a cown
def unpack(cown):
    return cown.get()

# Put contents back into a cown
def repack(cown, *obj):
    cown.set(obj)
