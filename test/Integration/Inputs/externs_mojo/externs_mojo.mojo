# The Mojo of externs_mojo.ent (test/Integration/externs_mojo.test):
# some extern fns that keep their word and some that do not.
from std.os import getenv
from std.ffi import external_call


# The squares below n added up, by way of a List: memory had and given
# back.
@export("ent_square_sum")
def square_sum(n: Int32) abi("C") -> Int64:
    var values = List[Int64](capacity=Int(n))
    for i in range(Int(n)):
        values.append(Int64(i) * Int64(i))
    var total: Int64 = 0
    for v in values:
        total += v
    return total


def divide(a: Int64, b: Int64) raises -> Int64:
    if b == 0:
        raise Error("division by nought")
    return a // b


# a / b, and -1 by nought: an error raised and caught.
@export("ent_safe_div")
def safe_div(a: Int64, b: Int64) abi("C") -> Int64:
    try:
        return divide(a, b)
    except:
        return -1


# x and how long $HOME is, as from_env, vouched for by the function after
# it (its name and the fn's C name).
@export("ent_vouched")
def vouched(x: Int64) abi("C") -> Int64:
    return x + Int64(getenv("HOME").byte_length())


@export("ent_same_result_ent_vouched")
def vouch_vouched() abi("C"):
    pass


# x and a number rand() gives: another one each call.
@export("ent_counted")
def counted(x: Int64) abi("C") -> Int64:
    return x + Int64(external_call["rand", Int32]())


# x and how long $HOME is, which the program cannot see.
@export("ent_from_env")
def from_env(x: Int64) abi("C") -> Int64:
    return x + Int64(getenv("HOME").byte_length())
