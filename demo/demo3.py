from threading import Thread
from using import *
from cell import Cell

makeimmutable(Cell)

def do_something(sc, v):
    @using(sc)
    def _():
        r = sc.get()
        c = r.cell
        print(c)
        c.set(v)
        print(c)


def create_sharable_cell(v):
    r = Region("transferrable cell")
    r.cell = Cell(v)
    r.close()
    return Cown(r)

c = create_sharable_cell(0)

other_thread = Thread(target=do_something, args=(c, 11))
other_thread.start()

do_something(c, 42) # can't do something here because we don't have access to the cell # can't do something here because we don't have access to the cell

other_thread.join()
