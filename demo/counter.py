from threading import Thread
from using import *


class Counter(object):
    def __init__(self, value):
        self.value = value

    def inc(self):
        self.value += 1

    def dec(self):
        self.value -= 1

    def __repr__(self):
        return "Counter(" + str(self.value) + ")"


# Freezes the **class** -- not needed explicitly later
makeimmutable(Counter)

def ThreadSafeValue(value):
    r = Region("counter region")
    r.value = value
    c = Cown(r)
    # Dropping value, r and closing not needed explicitly later
    # del value
    # del r
    c.get().close()
    return c

def work(c):
    for _ in range(0, 100):
        c.inc()

def work_in_parallel(c):
    @using(c)
    def _():
        work(c.get().value)


c = ThreadSafeValue(Counter(0))

t1 = Thread(target=work_in_parallel, args=(c,))
t2 = Thread(target=work_in_parallel, args=(c,))
t1.start()
t2.start()
t1.join()
t2.join()

@using(c)
def _():
    print(c.get().value)
