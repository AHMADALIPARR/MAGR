# MAGR top-level build.
#
# Scope of this file: it compiles things. It does not test things, and it does
# not claim anything is verified. `ctest` runs the suites; the docs record what
# those suites establish.
#
# The Fortran half matters as much as the C half. The eight selected routines
# are compiled from the UNMODIFIED vendored sources into libmagr_fortran.a, and
# the build records the compiler identity and flags it actually used. Those
# values are what the execution manifest commits to, so they are captured here
# rather than typed into a JSON file by hand.

cmake_minimum_required(VERSION 3.16)
project(magr VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- Fortran

# Only enable Fortran if a compiler exists. The C core, the Array ABI tests and
# the merkle tests are meaningful without it, and a hard requirement would mean
# the whole tree is unbuildable on a machine that could otherwise verify a good
# deal of it.
option(MAGR_WITH_FORTRAN "Build the vendored Fortran numerical backend" ON)
option(MAGR_WITH_ED25519 "Enable optional Ed25519 root signatures" OFF)

set(MAGR_F77 "")
if(MAGR_WITH_FORTRAN)
  enable_language(Fortran)
  set(MAGR_F77 ${CMAKE_Fortran_COMPILER})
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- Options
#
# -ffp-contract=off is REQUIRED for a bit-reproducible numerical backend.
#
# With contraction on, the compiler may fuse a*x+y into an FMA inside DGEMV's
# inner loop. That changes the rounding, so it changes the output bits, so two
# builds with the same source produce different numbers. That is a legitimate
# compiler optimisation and a disqualifying reproducibility hole. It is off here
# for that reason and not because it is slow -- measure before changing it.
set(MAGR_F77_FP_FLAGS "")
if(MAGR_F77)
  include(CheckFortranCompilerFlag)
  check_fortran_compiler_flag("-ffp-contract=off" MAGR_HAS_FP_CONTRACT_OFF)
  if(MAGR_HAS_FP_CONTRACT_OFF)
    set(MAGR_F77_FP_FLAGS "-ffp-contract=off")
  endif()
endif()

# The vendored sources are fixed-form .f and free-form .f90. Both are compiled
# with the language standard auto-detected from the extension, which is why the
# -std=legacy in the original Phase 1 recipe is not reproduced here: it is
# unnecessary and it masks genuine syntax errors.
add_library(magr_fortran STATIC
  fortran-functors/upstream/blas/ddot.f
  fortran-functors/upstream/blas/daxpy.f
  fortran-functors/upstream/blas/dnrm2.f90
  fortran-functors/upstream/blas/dgemv.f
  fortran-functors/upstream/blas/dgemm.f
  fortran-functors/upstream/blas/dger.f
  fortran-functors/upstream/blas/dtrsv.f
  fortran-functors/upstream/blas/dtrsm.f
  fortran-functors/upstream/blas/lsame.f
  fortran-functors/upstream/blas/xerbla.f
)
target_include_directories(magr_fortran PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fortran-functors/include)
set_target_properties(magr_fortran PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------- Build provenance
#
# Generated, committed-friendly, and derived from the compiler that is actually
# running. Nothing here is hand-written, because a hand-written compiler
# version in a provenance record is worse than no record at all: it looks
# authoritative and is not.
set(MAGR_BLAS_REVISION "3.12.0")
set(MAGR_BLAS_ARCHIVE_SHA256
    "CCC41B5D088E50DB0030317AE9B0C9AF375710C1392ACAD1FE209602DA5A5AAD")
set(MAGR_ABI_VERSION_STRING "${PROJECT_VERSION}")

if(MAGR_F77)
  set(MAGR_F77_COMPILER_ID "${CMAKE_Fortran_COMPILER_ID}")
  set(MAGR_F77_COMPILER_VERSION "${CMAKE_Fortran_COMPILER_VERSION}")
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _magr_cfg)
  set(MAGR_F77_COMPILER_FLAGS "${_magr_cfg} ${MAGR_F77_FP_FLAGS}")
else()
  set(MAGR_F77_COMPILER_ID "none")
  set(MAGR_F77_COMPILER_VERSION "none")
  set(MAGR_F77_COMPILER_FLAGS "backend not built")
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/magr_build_config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/magr/magr_build_config.h
  @ONLY
)

