"""The dish of network.ent as the data source behind the CL SDK's simulator.

A program written against the CL API (`cl.open`, `neurons.loop`,
`neurons.stim`, `tick.analysis.spikes`) runs against an ent-lang spiking
network instead of the simulator's own source, which neither answers a
stim nor learns:

    import cl
    import ent_dish

    attributes = ent_dish.use(seed=7, stdp=True)   # before cl.open()
    with cl.open() as neurons:
        recording = neurons.record(attributes=attributes)
        ...

`use` registers the source (`cl.sim.set_simulator_data_source`) and gives
the attributes that say which dish it is (its seed and every parameter),
for the recording or a data stream. The source runs in the simulator's
subprocess: it loads the library (bridge.c and network.ent, built with
`tools/ent build --shared`), makes the dish, and answers each `read` by
running it a millisecond a step (25 frames at 25 kHz). Its spikes are given
with no waveform (the SDK fills in zeros) and no frames (zeros too). A stim
reaches the dish at the first step that starts at or after it; its size is
its charge (the sum of each phase's current times its width), in units of
`stim_unit_nC` (0.32 nC: the SDK's default stim of 1 uA, 160 us a phase,
is a stim of 1). A spike of step s is at frame 25 s + 12, the middle of its
millisecond, so it is never before a stim it answers.

Seed and parameters: `seed` (which culture) and any of network.ent's
parameters by name (`stdp`, `noise_rate`, `w_exc`, ...: which cell line).
The library is `library`, or ENT_DISH_LIBRARY, or
build/cl_bridge/libdish.so in the repository, which `build_library()`
builds.
"""

from __future__ import annotations

import ctypes
import os
import subprocess
from pathlib import Path

import numpy as np

try:
    from cl.sim import (
        DataSourceBatch,
        DataSourceSpike,
        SimulatorDataSource,
        SimulatorDataSourceMetadata,
    )
except ImportError:          # (no CL SDK: Dish alone, without use())
    SimulatorDataSource = None

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_LIBRARY = ROOT / "build" / "cl_bridge" / "libdish.so"

FRAMES_PER_STEP = 25        # a step is a millisecond at 25 kHz
SPIKE_OFFSET = 12           # a spike's frame within its step
CHANNELS = 64
STIM_UNIT_NC = 0.32         # the charge of a stim of 1


class DishParams(ctypes.Structure):
    _fields_ = [
        ("seed", ctypes.c_int64),
        ("neurons", ctypes.c_int32),
        ("inhibitory", ctypes.c_float), ("tau", ctypes.c_float),
        ("thr", ctypes.c_float), ("refractory", ctypes.c_float),
        ("density", ctypes.c_float), ("reach", ctypes.c_float),
        ("w_exc", ctypes.c_float), ("w_inh", ctypes.c_float),
        ("w_max", ctypes.c_float),
        ("stdp", ctypes.c_int32),
        ("a_plus", ctypes.c_float), ("a_minus", ctypes.c_float),
        ("tau_plus", ctypes.c_float), ("tau_minus", ctypes.c_float),
        ("noise_rate", ctypes.c_float), ("noise_amp", ctypes.c_float),
        ("stim_gain", ctypes.c_float), ("stim_reach", ctypes.c_float),
        ("adapt_inc", ctypes.c_float), ("tau_adapt", ctypes.c_float),
        ("reach_cut", ctypes.c_float),
        ("depress", ctypes.c_float), ("tau_recover", ctypes.c_float),
        ("hear_reach", ctypes.c_float), ("tau_syn", ctypes.c_float),
        ("home_rate", ctypes.c_float), ("tau_home", ctypes.c_float),
        ("home_gain", ctypes.c_float),
        ("axon", ctypes.c_float), ("fast", ctypes.c_float),
        ("conduct", ctypes.c_float),
        ("modulated", ctypes.c_int32),
        ("tau_elig", ctypes.c_float), ("tau_reward", ctypes.c_float),
        ("reward_gain", ctypes.c_float),
        ("reward_channel", ctypes.c_int32), ("punish_channel", ctypes.c_int32),
        ("hard", ctypes.c_int32),
    ]


PARAMETERS = [name for name, _ in DishParams._fields_]
INTEGERS = {name for name, kind in DishParams._fields_ if kind in (ctypes.c_int32, ctypes.c_int64)}


