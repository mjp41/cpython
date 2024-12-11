class Region:
    def __init__(self, name: str, **kwargs):
        self.name = name
        self.shared = False
        for k, v in kwargs.items():
            setattr(self, k, v)

    def open(self):
        print("opening", self.name)

    def close(self):
        print("closing", self.name)


def when(region: Region):
    def decorator(func):
        region.open()
        func(region)
        region.close()

    return decorator


def cown(region: Region):
    region.shared = True
    return region


class T:
    def __init__(self, name: str):
        self.name = name


x = T("obj0")
y = T("obj1")
z = T("obj2")
r = Region("reg0", f=x)
s = Region("reg1", g=y)

# if somethings gets passed to when, can we just implicitly
# share it?
@when(r) 
def b0():
    print(r.f)

    # is this ok?
    # since x is in r, can we capture x?
    print(x)

    # what region does z belong to? Or is it just
    # moved into b0 as a local? What mechanism would
    # we use to do that (as z may have been allocated
    # elsewhere)
    print(z)

    # This statement transfers ownership of s
    # to b0
    print(s.g)


@when(r)
def b1():
    print(r.f)

    # this should probably throw an error, as z has
    # been moved (somehow) to b0. This bit will be
    # weird for Python programmers, but if we threw
    # a helpful error on Behavior creation explaining
    # why this Behavior can't access z then maybe its
    # OK
    print(z)

    # Throws an error
    # where/when does this get thrown?
    print(s.g)


# Do we throw an error here, or above (in the implicit
# sharing scenario)?
@when(s)
def b2():
    print(s.g)


"""
Given that we have to do capture analysis on the behaviour
(if we want to allow capture of locals) we know which
regions each behavior captures. This means we could, dynamically,
determine what regions can be moved into the behavior, and what
regions need to be shared, and do so automatically. Is this
a good idea? It would mean that users could write code that looks
exactly like Python, and that it would "just work" with the cost
of some things being implicitly promoted to cowns.

Alternatively, we can require that the user explicitly share
regions. Let's look at the same code with a version of that
scenario.
"""

x = T("obj0")
y = T("obj1")
z = T("obj2")
r = Region("reg0", f=x)
s = Region("reg1", g=y)
cr = cown(r)

# in the explicit sharing scenario, we do not need
# to pass the region, because we're doing capture
# analysis and we can see which regions cowns are being
# used.


@when
def b0():
    print(cr.f)

    # Same question: is this ok?
    # since x is in r and we have temporal ownership of cr,
    # this is OK but potentially confusing.
    print(x)

    # This statement transfers ownership of s
    # to b0.
    print(s.g)

# In this alternate version, the programmer says
# what cowns/regions they want to capture. This is
# nice because we can then do capture analysis and
# compare the captured regions against this list
# and given a helpful error message.


@when(cr, s)
def b0():
    print(cr.f)
    print(s.g)
