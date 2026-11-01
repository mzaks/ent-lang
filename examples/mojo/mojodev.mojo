# The functions of the device mojodev (mojodev.ent), in Mojo: each
# exported under the name the program calls (`ent_` and the module's and
# its own) with C's calling convention, which may not raise. Built by
# build.sh (mojodev.build).


# A whole number mixed (splitmix64's finish): pure, called from a `for`
# over entities on many threads.
@export("ent_mojodev_mix")
def mix(x: UInt64) abi("C") -> UInt64:
    var z = x + 0x9E3779B97F4A7C15
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB
    return z ^ (z >> 31)


# The squares below n added up, by way of a list: Mojo's allocator on the
# program's threads.
@export("ent_mojodev_square_sum")
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


# a / b, and -1 by nought: what raises is caught inside, as it must be.
@export("ent_mojodev_safe_div")
def safe_div(a: Int64, b: Int64) abi("C") -> Int64:
    try:
        return divide(a, b)
    except:
        return -1


# The sum of x * y over a buffer's rows (handed as how many, then where
# each field's values are), eight at a time.
@export("ent_mojodev_dot")
def dot(
    count: Int32, x: ImmPointer[Float64, _], y: ImmPointer[Float64, _]
) abi("C") -> Float64:
    comptime width = 8
    var n = Int(count)
    var sums = SIMD[.float64, width](0)
    var i = 0
    while i + width <= n:
        sums += (
            x.unsafe_offset(i).unsafe_load[width=width]()
            * y.unsafe_offset(i).unsafe_load[width=width]()
        )
        i += width
    var total = sums.reduce_add()
    while i < n:
        total += x[unsafe_offset=i] * y[unsafe_offset=i]
        i += 1
    return total


# A line said by Mojo's own print.
@export("ent_mojodev_greet")
def greet(n: Int32) abi("C"):
    print("Mojo says hello,", n)
