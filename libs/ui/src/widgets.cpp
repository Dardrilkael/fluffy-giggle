#include <ws/ui/widgets.hpp>

#include <imgui.h>
#include <implot.h>

namespace ws::ui {

bool begin_window(const char* name, bool* open) {
    return ImGui::Begin(name, open);
}

void end_window() {
    ImGui::End();
}

void text(std::string_view value) {
    // TextUnformatted takes a [begin, end) pair, so the view needs no null
    // terminator - but an empty view may hand us a null data() pointer.
    if (value.empty()) {
        ImGui::TextUnformatted("");
        return;
    }
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

bool button(const char* label) {
    return ImGui::Button(label);
}

bool checkbox(const char* label, bool* value) {
    return ImGui::Checkbox(label, value);
}

bool slider_float(const char* label, float* value, float min_value, float max_value) {
    return ImGui::SliderFloat(label, value, min_value, max_value);
}

bool slider_int(const char* label, int* value, int min_value, int max_value) {
    return ImGui::SliderInt(label, value, min_value, max_value);
}

void separator() {
    ImGui::Separator();
}

void same_line() {
    ImGui::SameLine();
}

bool begin_plot(const char* title, float width, float height) {
    return ImPlot::BeginPlot(title, ImVec2(width, height));
}

void end_plot() {
    ImPlot::EndPlot();
}

void plot_line(const char* label, const float* xs, const float* ys, int count) {
    ImPlot::PlotLine(label, xs, ys, count);
}

void show_imgui_demo(bool* open) {
    ImGui::ShowDemoWindow(open);
}

void show_implot_demo(bool* open) {
    ImPlot::ShowDemoWindow(open);
}

}  // namespace ws::ui
