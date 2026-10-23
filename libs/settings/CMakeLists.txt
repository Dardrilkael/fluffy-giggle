# The headless settings store (nextplan stage 9): a typed schema
# (defaults + constraints), layered sources with provenance, change
# observers and JSON persistence through a minimal FileSystem seam.
# Pure std + ws::core.
ws_add_library(settings DEPENDS ws::core)

ws_add_test(settings SOURCES tests/test_settings.cpp)
