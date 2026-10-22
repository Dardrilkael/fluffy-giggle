# The headless job system (nextplan stage 8): a Result-shaped pool -
# priorities, cooperative cancellation, coalesced progress and completions
# marshalled to the owner thread. Pure std and core-only; jobs never touch
# the gui, ws::ui (or any consumer) links this lib and drains it.
ws_add_library(jobs DEPENDS ws::core)

ws_add_test(jobs SOURCES tests/test_jobs.cpp)
