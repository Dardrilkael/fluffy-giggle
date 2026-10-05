# Headless ws::physics throughput bench: steps a falling-boxes scene per
# dimension and prints wall-clock cost per tick - 2D (Box2D) and 3D (Jolt,
# single-threaded and with the machine's default worker pool). Absolute
# numbers are machine-dependent on purpose, which is why this is an app
# and not a suite case. Desktop-only like the other console apps (gated in
# apps/CMakeLists.txt).
ws_add_app(bench_physics DEPENDS ws::physics)
