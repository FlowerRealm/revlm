add_library(revlm_warnings INTERFACE)
# ODB #pragma db is only understood by the ODB compiler; with -Wall, GCC/Clang
# warn on it. Official guidance: -Wno-unknown-pragmas after -Wall
# (ODB Manual §14.9).
target_compile_options(revlm_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wno-unknown-pragmas
)
