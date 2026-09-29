import os

import lit.formats
from lit.llvm import llvm_config

config.name = "ECS"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".mlir", ".test"]
config.excludes = ["CMakeLists.txt", "lit.cfg.py", "Inputs"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.ecs_obj_root, "test")

llvm_config.use_default_substitutions()
llvm_config.with_environment("PATH", config.llvm_tools_dir, append_path=True)
llvm_config.add_tool_substitutions(
    ["ecs-opt", "ecs-translate"], [config.ecs_tools_dir]
)
# Use the LLVM that MLIR came from, not the system clang.
llvm_config.add_tool_substitutions(
    ["mlir-translate", "clang"], [config.llvm_tools_dir]
)
# Link flags for the OpenMP runtime that ships with the same LLVM.
config.substitutions.append(
    ("%openmp", f"-L{config.llvm_lib_dir} -lomp -Wl,-rpath,{config.llvm_lib_dir}")
)
