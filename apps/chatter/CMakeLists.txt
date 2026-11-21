# The gui sibling of netchat: the same discovery beats, the same HELLO/SAY
# framed-TCP protocol, so a phone and the desktop terminals share one
# chat (run chatter next to a netchat - they find each other). Input is
# a real text field + send button (the ui lib's ime-rect seam opens the
# soft keyboard on Android; the v1 tap-phrase palette existed because
# that seam did not) and the feed draws the WhatsApp face: teal header,
# bubble feed with tails, timestamps and delivery ticks, a TODAY chip,
# and the scrolling input bar. GUI app on both platforms - added
# ungated in apps/CMakeLists.txt.
ws_add_app(chatter DEPENDS ws::ui ws::net)

# MSVC: silence deprecated localtime/getenv warnings from the patch
target_compile_definitions(chatter PRIVATE _CRT_SECURE_NO_WARNINGS)
