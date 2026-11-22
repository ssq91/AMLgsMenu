#pragma once

#include <string>
#include <vector>

struct VideoMode {
    std::string label;
    int width{};
    int height{};
    int refresh{};
};

std::vector<VideoMode> LoadHdmiModes(const std::string &path);
std::vector<VideoMode> DefaultSkyModes();
std::string FormatVideoModeLabel(const VideoMode &mode);
// 紧凑形式，用于下拉框闭合态显示（如 1080p60），避免长标签被控件宽度截断。
std::string FormatVideoModeCompact(const VideoMode &mode);
int GetOutputFps(const std::string &path = "/sys/class/video/fps_info");
float ReadTemperatureC(const std::string &path = "/sys/class/thermal/thermal_zone0/temp");
