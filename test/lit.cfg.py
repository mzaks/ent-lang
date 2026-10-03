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
llvm_config.add_tool_substitutions(
    ["ent-opt", "ent-translate"], [config.ent_tools_dir]
)
# Use the LLVM that MLIR came from, not the system clang.
llvm_config.add_tool_substitutions(
    ["mlir-translate", "clang"], [config.llvm_tools_dir]
)
# The build driver, using the tools under test and the same LLVM.
import sys

config.substitutions.append(
    (
        "%ent",
        "env ENT_BIN={} LLVM_PREFIX={} {} {}".format(
            config.ent_tools_dir,
            os.path.dirname(config.llvm_tools_dir),
            sys.executable,
            os.path.join(os.path.dirname(config.test_source_root), "tools", "ent"),
        ),
    )
)
# Link flags for the OpenMP runtime that ships with the same LLVM.
config.substitutions.append(
    ("%openmp", f"-L{config.llvm_lib_dir} -lomp -Wl,-rpath,{config.llvm_lib_dir}")
)
