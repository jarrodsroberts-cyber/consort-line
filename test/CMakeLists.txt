# The add-on's own tests, without baresip: the frames, the player, the proofs, and a call's
# link driven over real sockets on 127.0.0.1 with stand-in audio.
cmake_minimum_required(VERSION 3.18)
project(consort_line_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(unit unit.c ../consort/proto.c ../consort/proof.c ../consort/link.c)
target_include_directories(unit PRIVATE ../consort)
target_link_libraries(unit PRIVATE ws2_32 bcrypt winmm)
if(MSVC)
  # Every warning an error, but C4127 ("conditional expression is constant"), which some
  # versions give for do { } while (0)
  target_compile_options(unit PRIVATE /W4 /WX /wd4127)
endif()
