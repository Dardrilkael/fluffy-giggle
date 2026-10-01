# The gui sibling of netchat: the same discovery beats, the same HELLO/SAY
# framed-TCP protocol, so a phone and the desktop terminals share one chat
# (run chatter next to a netchat - they find each other). Input is a
# tap-phrase palette; the soft keyboard would need an IME seam in the ui
# lib's Android window code, and this app deliberately makes zero lib
# edits. GUI app on both platforms - added ungated in apps/CMakeLists.txt.
ws_add_app(chatter DEPENDS ws::ui ws::net)