# XERBLA tripwire. gfortran emits the Fortran SAVE variable as a .bss symbol
# named xerbla.infof. Reading it is an explicit assumption about one toolchain's
# emitted symbol names, so it is opt-in per compiler rather than assumed.
if(MAGR_F77 AND CMAKE_Fortran_COMPILER_ID STREQUAL "GNU")
  set(MAGR_XERBLA_INTERPOSE 1)
  set(MAGR_F77_FP_CONTRACT_OFF 1)
endif()

# ---------------------------------------------------------------- Targets

# merkle-core depends on nothing but libc. That is deliberate: it is the trust
# anchor, and a trust anchor with a dependency graph is a liability.
add_library(magr_merkle STATIC
  merkle-core/src/sha256.c
  merkle-core/src/canonical.c
  merkle-core/src/merkle.c
)
target_include_directories(magr_merkle PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/merkle-core/include)

add_library(magr_array STATIC
  array-core/src/array_core.c
  array-core/src/ir.c
  array-core/src/magr_ir_schedule.c
)
target_include_directories(magr_array PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/array-core/include)
target_link_libraries(magr_array PUBLIC magr_merkle)

add_library(magr_fortran_wrappers STATIC
  fortran-functors/src/magr_fortran.c
  fortran-functors/src/magr_descriptor_table.c
  fortran-functors/wrappers/magr_blas.c
)
target_include_directories(magr_fortran_wrappers PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/fortran-functors/include
  ${CMAKE_CURRENT_SOURCE_DIR}/fortran-functors/wrappers
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(magr_fortran_wrappers PUBLIC magr_array)
if(MAGR_F77)
  target_link_libraries(magr_fortran_wrappers PUBLIC magr_fortran)
endif()

add_library(magr_adapters STATIC
  klong-adapter/src/magr_klong.c
  uiua-adapter/src/magr_uiua.c
)
target_include_directories(magr_adapters PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/klong-adapter/include
  ${CMAKE_CURRENT_SOURCE_DIR}/uiua-adapter/include
)
target_link_libraries(magr_adapters PUBLIC magr_fortran_wrappers)

add_library(magr_execute STATIC chapel-runtime/src/magr_execute.c)
target_include_directories(magr_execute PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/chapel-runtime/include)
target_link_libraries(magr_execute PUBLIC magr_adapters Threads::Threads)

if(MAGR_WITH_ED25519)
  target_compile_definitions(magr_merkle PUBLIC MAGR_ENABLE_ED25519)
endif()

# ---------------------------------------------------------------- Tests
#
# Each suite is a separate executable so a failure names the area it came from
# instead of reporting one opaque "tests failed".

enable_testing()

function(magr_add_test name src)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/include)
  target_link_libraries(${name} PRIVATE magr_execute)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

magr_add_test(test_array_abi       tests/abi/test_array_abi.c)
magr_add_test(test_sha256          tests/merkle-integrity/test_sha256.c)
magr_add_test(test_canonical       tests/merkle-integrity/test_canonical.c)
magr_add_test(test_fortran_conformance tests/fortran-conformance/test_fortran_conformance.c)
magr_add_test(test_scheduler       tests/array-conformance/test_scheduler.c)

# The Python reference suite needs no compiler and is the only suite that can
# run on a machine with no toolchain at all.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME test_merkle_reference
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/merkle-integrity/test_merkle_integrity.py)
endif()

# ---------------------------------------------------------------- Summary
#
# Printed at configure time because the provenance of a numerical runtime is
# something you should not have to go looking for.
message(STATUS "")
message(STATUS "MAGR ${PROJECT_VERSION}  ABI ${PROJECT_VERSION}")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Fortran backend : ${MAGR_BLAS_REVISION} (netlib reference BLAS)")
message(STATUS "  Fortran compiler: ${MAGR_F77_COMPILER_ID} ${MAGR_F77_COMPILER_VERSION}")
message(STATUS "  Fortran flags   : ${MAGR_F77_COMPILER_FLAGS}")
message(STATUS "  fp-contract off : ${MAGR_F77_FP_FLAGS}")
message(STATUS "  XERBLA probe    : ${MAGR_XERBLA_INTERPOSE}")
message(STATUS "  Ed25519         : ${MAGR_WITH_ED25519}")
message(STATUS "  SHA-256 archive : ${MAGR_BLAS_ARCHIVE_SHA256}")
message(STATUS "")