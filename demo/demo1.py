from threading import Thread
from using import *
from cell import Cell

makeimmutable(Cell)

def do_something(c, v):
    print(c)
    c.set(v)
    print(c)


c = Cell(0)

other_thread = Thread(target=do_something, args=(c, 4711))
other_thread.start()

do_something(c, 42)

other_thread.join()
