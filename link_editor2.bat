set INCLUDE=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um
set LIB=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64
C:\PROGRA~1\MICROS~2\18\COMMUN~1\VC\Tools\MSVC\1451~1.362\bin\Hostx64\x64\link.exe /nologo ^
 build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir\src\android_saf.cpp.obj ^
 build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir\src\canvas_panel.cpp.obj ^
 build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir\src\document.cpp.obj ^
 build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir\src\editor.cpp.obj ^
 build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir\src\main.cpp.obj ^
 build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir\src\results_panel.cpp.obj ^
 build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir\src\side_panels.cpp.obj ^
 /out:build\debug\bin\circsim_editor.exe ^
 /implib:build\debug\apps\circsim_editor\circsim_editor.lib ^
 /pdb:build\debug\bin\circsim_editor.pdb ^
 /version:0.0 /machine:x64 /debug /INCREMENTAL /subsystem:console ^
 build\debug\libs\ui\ws_ui.lib ^
 build\debug\libs\circsim\ws_circsim.lib ^
 build\debug\libs\jobs\ws_jobs.lib ^
 build\debug\libs\platform\ws_platform.lib ^
 build\debug\libs\app\ws_app.lib ^
 build\debug\libs\settings\ws_settings.lib ^
 build\debug\libs\gfx\ws_gfx.lib ^
 build\debug\libs\ui\ws_ui_gui.lib ^
 build\debug\libs\jobs\ws_jobs.lib ^
 build\debug\libs\platform\ws_platform.lib ^
 shell32.lib ole32.lib user32.lib ^
 build\debug\libs\app\ws_app.lib ^
 build\debug\libs\input\ws_input.lib ^
 build\debug\implot.lib build\debug\imgui.lib ^
 build\debug\build\fc-msvc-debug\glfw-build\src\glfw3.lib ^
 build\debug\volk.lib advapi32.lib ^
 build\debug\libs\settings\ws_settings.lib ^
 build\debug\libs\spice\ws_spice.lib ^
 build\debug\libs\math\ws_math.lib ^
 build\debug\libs\core\ws_core.lib ^
 kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib ^
 /MANIFEST /MANIFESTFILE:build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir/intermediate.manifest ^
 build\debug\apps\circsim_editor\CMakeFiles\circsim_editor.dir/manifest.res
