# ws::app is the headless app-framework services lib (nextplan stage
# 13): the command registry, then shortcut resolution with user
# rebinding and the undo stack - the headless halves of stage 13; the
# ui views over them (menu/toolbar builders, the command palette, the
# keymap editor) live in ws::ui. Pure std + ws::core - no third-party
# anywhere; ws::settings joins when keymap persistence lands (the
# FileSystem + json seams, the platform precedent).
ws_add_library(app DEPENDS ws::core)

ws_add_test(app SOURCES tests/test_app.cpp)
