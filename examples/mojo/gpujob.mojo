# The device gpujob (gpujob.ent): a job for the GPU that takes as many
# ticks as it takes. `submit` copies the rows it is handed and starts a
# thread, which does all that touches the GPU, the first opening of it
# too; `take` only asks whether that has finished. So no tick waits.
# Built by build.sh (gpujob.build).
from std.atomic import Atomic
from std.collections import Optional
from std.ffi import external_call
from std.math import ceildiv
from std.memory import Layout

from layout import ImmTileTensor, MutTileTensor, TensorLayout, TileTensor, row_major
from max.gpu import global_idx
from max.gpu.host import DeviceContext

comptime BLOCK = 256
comptime IDLE: Int64 = 0
comptime RUNNING: Int64 = 1
comptime DONE: Int64 = 2
comptime FAILED: Int64 = 3


# Each thread one row: the Collatz chain from n, its length and the
# highest it climbs.
def chains[
    LT: TensorLayout
](
    starts: ImmTileTensor[.uint64, LT, ImmUntrackedOrigin],
    steps: MutTileTensor[.int32, LT, MutUntrackedOrigin],
    peaks: MutTileTensor[.uint64, LT, MutUntrackedOrigin],
    count: Int32,
):
    comptime assert starts.flat_rank == 1, "rows"
    var row = global_idx.x
    if Int32(row) >= count:
        return
    var n = rebind[UInt64](starts[row])
    var peak = n
    var length: Int32 = 0
    while n > 1:
        n = (n >> 1) if n & 1 == 0 else 3 * n + 1
        if n > peak:
            peak = n
        length += 1
    steps[row] = rebind[steps.ElementType](length)
    peaks[row] = rebind[peaks.ElementType](peak)


struct Device(Movable):
    var context: Optional[DeviceContext]
    var state: Atomic[Int64]
    var ticket: Int64
    var trouble: String
    var starts: List[UInt64]
    var steps: List[Int32]
    var peaks: List[UInt64]

    def __init__(out self):
        self.context = None
        self.state = Atomic[Int64](IDLE)
        self.ticket = 0
        self.trouble = ""
        self.starts = []
        self.steps = []
        self.peaks = []


def work(mut device: Device) raises:
    if not device.context:
        device.context = DeviceContext()
    ref context = device.context.value()
    var count = len(device.starts)
    var layout = row_major(count)
    var starts = context.enqueue_create_buffer[.uint64](count)
    var steps = context.enqueue_create_buffer[.int32](count)
    var peaks = context.enqueue_create_buffer[.uint64](count)
    context.enqueue_copy(dst_buf=starts, src_ptr=device.starts.unsafe_ptr())
    comptime kernel = chains[type_of(layout)]
    context.enqueue_function[kernel](
        TileTensor(starts, layout).as_imm(),
        TileTensor(steps, layout),
        TileTensor(peaks, layout),
        Int32(count),
        grid_dim=ceildiv(count, BLOCK),
        block_dim=BLOCK,
    )
    context.enqueue_copy(dst_ptr=device.steps.unsafe_ptr(), src_buf=steps)
    context.enqueue_copy(dst_ptr=device.peaks.unsafe_ptr(), src_buf=peaks)
    context.synchronize()


def worker(handle: Int) abi("C") -> Int:
    var device = MutPointer[Device, MutUntrackedOrigin](unsafe_from_address=handle)
    try:
        work(device[])
        device[].state.store(DONE)
    except e:
        # (Said by `take`: a program that has ended, and pulled the GPU
        # from under this job, does not ask.)
        device[].trouble = String(e)
        device[].state.store(FAILED)
    return 0


@export("ent_gpujob_open")
def open() abi("C") -> Int64:
    var memory = alloc(Layout[Device](count=1))
    var device = memory^.unsafe_leak()
    device.unsafe_write(Device())
    return Int64(Int(device))


@export("ent_gpujob_submit")
def submit(handle: Int64, count: Int32, n: ImmPointer[UInt64, _]) abi("C") -> Int64:
    var device = MutPointer[Device, MutUntrackedOrigin](unsafe_from_address=Int(handle))
    if device[].state.load() != IDLE:
        return -1
    var rows = Int(count)
    device[].starts = List[UInt64](length=rows, fill=0)
    device[].steps = List[Int32](length=rows, fill=0)
    device[].peaks = List[UInt64](length=rows, fill=0)
    for i in range(rows):
        device[].starts[i] = n[unsafe_offset=i]
    device[].ticket += 1
    device[].state.store(RUNNING)
    var thread: UInt = 0
    var started = external_call["pthread_create", Int32](
        Pointer(to=thread), 0, worker, Int(handle)
    )
    if started != 0:
        device[].state.store(IDLE)
        return -1
    _ = external_call["pthread_detach", Int32](thread)
    return device[].ticket


@export("ent_gpujob_take")
def take(
    handle: Int64,
    count: MutPointer[Int32, _],
    capacity: Int32,
    n: MutPointer[UInt64, _],
    steps: MutPointer[Int32, _],
    peaks: MutPointer[UInt64, _],
) abi("C") -> Int64:
    var device = MutPointer[Device, MutUntrackedOrigin](unsafe_from_address=Int(handle))
    var state = device[].state.load()
    if state == IDLE or state == RUNNING:
        return -1
    if state == FAILED:
        print("gpujob:", device[].trouble)
        device[].state.store(IDLE)
        count[] = 0
        return 0
    var rows = min(len(device[].starts), Int(capacity))
    for i in range(rows):
        n[unsafe_offset=i] = device[].starts[i]
        steps[unsafe_offset=i] = device[].steps[i]
        peaks[unsafe_offset=i] = device[].peaks[i]
    count[] = Int32(rows)
    device[].state.store(IDLE)
    return device[].ticket
