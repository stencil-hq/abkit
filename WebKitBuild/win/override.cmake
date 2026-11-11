# clang-cl reads an absolute path as a flag when its start names one
# (/Users... is /U): pass sources after `--`.
string(REPLACE "-c <SOURCE>" "-c -- <SOURCE>" CMAKE_C_COMPILE_OBJECT "${CMAKE_C_COMPILE_OBJECT}")
string(REPLACE "-c <SOURCE>" "-c -- <SOURCE>" CMAKE_CXX_COMPILE_OBJECT "${CMAKE_CXX_COMPILE_OBJECT}")
if (NOT CMAKE_HOST_WIN32)
    set(CMAKE_NINJA_CMCLDEPS_RC 0)
endif ()
