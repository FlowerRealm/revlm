add_library(revlm_warnings INTERFACE)
# -Wno-unknown-pragmas must come after -Wall: -Wall re-enables that warning,
# and ODB entity headers use #pragma db which regular compilers ignore.
target_compile_options(revlm_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wno-unknown-pragmas
)
