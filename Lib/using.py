from contextlib import contextmanager

# This library defines a decorator "@using" that uses blocking semantics.
# A function decorated by a @using will be called as a result of its
# definition.
#
# Example:
#
# @using(c1, c2)
# def _():
#     print(f"c1 and c2 are now acquired")
#
# Assuming c1 and c2 are cowns, the system will block on acquiring them,
# then call the function _ and release c1 and c2 when the function
# terminates. If c1 or c2 are updated with a closed region, a cown or an
# immutable object, c1 or c2 will be released immediately.


def using(*args):
    @contextmanager
    def CS(cowns, *args):
        for c in cowns:
            c.acquire()

        try:
            # Yield control to the code inside the 'with' block
            yield args
        finally:
            for c in cowns:
                c.release()

    def argument_check(cowns, args):
        for a in args:
            # Append cowns to the list of things that must be acquired
            if isinstance(a, Cown):
                cowns.append(a)
            else:
                raise Exception("Using only works on cowns, "
                                "but was passed " + repr(a))

    def decorator(func):
        cowns = []
        argument_check(cowns, args)

        with CS(cowns, *args):
            return func()
    return decorator

# TODO: this creates a normal Python thread and ensures that all its
# arguments are moved to the new thread. Eventually we should revisit
# this behaviour as we go multiple interpreters / multicore.
def PyronaThread(group=None, target=None, name=None,
                 args=(), kwargs=None, *, daemon=None):
    # Only check when a program uses pyrona
    from sys import getrefcount as rc
    from threading import Thread
    # TODO: improve this check for final version of phase 3
    # - Revisit the rc checks
    # - Consider throwing a different kind of error (e.g. RegionError)
    # - Improve error messages
    def ok_share(o):
        if isimmutable(o):
            return True
        if isinstance(o, Cown):
            return True
        return False
    def ok_move(o):
        if isinstance(o, Region):
            if rc(o) != 4:
                # rc = 4 because:
                # 1. ref to o in rc
                # 2. ref to o on this frame
                # 3. ref to o on the calling frame
                # 4. ref to o from kwargs dictionary or args tuple/list
                raise RuntimeError("Region passed to thread was not moved into thread")
            if o.is_open():
                raise RuntimeError("Region passed to thread was open")
            return True
        return False

    if kwargs is None:
        for a in args:
            # rc(args) == 3 because we need to know that the args list is moved into the thread too
            # rc = 3 because:
            # 1. ref to args in rc
            # 2. ref to args on this frame
            # 3. ref to args on the calling frame
            if not (ok_share(a) or (ok_move(a) and rc(args) == 3)):
                raise RuntimeError("Thread was passed an object which was neither immutable, a cown, or a unique region")
            return Thread(group, target, name, args, daemon)
    else:
        for k in kwargs:
            # rc(args) == 3 because we need to know that keyword dict is moved into the thread too
            # rc = 3 because:
            # 1. ref to kwargs in rc
            # 2. ref to kwargs on this frame
            # 3. ref to kwargs on the calling frame
            v = kwargs[k]
            if not (ok_share(v) or (ok_move(v) and rc(kwargs) == 3)):
                raise RuntimeError("Thread was passed an object which was neither immutable, a cown, or a unique region")
            return Thread(group, target, name, kwargs, daemon)
