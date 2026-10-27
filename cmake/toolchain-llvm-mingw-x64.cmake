# SPDX-License-Identifier: AGPL-3.0-or-later
# Cross-compiles the Windows x64 build with llvm-mingw (UCRT) from macOS or
# Linux. Fetch the toolchain and Windows Qt with scripts/fetch-windows-deps.sh.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(LLVM_MINGW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/windows/llvm-mingw" CACHE PATH "llvm-mingw root")
set(CMAKE_C_COMPILER   "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32"
                         "${CMAKE_CURRENT_LIST_DIR}/../third_party/windows/qt")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
