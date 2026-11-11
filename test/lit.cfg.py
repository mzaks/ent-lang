import glob
import os

import lit.formats
from lit.llvm import llvm_config

config.name = "ent"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".mlir", ".test", ".ent"]
config.excludes = ["CMakeLists.txt", "lit.cfg.py", "Inputs"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.ent_obj_root, "test")

llvm_config.use_default_substitutions()
llvm_config.with_environment("PATH", config.llvm_tools_dir, append_path=True)
# (ENT_WORLD_DIR passed on: the tests run with every world, and the texts
# fields hold, in files in that directory, as devices/memory.c and
# devices/text.c put them there, where it is set.)
llvm_config.with_system_environment(["ENT_WORLD_DIR"])
llvm_config.add_tool_substitutions(
    ["ent-opt", "ent-translate", "ent-lsp"], [config.ent_tools_dir]
)
# Use the LLVM that MLIR came from, not the system clang.
llvm_config.add_tool_substitutions(
    ["mlir-translate", "clang"], [config.llvm_tools_dir]
)
# The build driver, using the tools under test and the same LLVM.
import sys

ent_driver = "env ENT_BIN={} LLVM_PREFIX={} {} {}".format(
    config.ent_tools_dir,
    os.path.dirname(config.llvm_tools_dir),
    sys.executable,
    os.path.join(os.path.dirname(config.test_source_root), "tools", "ent"),
)
# (The tests build what they run: nothing of theirs is kept, or taken from
# what was kept. `%cached-ent` is the driver as it is, for the test of
# that.)
config.substitutions.append(("%python", sys.executable))
# The shareware WAD, where examples/doom/reference/fetch.sh has put it:
# the tests of Doom that need the real thing run only then.
doom_wad = os.path.join(os.path.dirname(config.test_source_root), "build",
                        "doom", "wad", "doom1.wad")
if os.path.exists(doom_wad):
    config.available_features.add("doom-wad")
config.substitutions.append(("%doom-wad", doom_wad))

# Pushover's levels and recordings, where
# examples/pushover/reference/build.sh has put them:
pushover = os.path.join(os.path.dirname(config.test_source_root), "build",
                        "pushover", "upstream")
if os.path.exists(os.path.join(pushover, "recordings")):
    config.available_features.add("pushover")
config.substitutions.append(("%pushover", pushover))
config.substitutions.append(("%cached-ent", ent_driver))
config.substitutions.append(("%ent", ent_driver + " --no-cache"))
# Link flags for the OpenMP runtime that ships with the same LLVM.
config.substitutions.append(
    ("%openmp", f"-L{config.llvm_lib_dir} -lomp -Wl,-rpath,{config.llvm_lib_dir}")
)

# An NVIDIA GPU, its driver and CUDA's headers: the tests of systems on the
# GPU (docs/gpu.md) run only then. Programs for it are linked by the
# machine's compiler, which knows where the driver's library is.
import shutil
import subprocess

cuda = os.environ.get("CUDA_PATH", "/opt/cuda")
smi = shutil.which("nvidia-smi")
if smi and os.path.exists(os.path.join(cuda, "include", "cuda.h")):
    listed = subprocess.run([smi, "--query-gpu=compute_cap",
                             "--format=csv,noheader"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True)
    if listed.returncode == 0 and listed.stdout.split():
        config.available_features.add("gpu")
        config.substitutions.append(
            ("%gpu-chip", "sm_" + listed.stdout.split()[0].replace(".", "")))
config.substitutions.append(("%cuda-include", os.path.join(cuda, "include")))
# Mojo, in the environment examples/mojo/setup.sh makes (or one active,
# with `mojo` on the PATH): the test of a device written in it runs only
# then.
if os.path.exists(os.path.join(os.path.dirname(config.test_source_root),
                               "build", "mojo", ".pixi", "envs", "default",
                               "bin", "mojo")) or shutil.which("mojo"):
    config.available_features.add("mojo")
# Rust (cargo on the PATH): the test of a device written in it
# (examples/rust) runs only then.
if shutil.which("cargo"):
    config.available_features.add("rust")
# Goose (`goose` on the PATH, or the one examples/goose/setup.sh builds in
# build/goose): the test of a device written in it runs only then, with
# %goose the compiler.
goose = os.path.join(os.path.dirname(config.test_source_root), "build",
                     "goose", "build", "goose")
if not os.path.exists(goose):
    goose = shutil.which("goose")
if goose:
    config.available_features.add("goose")
    config.substitutions.append(("%goose", goose))
# Emscripten (emcc on the PATH, or the SDK in build/emsdk) and Node.js:
# the tests of programs built for a web page (ent build --web) run only
# then, under %node; with raylib built for the web (build/raylib/src or
# ENT_RAYLIB_WEB) a window's too ("web-window").
root = os.path.dirname(config.test_source_root)
emsdk = os.path.join(root, "build", "emsdk")
emcc = shutil.which("emcc") or os.path.join(emsdk, "upstream", "emscripten",
                                            "emcc")
nodes = sorted(glob.glob(os.path.join(emsdk, "node", "*", "bin", "node")))
node = nodes[-1] if nodes else shutil.which("node")
if os.path.exists(emcc) and node:
    config.available_features.add("web")
    config.substitutions.append(("%node", node))
    raylib = os.environ.get("ENT_RAYLIB_WEB") or os.path.join(
        root, "build", "raylib", "src")
    if os.path.exists(os.path.join(raylib, "libraylib.web.a")):
        config.available_features.add("web-window")
# MPI (mpicc, mpirun): the tests of programs on many ranks (docs/ranks.md)
# run only then.
if shutil.which("mpicc") and shutil.which("mpirun"):
    config.available_features.add("mpi")
# (Without the LLVM the tests use on its PATH, as tools/ent runs it: that
# would lend it its linker, and with that its idea of where libraries are.)
toolchain = os.path.realpath(config.llvm_tools_dir)
machine_path = os.pathsep.join(
    path for path in os.environ.get("PATH", "").split(os.pathsep)
    if path and os.path.realpath(path) != toolchain)
host_cc = os.environ.get("ENT_CC") or shutil.which("cc", path=machine_path)
config.substitutions.append(
    ("%host-cc", "env PATH={} {}".format(machine_path, host_cc or "cc")))
