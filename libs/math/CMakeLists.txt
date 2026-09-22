ws_add_library(math DEPENDS ws::core)
# Public headers expose Eigen (matrix.hpp) and GLM (vecmath.hpp), so both
# include paths must propagate to consumers.
target_link_libraries(ws_math PUBLIC eigen3 glm)

ws_add_test(math SOURCES tests/test_math.cpp tests/test_vecmath.cpp)
