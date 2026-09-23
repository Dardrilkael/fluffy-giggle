#pragma once

#include <string_view>

namespace ws::ui {

// Immediate-mode widgets: thin wrappers over Dear ImGui / ImPlot so apps can
// build UIs without ever including a third-party header (workspace rule:
// libs never leak their dependencies into consumers). Call these from
// on_frame() only. Label/name/title parameters are null-terminated C strings
// (like ImGui's); text() is the exception and takes arbitrary string data.
//
// Missing a wrapper you need? Add a one-liner in src/widgets.cpp rather
// than including <imgui.h> from app code.

// --- windows ------------------------------------------------------------------
// If begin_window() returns true, emit widgets, then call end_window() -
// end_window() is required either way (collapsed windows just skip the
// widgets in between).
bool begin_window(const char* name, bool* open = nullptr);
void end_window();

// --- widgets -------------------------------------------------------------------
void text(std::string_view value);
bool button(const char* label);
bool checkbox(const char* label, bool* value);
bool slider_float(const char* label, float* value, float min_value, float max_value);
bool slider_int(const char* label, int* value, int min_value, int max_value);
void separator();
void same_line();

// --- plots ---------------------------------------------------------------------
// Unlike windows, end_plot() is required ONLY when begin_plot() returned
// true. width/height <= 0 means "fit the available space".
bool begin_plot(const char* title, float width = -1.0f, float height = 0.0f);
void end_plot();
void plot_line(const char* label, const float* xs, const float* ys, int count);

// --- debug helpers ---------------------------------------------------------------
// The upstream demo windows - handy to verify a renderer, not for shipping
// UIs. The pointed-to bool unticks when the window's X is clicked.
void show_imgui_demo(bool* open = nullptr);
void show_implot_demo(bool* open = nullptr);

}  // namespace ws::ui
