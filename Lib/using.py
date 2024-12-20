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
