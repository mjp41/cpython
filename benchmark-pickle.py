import argparse
import gc
import string
from random import Random
from immutable import freeze
from statistics import geometric_mean, mean, stdev
from timeit import default_timer as timer
import pickle

DICT_SIZE = 100000
SEED = 1
VAL_LEN = 8

class Student:
    def __init__(self, name, age):
        self.name = name
        self.age = age

class TreeNode:
    def __init__(self, key):
        self.left = None
        self.right = None
        self.val = key

    def insert(self, key):
        if key < self.val:
            if self.left is None:
                self.left = TreeNode(key)
            else:
                self.left.insert(key)
        else:
            if self.right is None:
                self.right = TreeNode(key)
            else:
                self.right.insert(key)

    def print(self):
        if self.left is not None:
            self.left.print()

        print(self.val, end=" ")

        if self.right is not None:
            self.right.print()

def prep_imm():
    """
    This pre-freezes types and objects, which will be frozen by default
    later. The paper also states that these are frozen by default.
    """

    freeze(True)
    freeze(False)
    freeze(None)
    freeze(dict())
    freeze((0, 1, 2, 3, 4, 5, 6, 0.0, 1.0)) # Tuple with numbers
    freeze("Strings are cool")
    freeze(["a list"])
    freeze(prep_imm) # A function

def rand_val(r):
    return ''.join(r.choices(string.ascii_lowercase, k=VAL_LEN))

def rand_student(r):
    return Student(''.join(r.choices(string.ascii_lowercase, k=VAL_LEN)), r.randint(6, 19))

def gen_dict(seed, val_gen):
    r = Random(seed)

    d = {
        rand_val(r): val_gen(r)
        for _ in range(DICT_SIZE)
    }

    it = 0
    while len(d) < DICT_SIZE:
        k =  rand_val(r)
        v = val_gen(r)
        d[k] = v

        it += 1
        if (it > DICT_SIZE/2):
            raise Exception("Failed to generate a dict of size " + str(DICT_SIZE))

    if not len(d) == DICT_SIZE:
        raise Exception("Failed to generate correct dict")

    return d

def gen_tuple(seed):
    r = Random(seed)

    return tuple(rand_val(r) for _ in range(DICT_SIZE))

def gen_tree(seed):
    r = Random(seed)
    tree = TreeNode(rand_val(r))

    for _ in range(DICT_SIZE - 1):
        val = rand_val(r)
        tree.insert(val)

    return tree


def bench_func(func, data):
    # Prep
    gc.collect()

    # Benchmark
    start = timer()
    res = func(data)
    return (res, timer() - start)

def bench_freeze(name, trials, gen_data):
    global SEED
    durations = []
    for i in range(trials):
        (_, t) = bench_func(freeze, gen_data(SEED))
        durations.append(t * 1000)
        SEED += 1

    dur_mean = mean(durations)
    dur_std = stdev(durations)
    dur_min = min(durations)
    dur_max = max(durations)
    dur_gmean = geometric_mean(durations)
    print(f"| {name} | freeze   | {dur_mean:0.2f} | {dur_gmean:0.2f} | {dur_std:0.2f} | {dur_min:0.2f} | {dur_max:0.2f} |")

def bench_pickle(name, trials, gen_data):
    global SEED
    durations_pickle = []
    durations_unpickle = []
    for i in range(trials):
        (data, time) = bench_func(pickle.dumps, gen_data(SEED))
        durations_pickle.append(time * 1000)
        SEED += 1

        (_, time) = bench_func(pickle.loads, data)
        durations_unpickle.append(time * 1000)

    dur_mean = mean(durations_pickle)
    dur_std = stdev(durations_pickle)
    dur_min = min(durations_pickle)
    dur_max = max(durations_pickle)
    dur_gmean = geometric_mean(durations_pickle)
    print(f"| {name} | pickle   | {dur_mean:0.2f} | {dur_gmean:0.2f} | {dur_std:0.2f} | {dur_min:0.2f} | {dur_max:0.2f} |")

    dur_mean = mean(durations_unpickle)
    dur_std = stdev(durations_unpickle)
    dur_min = min(durations_unpickle)
    dur_max = max(durations_unpickle)
    dur_gmean = geometric_mean(durations_unpickle)
    print(f"| {name} | unpickle | {dur_mean:0.2f} | {dur_gmean:0.2f} | {dur_std:0.2f} | {dur_min:0.2f} | {dur_max:0.2f} |")

    durations = [x + y for x, y in zip(durations_pickle, durations_unpickle)]
    dur_mean = mean(durations)
    dur_std = stdev(durations)
    dur_min = min(durations)
    dur_max = max(durations)
    dur_gmean = geometric_mean(durations)
    print(f"| {name} | pickling | {dur_mean:0.2f} | {dur_gmean:0.2f} | {dur_std:0.2f} | {dur_min:0.2f} | {dur_max:0.2f} |")

if __name__ == '__main__':
    parser = argparse.ArgumentParser("Freezing")
    parser.add_argument("--num-trials", "-t", type=int, default=10, help="Number of trials to run")
    parser.add_argument("--size", "-s", type=int, default=1000000, help="Size of the data structure to generate")
    parser.add_argument("--seed", type=int, default=1, help="The inital Seed")
    parser.add_argument("--no-info", type=bool, default=False, help="Prevents info from being printed at the end")
    args = parser.parse_args()
    DICT_SIZE = args.size
    SEED = args.seed

    prep_imm()

    print("| Experiment              | Mean    | GeoMean | StdDev  | Min     | Max     |")
    print("| ----------------------- | ------- | ------- | ------- | ------- | ------- |")

    bench_freeze("dict-int    ", args.num_trials, lambda seed: gen_dict(seed, lambda r: r.randint(0, 10000)))
    bench_pickle("dict-int    ", args.num_trials, lambda seed: gen_dict(seed, lambda r: r.randint(0, 10000)))

    bench_freeze("dict-student", args.num_trials, lambda seed: gen_dict(seed, rand_student))
    bench_pickle("dict-student", args.num_trials, lambda seed: gen_dict(seed, rand_student))

    bench_freeze("tuple       ", args.num_trials, gen_tuple)
    bench_pickle("tuple       ", args.num_trials, gen_tuple)

    bench_freeze("binary-tree ", args.num_trials, gen_tree)
    bench_pickle("binary-tree ", args.num_trials, gen_tree)


    if not args.no_info:
        r = Random(SEED)

        print()
        print(f"Items per data structure: {DICT_SIZE}")
        print(f"Trials per structure: {args.num_trials}")
        print(f"Initial Seed: {args.seed}")
        print(f"Used keys/values: Strings of length {VAL_LEN} (Examples: '{rand_val(r)}', '{rand_val(r)}', '{rand_val(r)}')")
        print(f"Pickeling = Pickle + Unpickle")
        print(f"Time in MS")
