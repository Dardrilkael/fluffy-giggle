# ws::app is the headless app-framework services lib (nextplan stage
# 13): the command registry, then shortcut resolution with user
# rebinding and the undo stack - the headless halves of stage 13; the
# ui views over them (menu/toolbar builders, the command palette, the
# keymap editor) live in ws::ui. Pure std + ws::core + ws::settings -
# no third-party anywhere. ws::settings is PUBLIC like ws::core: the
# keymap file rides its FileSystem + json seams and the reference
# lives in a public header - the ws::platform precedent.
ws_add_library(app DEPENDS ws::core ws::settings)

ws_add_test(app SOURCES tests/test_app.cpp)
