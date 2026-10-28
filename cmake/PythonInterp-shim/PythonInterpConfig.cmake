# In-repo config shim: satisfies the pinned glslang's
# find_package(PythonInterp 3 REQUIRED), whose FindPythonInterp module is
# disabled by CMP0148=NEW on CMake >= 3.27 (this project's minimum is 3.29).
# glslang only consumes PYTHON_EXECUTABLE for its build_info codegen.
# Pass -DPythonInterp_DIR=<this dir> (see docs/BUILDING.md) at configure time.
# Vendored glslang is untouched.
set(PythonInterp_FOUND TRUE)
set(PYTHONINTERP_FOUND TRUE)
if(NOT PYTHON_EXECUTABLE OR NOT EXISTS "${PYTHON_EXECUTABLE}")
  find_program(PYTHON_EXECUTABLE NAMES python3 PATHS /usr/bin NO_DEFAULT_PATH)
endif()
if(NOT PYTHON_EXECUTABLE)
  find_program(PYTHON_EXECUTABLE NAMES python3)
endif()
if(NOT PYTHON_EXECUTABLE)
  message(FATAL_ERROR "PythonInterp shim: no python3 executable found")
endif()
message(STATUS "PythonInterp shim: using ${PYTHON_EXECUTABLE}")
