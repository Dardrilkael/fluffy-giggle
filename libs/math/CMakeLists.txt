ws_add_library(math DEPENDS ws::core)
# Eigen is a private implementation detail (the LU factorization in
# src/lu.cpp); nothing under include/ may include it, so it never reaches
# consumers.
target_link_libraries(ws_math PRIVATE eigen3)

ws_add_test(math SOURCES tests/test_math.cpp tests/test_vecmath.cpp tests/test_sparse.cpp)
