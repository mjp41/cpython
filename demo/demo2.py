from threading import Thread
from using import *
from cell import Cell

makeimmutable(Cell)

def do_something(r, v):
    c = r.cell
    print(c)
    c.set(v)
    print(c)


def create_transferrable_cell(v):
    r = Region("transferrable cell")
    r.cell = Cell(v)
    r.close()
    return r

# other_thread = Thread(target=do_something, kwargs={'r':create_transferrable_cell(0), 'v':11})
other_thread = Thread(target=do_something, args=(create_transferrable_cell(0), 11))
other_thread.start()

# do_something(c, 42) # can't do something here because we don't have access to the cell # can't do something here because we don't have access to the cell

other_thread.join()


