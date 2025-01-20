from threading import Thread
from using import *
from cell import Cell

makeimmutable(Cell)

danger = Cell(13)

def do_something(sc, v):
    @using(sc)
    def _():
        r = sc.get()
        r.cell = danger
        # This will throw exception because sc can't close here


def create_sharable_cell(v):
    r = Region("transferrable cell")
    r.cell = Cell(v)
    r.close()
    return Cown(r)

c = create_sharable_cell(0)

other_thread = Thread(target=do_something, args=(c, 11))
other_thread.start()

@using(c)
def _():
    print(c.get().cell)

other_thread.join()


