# src/math gives the same bits on every host (recomp_math_hash.c).
add_executable(recomp_math_hash recomp_math_hash.c)
target_link_libraries(recomp_math_hash PRIVATE xbox_kernel)
add_test(NAME recomp_math_hash COMMAND recomp_math_hash)