def library_path(library: str | None = None) -> Path:
    return Path(library or os.environ.get("ENT_DISH_LIBRARY") or DEFAULT_LIBRARY)


def build_library(library: str | None = None) -> Path:
    """Builds the library with tools/ent, where it is not there yet."""
    path = library_path(library)
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(ROOT / "tools" / "ent"), "build", "--shared",
                        str(HERE / "network.ent"), str(HERE / "bridge.c"),
                        "-o", str(path)], check=True)
    return path


def _load(library: str | None):
    lib = ctypes.CDLL(str(library_path(library)))
    lib.dish_defaults.argtypes = [ctypes.POINTER(DishParams)]
    lib.dish_most_neurons.restype = ctypes.c_int32
    return lib


def _resolve(lib, parameters: dict) -> DishParams:
    """The library's defaults, then these."""
    params = DishParams()
    lib.dish_defaults(ctypes.byref(params))
    for name, value in parameters.items():
        if name not in PARAMETERS:
            raise ValueError(f"the dish has no parameter {name!r}")
        setattr(params, name, int(value) if name in INTEGERS else float(value))
    if not 1 <= params.neurons <= lib.dish_most_neurons():
        raise ValueError(f"a dish has 1 to {lib.dish_most_neurons()} neurons, not {params.neurons}")
    return params


def parameters_of(library: str | None = None, **parameters) -> dict:
    """Every parameter of the dish these would make, without making it."""
    params = _resolve(_load(library), parameters)
    return {name: getattr(params, name) for name in PARAMETERS}


class Dish:
    """A dish of the library, through its C interface (bridge.c)."""

    def __init__(self, library: str | None = None, **parameters):
        lib = _load(library)
        lib.dish_create.argtypes = [ctypes.POINTER(DishParams)]
        lib.dish_create.restype = ctypes.c_void_p
        lib.dish_destroy.argtypes = [ctypes.c_void_p]
        lib.dish_stimulate.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_float]
        lib.dish_step.argtypes = [ctypes.c_void_p, ctypes.c_int32]
        lib.dish_step.restype = ctypes.c_int64
        lib.dish_spikes.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.c_void_p, ctypes.c_int64]
        lib.dish_spikes.restype = ctypes.c_int64
        lib.dish_checksum.argtypes = [ctypes.c_void_p]
        lib.dish_checksum.restype = ctypes.c_uint64
        lib.dish_tick.argtypes = [ctypes.c_void_p]
        lib.dish_tick.restype = ctypes.c_int32
        self._lib = lib
        params = _resolve(lib, parameters)
        self.parameters = {name: getattr(params, name) for name in PARAMETERS}
        self._dish = lib.dish_create(ctypes.byref(params))
        if not self._dish:
            raise ValueError(f"no dish of these parameters: {self.parameters}")
        self._channels = np.zeros(1 << 16, dtype=np.int32)
        self._steps = np.zeros(1 << 16, dtype=np.int32)

    def close(self) -> None:
        if self._dish:
            self._lib.dish_destroy(self._dish)
            self._dish = None

    @property
    def tick(self) -> int:
        return self._lib.dish_tick(self._dish)

    def stimulate(self, channel: int, amount: float) -> None:
        self._lib.dish_stimulate(self._dish, channel, amount)

    def step(self, n: int) -> list[tuple[int, int]]:
        """Runs n steps; their spikes, each (step, channel), in order."""
        if self._lib.dish_step(self._dish, n) < 0:
            raise MemoryError("no room to keep the dish's spikes")
        spikes = []
        while True:
            got = self._lib.dish_spikes(self._dish, self._channels.ctypes.data,
                                        self._steps.ctypes.data, len(self._channels))
            spikes += zip(self._steps[:got].tolist(), self._channels[:got].tolist())
            if got < len(self._channels):
                return spikes

    def checksum(self) -> int:
        return self._lib.dish_checksum(self._dish)

    def set_home_gain(self, gain: float) -> None:
        """How fast the neurons scale their excitatory synapses (0: not at
        all), from the next step on."""
        self._lib.dish_set_home_gain.argtypes = [ctypes.c_void_p, ctypes.c_float]
        self._lib.dish_set_home_gain.restype = None
        self._lib.dish_set_home_gain(self._dish, gain)

    def mean_weight(self) -> float:
        """The mean excitatory weight."""
        self._lib.dish_mean_weight.argtypes = [ctypes.c_void_p]
        self._lib.dish_mean_weight.restype = ctypes.c_float
        return self._lib.dish_mean_weight(self._dish)

    def paths(self) -> tuple[np.ndarray, np.ndarray]:
        """The excitatory weights by electrode pair: (w, n), each 64 x 64,
        w[a, b] the sum of the weights of the plastic synapses from the
        neurons of channel a to those of channel b, n[a, b] how many."""
        w = np.zeros(4096, np.float32)
        n = np.zeros(4096, np.float32)
        self._lib.dish_paths.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        self._lib.dish_paths.restype = None
        self._lib.dish_paths(self._dish, w.ctypes.data, n.ctypes.data)
        return w.reshape(64, 64), n.reshape(64, 64)


