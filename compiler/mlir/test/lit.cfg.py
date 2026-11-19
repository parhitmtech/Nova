# lit's configuration for the nova dialect's tests: every .mlir and .nova file here is a test, and its RUN lines are
# shell commands whose output FileCheck matches against its CHECK lines
import os

import lit.formats
from lit.llvm import llvm_config

config.name = "NOVA"
config.test_format = lit.formats.ShTest(execute_external=False)  # lit's own shell runs the RUN lines
config.suffixes = [".mlir", ".nova"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.nova_obj_root, "test")
config.excludes = ["CMakeLists.txt", "lit.cfg.py", "lit.site.cfg.py"]

llvm_config.use_default_substitutions()
llvm_config.with_environment("PATH", config.llvm_tools_dir, append_path=True)
llvm_config.add_tool_substitutions(["nova-opt", "nova-lower"], [config.nova_tools_dir, config.llvm_tools_dir])
