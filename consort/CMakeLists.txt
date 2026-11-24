# Consort's line, built inside baresip's own build as an app module:
#   cmake -S <baresip> -B <build> -DSTATIC=ON -DMODULES=g711
#         -DAPP_MODULES_DIR=<this repo> -DAPP_MODULES=consort
# The add-on is an OBJECT library baresip links into libbaresip's users; consort-line is the
# program (main.c) that loads only g711 and the add-on. -DCONSORT_LINE_CALLER=ON also builds
# the tests' far end (test/caller.c), which is never released.
project(consort)

if(NOT STATIC)
  message(FATAL_ERROR "consort-line is built static: -DSTATIC=ON")
endif()

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

add_library(${PROJECT_NAME} OBJECT consort.c link.c proof.c proto.c wake.c)
# BCrypt for the proofs and random bytes; winmm for the timer on older Windows
target_link_libraries(${PROJECT_NAME} PUBLIC bcrypt winmm ws2_32)

# libbaresip (the target "baresip") is defined after this folder: CMake links it at the end.
add_executable(consort-line main.c)
target_link_libraries(consort-line PRIVATE baresip)

option(CONSORT_LINE_CALLER "Build the tests' far end too (never released)" OFF)
if(CONSORT_LINE_CALLER)
  add_executable(consort-line-caller ${CMAKE_CURRENT_SOURCE_DIR}/../test/caller.c)
  target_link_libraries(consort-line-caller PRIVATE baresip)
endif()