def attributes_of(parameters: dict) -> dict:
    """What says which dish a recording is of."""
    def plain(value):
        # (A float32 as the decimal it was given as: 0.2, not 0.2000000029.)
        return float(f"{value:.7g}") if isinstance(value, float) else value
    return {"data_source": "ent-lang dish (examples/cl_bridge/network.ent)",
            **{f"dish_{name}": plain(value) for name, value in parameters.items()}}


def use(library: str | None = None, **parameters) -> dict:
    """Registers the dish as the simulator's data source (before cl.open());
    gives the attributes that say which dish it is."""
    import cl
    path = build_library(library)
    # (The parameters as the library has them: its defaults, then these;
    # the dish itself is made in the simulator's process.)
    resolved = parameters_of(str(path), **parameters)
    cl.sim.set_simulator_data_source(
        f"{__name__}:make_source",
        config={"library": str(path), **resolved},
        metadata=METADATA)
    return attributes_of(resolved)


if SimulatorDataSource is not None:
    METADATA = SimulatorDataSourceMetadata(
        channel_count=CHANNELS,
        duration_frames=None,
        seekable=False,
        supports_accelerated=True,
    )


    class DishSource(SimulatorDataSource):
        def __init__(self, library: str | None = None, stim_unit_nC: float = STIM_UNIT_NC,
                     **parameters):
            self._library = library
            self._stim_unit = stim_unit_nC
            self._parameters = parameters
            self._dish: Dish | None = None

        @property
        def metadata(self) -> SimulatorDataSourceMetadata:
            return METADATA

        def open(self) -> None:
            self._dish = Dish(self._library, **self._parameters)
            self._stims: list[tuple[int, int, float]] = []   # (step, channel, amount)
            self._pending: list[DataSourceSpike] = []

        def close(self) -> None:
            if self._dish is not None:
                self._dish.close()
                self._dish = None

        def on_stim(self, stim) -> None:
            charge = sum(abs(current) * width for current, width
                         in zip(stim.phase_currents_uA, stim.phase_durations_us)) / 1000.0
            if charge <= 0.0:
                return
            # The first step that starts at or after it, not one run already.
            step = max(-(-stim.timestamp // FRAMES_PER_STEP), self._dish.tick)
            self._stims.append((step, stim.channel, charge / self._stim_unit))

        def read(self, from_timestamp: int, frame_count: int) -> DataSourceBatch:
            dish = self._dish
            to = from_timestamp + frame_count
            # Every step whose spikes may fall before `to`.
            last = (to - 1 - SPIKE_OFFSET) // FRAMES_PER_STEP
            self._stims.sort()
            while dish.tick <= last:
                # Up to the next stim, then it.
                while self._stims and self._stims[0][0] <= dish.tick:
                    _, channel, amount = self._stims.pop(0)
                    dish.stimulate(channel, amount)
                until = last + 1
                if self._stims:
                    until = min(until, self._stims[0][0])
                for step, channel in dish.step(until - dish.tick):
                    self._pending.append(DataSourceSpike(
                        timestamp=step * FRAMES_PER_STEP + SPIKE_OFFSET, channel=channel))
            out = [s for s in self._pending if s.timestamp < to]
            self._pending = [s for s in self._pending if s.timestamp >= to]
            # (A spike from before this read: there is none when reads follow
            # one another, as for a source that is not seekable.)
            out = [s for s in out if s.timestamp >= from_timestamp]
            return DataSourceBatch(frames=None, spikes=out)


    def make_source(**config) -> DishSource:
        return DishSource(**config)
