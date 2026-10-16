# The input family, promoted out of ws::ui (stage 3a): the GLFW-free
# event types + the InputFrame snapshot. Pure CPU and core-only - key/mouse
# codes ride as plain ints (GLFW's numbering), and the only writers of an
# InputFrame are ws::ui's platform TUs, so ws::ui links this lib and never
# the other way around.
ws_add_library(input DEPENDS ws::core)

ws_add_test(input SOURCES tests/test_input.cpp)
