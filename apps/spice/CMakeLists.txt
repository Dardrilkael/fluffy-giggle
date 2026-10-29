# The ws::spice CLI: a thin parse -> simulate -> write shell (DESIGN.md
# §0.6). argv, file I/O and nothing else - every real behavior lives in the
# lib. Desktop console app like wavplayer/netchat (gated in
# apps/CMakeLists.txt).
ws_add_app(spice DEPENDS ws::spice)
