#include "menu_renderer.h"

#include "imgui.h"
// ActivateItemByID() 只在 internal 头里声明。它是 ImGui 官方提供的程序化激活入口，
// 走 NavNextActivateId（不受 NoNavInputs 限制），语义与真实按下 Enter 完全一致。
#include "imgui_internal.h"
#include "video_mode.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <png.h>
#include <GLES2/gl2.h>
#include <thread>
#include <iostream>
#include <filesystem>

static MenuRenderer::TelemetryData BuildMockTelemetry(const MenuState &state)
{
    using Clock = std::chrono::steady_clock;
    static auto last_ground_sample = Clock::time_point{};
    const auto now_tp = Clock::now();
    const float t = static_cast<float>(ImGui::GetTime());

    MenuRenderer::TelemetryData data{};
    data.has_rc_signal = true;
    data.has_flight_mode = true;
    data.has_attitude = true;
    data.has_gps = true;
    data.has_battery = true;
    data.has_sky_temp = true;
    data.has_ground_batt = true;

    // Always update attitude/flight mode/GPS/battery each tick
    const char *modes[] = {"HORIZON", "ANGLE", "ACRO", "RTH"};
    data.flight_mode = modes[static_cast<int>(t / 4.0f) % 4];
    data.latitude = 37.773 + 0.001 * std::sin(t * 0.15f);
    data.longitude = -122.431 + 0.0015 * std::cos(t * 0.12f);
    data.altitude_m = 120.0f + 12.0f * std::sin(t * 0.35f);
    data.home_distance_m = 250.0f + 35.0f * std::cos(t * 0.45f);
    data.has_home_bearing = true;
    data.home_bearing_rel_deg = 45.0f * std::sin(t * 0.2f);
    data.gps_satellites = 12 + static_cast<int>(3.0f * std::sin(t * 0.25f));
    data.cell_voltage = 3.8f + 0.12f * std::sin(t * 0.6f);
    data.pack_voltage = data.cell_voltage * 4.0f + 0.4f * std::cos(t * 0.3f);
    data.sky_temp_c = 45.0f + 5.0f * std::sin(t * 0.22f);
    data.roll_deg = 10.0f * std::sin(t * 0.6f);
    data.pitch_deg = 15.0f * std::cos(t * 0.5f);
    data.has_speed = true;
    data.ground_speed_mps = 12.0f + 2.0f * std::sin(t * 0.45f);
    data.air_speed_mps = 11.0f + 2.5f * std::cos(t * 0.4f);
    data.rc_signal = -55.0f + 4.0f * std::sin(t * 1.1f);
    data.ground_batt_percent = 70.0f + 10.0f * std::sin(t * 0.3f);
    data.has_rc = true;
    data.rc_left_x = std::sin(t * 0.6f);
    data.rc_left_y = std::cos(t * 0.6f);
    data.rc_right_x = std::sin(t * 0.9f + 1.0f);
    data.rc_right_y = std::cos(t * 0.9f + 1.0f);

    // Ground-related metrics: sample at most once per second
    static MenuRenderer::TelemetryData ground_cache{};
    auto ms_since_ground = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_ground_sample).count();
    const bool update_ground = (last_ground_sample.time_since_epoch().count() == 0 || ms_since_ground >= 1000);
    if (update_ground)
    {
        ground_cache.ground_signal_a = -60.0f + 5.0f * std::sin(t * 0.8f);
        ground_cache.ground_signal_b = -62.0f + 6.0f * std::cos(t * 0.65f);
        ground_cache.ground_temp_c = ReadTemperatureC();
        ground_cache.wifi_monitor_count = 0;
        ground_cache.has_wifi_monitor = false;

        const auto &ground_modes = state.GroundModes();
        VideoMode mode = ground_modes.empty() ? VideoMode{"1920x1080 @ 60Hz", 1920, 1080, 60}
                                              : ground_modes[state.GroundModeIndex() % ground_modes.size()];
        std::ostringstream res;
        res << mode.width << "x" << mode.height;
        ground_cache.video_resolution = res.str();

        static float last_fps_time = -1.0f;
        static int cached_fps = 0;
        const float now = static_cast<float>(ImGui::GetTime());
        if (last_fps_time < 0.0f || (now - last_fps_time) >= 1.0f)
        {
            cached_fps = GetOutputFps();
            last_fps_time = now;
        }
        int fps = cached_fps;
        ground_cache.video_refresh_hz = fps > 0 ? fps : (mode.refresh ? mode.refresh : 60);
        ground_cache.bitrate_mbps = std::max(1.0f, 6.0f + 2.0f * std::sin(t * 0.4f));

        last_ground_sample = now_tp;
    }

    data.ground_signal_a = ground_cache.ground_signal_a;
    data.ground_signal_b = ground_cache.ground_signal_b;
    data.ground_temp_c = ground_cache.ground_temp_c;
    data.wifi_monitor_count = ground_cache.wifi_monitor_count;
    data.has_wifi_monitor = ground_cache.has_wifi_monitor;
    data.video_resolution = ground_cache.video_resolution;
    data.video_refresh_hz = ground_cache.video_refresh_hz;
    data.bitrate_mbps = ground_cache.bitrate_mbps;

    return data;
}

MenuRenderer::MenuRenderer(MenuState &state, bool &use_mock, std::function<TelemetryData(TelemetryData)> provider,
                           std::function<void()> toggle_terminal,
                           std::function<bool()> terminal_visible,
                           std::function<void(const std::string &)> start_update,
                           std::function<int()> update_status,
                           std::function<void()> request_reboot,
                           std::function<bool(const std::string &, const std::string &)> apply_wifi_client,
                           std::string wifi_client_ssid, std::string wifi_client_password)
    : state_(state), use_mock_(use_mock), telemetry_provider_(std::move(provider)),
      toggle_terminal_(std::move(toggle_terminal)), terminal_visible_(std::move(terminal_visible)),
      start_update_(std::move(start_update)), update_status_(std::move(update_status)),
      request_reboot_(std::move(request_reboot)),
      apply_wifi_client_(std::move(apply_wifi_client))
{
    std::snprintf(wifi_client_ssid_, sizeof(wifi_client_ssid_), "%s", wifi_client_ssid.c_str());
    std::snprintf(wifi_client_password_, sizeof(wifi_client_password_), "%s", wifi_client_password.c_str());
    // OSD 图标已改为 ui::OsdGlyph() 矢量绘制，不再加载 /storage/digitalfpv/icons/*.png。
    // 那些 PNG 是彩色蓝色系，与琥珀单色主题冲突；去掉后也省下 8 张纹理。
}

bool MenuRenderer::LoadIcon(const char *path, ImTextureID &out_id, int &out_w, int &out_h)
{
    FILE *fp = std::fopen(path, "rb");
    if (!fp)
        return false;
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        std::fclose(fp);
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        std::fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        std::fclose(fp);
        return false;
    }
    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);
    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);
    std::vector<png_byte> data;
    data.resize(png_get_rowbytes(png_ptr, info_ptr) * height);
    std::vector<png_bytep> row_ptrs(height);
    for (png_uint_32 y = 0; y < height; ++y)
    {
        row_ptrs[y] = data.data() + y * png_get_rowbytes(png_ptr, info_ptr);
    }
    png_read_image(png_ptr, row_ptrs.data());
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    std::fclose(fp);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    out_id = static_cast<ImTextureID>(tex);
    out_w = static_cast<int>(width);
    out_h = static_cast<int>(height);
    return true;
}
MenuRenderer::~MenuRenderer()
{
    // OSD 图标已改为 ui::OsdGlyph() 矢量绘制，不再持有 PNG 纹理，无需 glDeleteTextures。
}

void MenuRenderer::Render(bool &running_flag)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    auto now_tp = std::chrono::steady_clock::now();
    bool need_refresh = (last_osd_update_time_ < 0.0f ||
                         last_osd_tp_.time_since_epoch().count() == 0 ||
                         std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_osd_tp_).count() >= 100);
    if (need_refresh)
    {
        TelemetryData new_data = cached_telemetry_;
        if (use_mock_)
        {
            new_data = BuildMockTelemetry(state_);
            has_mavlink_data_ = true;
        }
        else if (telemetry_provider_)
        {
            new_data = telemetry_provider_(cached_telemetry_);

            static auto last_ground_sample = std::chrono::steady_clock::time_point{};
            auto ms_ground = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_ground_sample).count();
            const bool update_ground = (last_ground_sample.time_since_epoch().count() == 0 || ms_ground >= 1000);
            if (!update_ground)
            {
                new_data.ground_signal_a = cached_telemetry_.ground_signal_a;
                new_data.ground_signal_b = cached_telemetry_.ground_signal_b;
                new_data.ground_temp_c = cached_telemetry_.ground_temp_c;
                new_data.video_refresh_hz = cached_telemetry_.video_refresh_hz;
                new_data.video_resolution = cached_telemetry_.video_resolution;
                new_data.bitrate_mbps = cached_telemetry_.bitrate_mbps;
            }
            else
            {
                last_ground_sample = now_tp;
            }
            bool any = new_data.has_attitude || new_data.has_gps || new_data.has_battery ||
                       new_data.has_rc_signal || new_data.has_sky_temp || new_data.has_flight_mode;
            if (any)
                has_mavlink_data_ = true;
        }
        cached_telemetry_ = new_data;
        last_osd_update_time_ = static_cast<float>(ImGui::GetTime());
        last_osd_tp_ = now_tp;
    }

    // No render throttling; always draw each frame. Remove FPS logs to reduce noise.

    DrawOsd(viewport, cached_telemetry_);

    ImGuiIO &io = ImGui::GetIO();
    const bool menu_visible = state_.MenuVisible();
    io.MouseDrawCursor = menu_visible;
    if (menu_visible)
    {
        DrawMenu(viewport, running_flag);
    }
    else
    {
        menu_visible_last_ = false;
    }
}

void MenuRenderer::DrawOsd(const ImGuiViewport *viewport, const TelemetryData &data) const
{
    // ---------------------------------------------------------------------
    // NERV 风格 OSD：琥珀单色、贴边两列、全部用 drawlist 直绘。
    //
    // 和旧版的区别：
    //   * 旧版用 ImGui 子窗口（OSD_GPS / OSD_VIDEO / ...）+ 彩色 PNG 图标 +
    //     冷白字 #EBF5FF + 深蓝描边，与琥珀色菜单是两套视觉语言；
    //   * 新版只有一种强调色（kAmber / kAmberData / kAmberDim 三档），
    //     图标改为 ui::OsdGlyph() 手绘矢量，天然跟随主题；
    //   * 不再建任何 ImGui 窗口，因此不会抢焦点、不会因 AutoResize 抖动，
    //     位置完全由下面的坐标常量决定。
    // ---------------------------------------------------------------------
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    const bool is_cn = state_.GetLanguage() == MenuState::Language::CN;

    // 统一缩放：以 1080p 为基准，720p 下 k≈0.667。OSD 用和菜单同一套 k，
    // 这样菜单开着时两边的字号、间距是协调的。
    const ui::Metrics m = ui::ComputeMetrics(viewport);
    const float k = m.k;

    const ImVec2 vp_min = viewport->Pos;
    const ImVec2 vp_max(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y);
    const ImVec2 center((vp_min.x + vp_max.x) * 0.5f, (vp_min.y + vp_max.y) * 0.5f);

    // 边距与行距
    const float edge = 28.0f * k;          // 距屏幕边缘
    const float icon_sz = 20.0f * k;       // 图标外接方边长
    const float icon_gap = 9.0f * k;       // 图标与文字间距
    const float row_h = 30.0f * k;         // 每行高度
    const float body_sz = 22.0f * k;       // 正文
    const float small_sz = 18.0f * k;      // 次要信息

    const ImU32 ink = ui::kAmberData;      // 主数据色
    const ImU32 ink_dim = ui::kAmberDim;   // 次要/单位
    const ImU32 ink_struct = ui::kAmber;

    // 文字统一描一层纯黑边，保证压在任何画面上都读得清。
    auto text = [&](ImVec2 pos, const char *s, float size, ImU32 col)
    {
        const ImVec2 off[] = {ImVec2(-1.2f, 0), ImVec2(1.2f, 0), ImVec2(0, -1.2f), ImVec2(0, 1.2f),
                              ImVec2(-0.9f, -0.9f), ImVec2(0.9f, -0.9f), ImVec2(-0.9f, 0.9f), ImVec2(0.9f, 0.9f)};
        for (const auto &o : off)
        {
            dl->AddText(ImGui::GetFont(), size, ImVec2(pos.x + o.x, pos.y + o.y), IM_COL32(0, 0, 0, 255), s);
        }
        dl->AddText(ImGui::GetFont(), size, pos, col, s);
    };

    const auto text_w = [&](const char *s, float size)
    {
        return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    };

    // 一行 = 图标 + 主文本 + 可选次要文本（单位/说明），整体左对齐或右对齐。
    // align_right = true 时 x 是右边界，整行向左铺开。
    auto row = [&](float x, float y, ui::OsdIcon ic, const char *main, const char *sub,
                   float main_sz, bool align_right)
    {
        const float mw = text_w(main, main_sz);
        const float sw = (sub != nullptr && *sub != '\0') ? text_w(sub, small_sz) + 6.0f * k : 0.0f;
        const float total = icon_sz + icon_gap + mw + sw;

        const float x0 = align_right ? (x - total) : x;
        const ImVec2 ic_c(x0 + icon_sz * 0.5f, y + row_h * 0.5f);
        ui::OsdGlyph(dl, ic, ic_c, icon_sz, ink_struct);

        const float tx = x0 + icon_sz + icon_gap;
        // 正文基线：让文字在行内垂直居中
        const float ty = y + (row_h - main_sz) * 0.5f;
        text(ImVec2(tx, ty), main, main_sz, ink);
        if (sw > 0.0f)
        {
            text(ImVec2(tx + mw + 6.0f * k, ty + (main_sz - small_sz)), sub, small_sz, ink_dim);
        }
    };

    // 菜单打开时，左侧面板会盖住 OSD。把整块 OSD 的"可用区"右移到面板右侧，
    // 这样左右两列都不会被遮住（旧版只处理了顶部信号读数）。
    float usable_left = vp_min.x;
    if (state_.MenuVisible())
    {
        const float panel_right = vp_min.x + m.panel_x + m.panel_w;
        if (panel_right < vp_max.x - 120.0f * k)
        {
            usable_left = panel_right;
        }
    }

    // OSD 的横向基准：可用区（菜单关闭时=全屏，菜单打开时=面板右侧）的中心。
    // 俯仰梯和模式横幅都要用它，否则菜单一开它们就相对剩余空间偏左。
    const float osd_cx = (usable_left + vp_max.x) * 0.5f;

    // ---------------------------------------------------------------- 顶部
    // 左上：WiFi 监控计数（保留原有语义，只是换了配色与图标）
    // WiFi monitor 计数只对 wfb 模式有意义（统计 type monitor 接口数量）。
    // STA 模式下系统里没有 monitor 接口，计数恒为 0 —— 显示「0 WIFI PLUGGED」
    // 既不准确也没有信息量，因此该模式下整行不画。
    if (!use_mock_ && data.has_wifi_monitor && !data.link_sta && data.wifi_monitor_count > 0)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d WIFI PLUGGED", data.wifi_monitor_count);
        const float y = vp_min.y + edge * 0.7f;
        row(usable_left + edge, y, ui::OsdIcon::Wifi, buf, nullptr, small_sz, false);
    }

    // 顶部居中：链路信号。用「A/B/RC」三段，中间用竖线分隔，比原来的 " | " 更利落。
    // STA 模式下不显示信号读数（用户明确要求）。
    // wfb 模式保留原来的双天线「地面A / 地面B」读数。
    if (!data.link_sta &&
        (data.has_rc_signal || data.ground_signal_a != 0.0f || data.ground_signal_b != 0.0f))
    {
        // STA 模式同样能拿到双天线各自的 RSSI（驱动 rssi_a / rssi_b），
        // 所以和 wfb 一样显示 地面A / 地面B：两根天线的差值是分集是否有效的直接依据，
        // 合并成一个数字反而丢信息。
        {
            char a_buf[32], b_buf[32], rc_buf[32];
            std::snprintf(a_buf, sizeof(a_buf), is_cn ? "\u5730\u9762A %d dBm" : "A %d dBm",
                          static_cast<int>(data.ground_signal_a));
            std::snprintf(b_buf, sizeof(b_buf), is_cn ? "\u5730\u9762B %d dBm" : "B %d dBm",
                          static_cast<int>(data.ground_signal_b));
            std::snprintf(rc_buf, sizeof(rc_buf), "RC %d dBm", static_cast<int>(data.rc_signal));

            const bool show_rc = data.has_rc_signal;
            const float seg_w = text_w(a_buf, body_sz) + text_w(b_buf, body_sz) +
                                (show_rc ? text_w(rc_buf, body_sz) : 0.0f) +
                                (show_rc ? 2.0f : 1.0f) * 28.0f * k;
            const float total_w = icon_sz + icon_gap + seg_w;
            const float x0 = osd_cx - total_w * 0.5f;
            const float y = vp_min.y + edge * 0.7f;

            ui::OsdGlyph(dl, ui::OsdIcon::Antenna,
                         ImVec2(x0 + icon_sz * 0.5f, y + row_h * 0.5f), icon_sz, ink_struct);

            float tx = x0 + icon_sz + icon_gap;
            const float ty = y + (row_h - body_sz) * 0.5f;
            text(ImVec2(tx, ty), a_buf, body_sz, ink);
            tx += text_w(a_buf, body_sz) + 14.0f * k;
            dl->AddLine(ImVec2(tx, y + row_h * 0.28f), ImVec2(tx, y + row_h * 0.72f), ui::kAmberLine, 1.0f);
            tx += 14.0f * k;
            text(ImVec2(tx, ty), b_buf, body_sz, ink);
            if (show_rc)
            {
                tx += text_w(b_buf, body_sz) + 14.0f * k;
                dl->AddLine(ImVec2(tx, y + row_h * 0.28f), ImVec2(tx, y + row_h * 0.72f), ui::kAmberLine, 1.0f);
                tx += 14.0f * k;
                text(ImVec2(tx, ty), rc_buf, body_sz, ink);
            }
        }
    }

    // ------------------------------------------------------------ 飞行模式
    // 放在中心偏上，保留原有的"比正文大一号"处理，但改成琥珀 + 上下两条短横线夹住。
    if (data.has_flight_mode && !data.flight_mode.empty())
    {
        const float mode_sz = body_sz * 1.55f;
        const float mw = text_w(data.flight_mode.c_str(), mode_sz);
        const float y = center.y - viewport->Size.y * 0.25f;
        const float x = osd_cx - mw * 0.5f;
        text(ImVec2(x, y), data.flight_mode.c_str(), mode_sz, ink_struct);

        // 上下两条短横线夹住模式文字。mode_sz 是字号，实际字高约 0.75 倍，
        // 所以下横线不能直接用 y + mode_sz，否则会甩出很远（实测离文字有 20px 空白）。
        const float lw = mw * 0.5f + 26.0f * k;
        const float text_h = mode_sz * 0.75f;
        const float ly = y - 7.0f * k;
        const float by = y + text_h + 7.0f * k;
        dl->AddLine(ImVec2(osd_cx - lw, ly), ImVec2(osd_cx + lw, ly), ui::kAmberLine, 1.0f);
        dl->AddLine(ImVec2(osd_cx - lw, by), ImVec2(osd_cx + lw, by), ui::kAmberLine, 1.0f);
    }

    // ------------------------------------------------------------ 姿态地平线
    // 几何完全沿用旧实现（俯仰 2px/度、横滚旋转、±5° 刻度），只把配色换成琥珀。
    if (data.has_attitude)
    {
        const float line_half_len = viewport->Size.x * 0.25f * 0.66f;
        const float tick_gap = line_half_len;
        const float tick_step_deg = 10.0f;
        const int tick_count = 9;
        const float rad = data.roll_deg * 3.1415926f / 180.0f;
        const float cosr = std::cos(rad);
        const float sinr = std::sin(rad);
        const float pitch_offset = data.pitch_deg * 2.0f;

        auto rot = [&](float x, float y)
        {
            return ImVec2(x * cosr - y * sinr, x * sinr + y * cosr);
        };

        // 主地平线
        ImVec2 p1 = rot(-line_half_len, 0.0f);
        ImVec2 p2 = rot(line_half_len, 0.0f);
        p1 = ImVec2(p1.x + osd_cx, p1.y + center.y + pitch_offset);
        p2 = ImVec2(p2.x + osd_cx, p2.y + center.y + pitch_offset);
        dl->AddLine(p1, p2, IM_COL32(0, 0, 0, 200), 4.0f);   // 黑底衬
        dl->AddLine(p1, p2, ink_struct, 2.2f);

        // 中心零位刻度
        dl->AddLine(ImVec2(osd_cx - tick_gap, center.y), ImVec2(osd_cx + tick_gap, center.y),
                    IM_COL32(0, 0, 0, 200), 3.4f);
        dl->AddLine(ImVec2(osd_cx - tick_gap, center.y), ImVec2(osd_cx + tick_gap, center.y),
                    ink_struct, 1.6f);

        // 俯仰梯：贴近中心的短刻度 + 角度数字，随俯仰上下移动。
        // 旧实现把刻度画在 center ± line_half_len（1080p 下 ±316px），远离地平线，
        // 加上 5°/2px 的密集间距，实际渲染成两根糊在一起的竖柱，读不出俯仰信息。
        const float lad_inner = 54.0f * k;    // 刻度内端距中心的横向距离
        const float lad_len = 26.0f * k;      // 刻度长度
        const float num_sz = 17.0f * k;       // 角度数字字号
        for (int i = 1; i <= tick_count; ++i)
        {
            const float delta = tick_step_deg * static_cast<float>(i);
            // 数字只画到 ±40°，再远就出了可视区，画了也没用
            const bool with_num = (delta <= 40.0f);
            for (int sgn = 0; sgn < 2; ++sgn)
            {
                const float y = center.y + pitch_offset +
                                ((sgn == 0) ? -(delta * 2.0f) : (delta * 2.0f));
                if (y < vp_min.y + 40.0f * k || y > vp_max.y - 40.0f * k)
                {
                    continue;
                }
                // 左右各一条，向内指
                dl->AddLine(ImVec2(osd_cx - lad_inner - lad_len, y),
                            ImVec2(osd_cx - lad_inner, y), ui::kAmberLine, 1.6f);
                dl->AddLine(ImVec2(osd_cx + lad_inner, y),
                            ImVec2(osd_cx + lad_inner + lad_len, y), ui::kAmberLine, 1.6f);
                if (with_num)
                {
                    char deg[8];
                    std::snprintf(deg, sizeof(deg), "%d", static_cast<int>(delta));
                    const float dw = text_w(deg, num_sz);
                    // 角度数字用主数据色（原来是 ink_dim，实测在真实画面上几乎读不出）
                    text(ImVec2(osd_cx - lad_inner - lad_len - dw - 8.0f * k,
                                y - num_sz * 0.5f), deg, num_sz, ink);
                    text(ImVec2(osd_cx + lad_inner + lad_len + 8.0f * k,
                                y - num_sz * 0.5f), deg, num_sz, ink);
                }
            }
        }
    }

    // ------------------------------------------------- 左列（自下而上堆叠）
    float left_y = vp_max.y - edge - row_h;

    if (data.has_battery)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), is_cn ? "\u603b\u7535 %.2fV" : "PACK %.2fV", data.pack_voltage);
        row(usable_left + edge, left_y, ui::OsdIcon::BatteryPack, buf, nullptr, body_sz, false);
        left_y -= row_h;

        std::snprintf(buf, sizeof(buf), is_cn ? "\u5355\u8282 %.2fV%s" : "CELL %.2fV%s", data.cell_voltage,
                      data.cell_voltage_estimated ? (is_cn ? "(\u4f30)" : "~") : "");
        row(usable_left + edge, left_y, ui::OsdIcon::BatteryCell, buf, nullptr, body_sz, false);
        left_y -= row_h;
    }

    if (data.has_gps)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), is_cn ? "\u79bb\u5bb6 %.1fm" : "HOME %.1fm", data.home_distance_m);
        row(usable_left + edge, left_y, ui::OsdIcon::Home, buf, nullptr, body_sz, false);
        left_y -= row_h;

        if (data.gps_satellites >= 0)
        {
            std::snprintf(buf, sizeof(buf), is_cn ? "\u661f\u6570 %d" : "SAT %d", data.gps_satellites);
            row(usable_left + edge, left_y, ui::OsdIcon::Gps, buf, nullptr, body_sz, false);
            left_y -= row_h;
        }
    }

    if (data.has_speed)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), is_cn ? "\u5730\u901f %.1f m/s" : "GS %.1f m/s",
                      data.ground_speed_mps);
        char sub[32];
        std::snprintf(sub, sizeof(sub), is_cn ? "\u7a7a\u901f %.1f" : "AS %.1f", data.air_speed_mps);
        row(usable_left + edge, left_y, ui::OsdIcon::Speed, buf, sub, body_sz, false);
        left_y -= row_h;
    }

    // ------------------------------------------------- 右列（自下而上堆叠）
    float right_y = vp_max.y - edge - row_h;

    // 视频链路（原本在右下角，这里保留位置但补上分辨率/帧率的层级）
    {
        char buf[64];
        // 单位必须留在主文本里：副文本位置会被分辨率占用，否则「视频 0.0」就丢了 Mbps。
        std::snprintf(buf, sizeof(buf), is_cn ? "\u89c6\u9891 %.1f Mbps" : "VIDEO %.1f Mbps",
                      data.bitrate_mbps);
        char sub[64];
        sub[0] = '\0';
        // STA 模式只报真实吞吐（rx_bytes 增量），不再显示协商速率/频点 —— 用户不需要。
        // wfb 模式仍显示 分辨率@刷新率（该值来自菜单配置的输出模式，与 /sys/class/display/mode 一致）。
        if (!data.link_sta && !data.video_resolution.empty() && data.video_refresh_hz > 0)
        {
            std::snprintf(sub, sizeof(sub), "%s@%dHz", data.video_resolution.c_str(), data.video_refresh_hz);
        }
        row(vp_max.x - edge, right_y, ui::OsdIcon::Monitor, buf, sub, body_sz, true);
        right_y -= row_h;
    }

    if (data.has_ground_batt)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), is_cn ? "\u672c\u673a %.0f%%" : "GND %.0f%%", data.ground_batt_percent);
        row(vp_max.x - edge, right_y, ui::OsdIcon::BatteryPack, buf, nullptr, body_sz, true);
        right_y -= row_h;
    }

    // 温度：地面端总是显示；天空端仅在有效时显示
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), is_cn ? "\u5730\u9762 %.1f\u2103" : "GND %.1f\u2103", data.ground_temp_c);
        row(vp_max.x - edge, right_y, ui::OsdIcon::TempGround, buf, nullptr, body_sz, true);
        right_y -= row_h;
    }
    if (data.has_sky_temp)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), is_cn ? "\u5929\u7a7a %.1f\u2103" : "AIR %.1f\u2103", data.sky_temp_c);
        row(vp_max.x - edge, right_y, ui::OsdIcon::TempAir, buf, nullptr, body_sz, true);
        right_y -= row_h;
    }

    // ------------------------------------------------------------ 回家方向
    // 原来跟 GPS 块挤在左下角，现在独立放在左列上方，箭头单独绘制。
    if (data.has_gps && data.has_home_bearing)
    {
        const char *label = is_cn ? "\u56de\u5bb6" : "HOME";
        const float lw = text_w(label, small_sz);
        const float y = left_y;
        const float x = usable_left + edge;

        row(x, y, ui::OsdIcon::Home, label, nullptr, small_sz, false);

        // 箭头：以文字右侧为中心，按相对方位角旋转
        const float ax = x + icon_sz + icon_gap + lw + 26.0f * k;
        const float ay = y + row_h * 0.5f;
        const float a_sz = 11.0f * k;
        const float rel = data.home_bearing_rel_deg * 3.1415926f / 180.0f;
        const ImVec2 dir(std::sin(rel), -std::cos(rel));
        const ImVec2 side(-dir.y, dir.x);
        const ImVec2 tip(ax + dir.x * a_sz, ay + dir.y * a_sz);
        const ImVec2 lb(ax - dir.x * a_sz * 0.55f + side.x * a_sz * 0.62f,
                        ay - dir.y * a_sz * 0.55f + side.y * a_sz * 0.62f);
        const ImVec2 rb(ax - dir.x * a_sz * 0.55f - side.x * a_sz * 0.62f,
                        ay - dir.y * a_sz * 0.55f - side.y * a_sz * 0.62f);
        dl->AddTriangleFilled(tip, lb, rb, ink_struct);
        dl->AddTriangle(tip, lb, rb, IM_COL32(0, 0, 0, 180), 1.2f);
    }
}

namespace
{
// ImGui 公共头不导出 IM_PI（它在 imgui_internal.h 里），这里自带一份。
constexpr float kPi = 3.14159265358979323846f;

// 面板头部/卡片的纵向锚点（单位：k）。DrawMenu 与 DrawGroupRows 共用，
// 保证头部高度与卡片起点不会各写一份而漂移。
constexpr float kPanelHeaderTop = 30.0f;  // 头部顶边
constexpr float kPanelCardTop = 135.0f;   // SETTINGS 卡片顶边
constexpr float kPanelBottomBar = 56.0f;  // 底部动作条高度
} // namespace

// 左侧图标栏：提交阶段（第一趟）。
// 只建 item 并决定 rail_group_，不画任何图形 —— 这样 DrawRailItem() 在第二趟绘制时
// 已经知道本帧最终的 rail_group_，不会出现"旧项已画完才发现自己失焦"的一帧双高亮。
void MenuRenderer::SubmitRailItems(const ui::Metrics &m,
                                   const std::function<void(const std::function<void()> &)> &reg)
{
    const ImVec2 win = ImGui::GetWindowPos();
    const float item_h = 72.0f * m.k;
    const float total_h = item_h * 5.0f;
    const float start_y = win.y + (m.panel_h - total_h) * 0.5f;

    for (int g = 0; g < 5; ++g)
    {
        const float y0 = start_y + item_h * static_cast<float>(g);
        char id[16];
        std::snprintf(id, sizeof(id), "rail%d", g);
        reg([&]
            {
                ImGui::SetCursorScreenPos(ImVec2(win.x, y0));
                // EnableNav 必须显式传入：InvisibleButton 默认带 ImGuiItemFlags_NoNav，
                // 否则遥控器方向键永远无法把焦点落到图标栏上。
                const bool pressed = ImGui::InvisibleButton(id, ImVec2(m.rail_w, item_h),
                                                            ImGuiButtonFlags_EnableNav);
                // 只处理鼠标点击：点击哪个图标，权威光标就跟到哪个。
                // 这里绝不能读 IsItemFocused() 去同步 nav_cursor_ —— 焦点移动要下一帧才生效，
                // 那样会把刚由方向键设好的光标又踩回旧图标（历史上就是这样导致进不了面板）。
                if (pressed)
                {
                    rail_group_ = g;
                    nav_cursor_ = g;
                }
            });
    }
}

// 左侧图标栏：绘制阶段（第二趟）。
// item 已在 SubmitRailItems() 里提交，这里只按当前状态画图形，故同一帧内不会出现双高亮。
void MenuRenderer::DrawRailItem(int group, const ui::Metrics &m)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 win = ImGui::GetWindowPos();
    const float item_h = 72.0f * m.k;
    const float total_h = item_h * 5.0f;
    const float start_y = win.y + (m.panel_h - total_h) * 0.5f;
    const float y0 = start_y + item_h * static_cast<float>(group);

    const bool active = (rail_group_ == group);
    const ImU32 stroke = active ? ui::kAmber : ui::kAmberDim;
    const ImU32 glyph = active ? ui::kAmber : ui::kAmberDim;

    // 选中项最左侧 2px 竖条。
    if (active)
    {
        dl->AddRectFilled(ImVec2(win.x, y0), ImVec2(win.x + 2.0f, y0 + item_h), ui::kAmber);
    }

    const ImVec2 c(win.x + m.rail_w * 0.5f + 1.0f, y0 + item_h * 0.5f);
    ui::Hexagon(dl, c, 15.0f * m.k, 13.0f * m.k, stroke, 1.0f);

    const float u = m.k; // 图标单位，随 k 缩放
    switch (group)
    {
    case 0: // 链路：两段同心弧 + 底部 2x2 方块
    {
        dl->PathArcTo(ImVec2(c.x, c.y - 1.0f * u), 5.0f * u, -kPi * 0.85f, -kPi * 0.15f, 16);
        dl->PathStroke(glyph, 0, 1.0f);
        dl->PathArcTo(ImVec2(c.x, c.y - 1.0f * u), 8.0f * u, -kPi * 0.85f, -kPi * 0.15f, 16);
        dl->PathStroke(glyph, 0, 1.0f);
        for (int i = 0; i < 2; ++i)
        {
            for (int j = 0; j < 2; ++j)
            {
                const ImVec2 p0(c.x - 3.0f * u + static_cast<float>(i) * 2.6f * u,
                                c.y + 3.0f * u + static_cast<float>(j) * 2.6f * u);
                dl->AddRectFilled(p0, ImVec2(p0.x + 1.8f * u, p0.y + 1.8f * u), glyph);
            }
        }
        break;
    }
    case 1: // 图传：6x6 矩形 + 右侧三角形
    {
        dl->AddRect(ImVec2(c.x - 8.0f * u, c.y - 4.0f * u), ImVec2(c.x + 1.0f * u, c.y + 4.0f * u),
                    glyph, 0.0f, 0, 1.0f);
        dl->AddTriangleFilled(ImVec2(c.x + 2.5f * u, c.y),
                              ImVec2(c.x + 8.0f * u, c.y - 5.0f * u),
                              ImVec2(c.x + 8.0f * u, c.y + 5.0f * u), glyph);
        break;
    }
    case 2: // 网络：圆 + 一条横线 + 一个椭圆弧
    {
        dl->AddCircle(ImVec2(c.x, c.y - 4.5f * u), 3.0f * u, glyph, 16, 1.0f);
        dl->AddLine(ImVec2(c.x - 8.0f * u, c.y + 1.0f * u), ImVec2(c.x + 8.0f * u, c.y + 1.0f * u),
                    glyph, 1.0f);
        dl->PathArcTo(ImVec2(c.x, c.y + 1.0f * u), 7.0f * u, kPi * 0.18f, kPi * 0.82f, 16);
        dl->PathStroke(glyph, 0, 1.0f);
        break;
    }
    case 3: // 显示：9x6 矩形 + 下方一条横线
    {
        dl->AddRect(ImVec2(c.x - 9.0f * u, c.y - 6.0f * u), ImVec2(c.x + 9.0f * u, c.y + 2.0f * u),
                    glyph, 0.0f, 0, 1.0f);
        dl->AddLine(ImVec2(c.x - 4.5f * u, c.y + 6.0f * u), ImVec2(c.x + 4.5f * u, c.y + 6.0f * u),
                    glyph, 1.0f);
        break;
    }
    default: // 系统：小圆 + 6 根辐条
    {
        dl->AddCircle(c, 3.0f * u, glyph, 12, 1.0f);
        for (int i = 0; i < 6; ++i)
        {
            const float a = kPi * 2.0f * static_cast<float>(i) / 6.0f;
            dl->AddLine(ImVec2(c.x + std::cos(a) * 4.5f * u, c.y + std::sin(a) * 4.5f * u),
                        ImVec2(c.x + std::cos(a) * 8.0f * u, c.y + std::sin(a) * 8.0f * u),
                        glyph, 1.0f);
        }
        break;
    }
    }
}

// 绘制当前分组（rail_group_）的全部设置行。
// 焦点注册顺序 == 视觉顺序（自上而下），因此单一 focus 链就能正确驱动方向键。
void MenuRenderer::DrawGroupRows(const ui::Metrics &m, int &focus_index, int &focused_index,
                                 const std::function<void(const std::function<void()> &)> &reg,
                                 bool &kodi_popup_requested, bool &running_flag)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 win = ImGui::GetWindowPos();
    const bool is_cn = state_.GetLanguage() == MenuState::Language::CN;

    const float card_x0 = win.x + m.panel_x + m.pad;
    const float card_x1 = win.x + m.panel_x + m.panel_w - m.pad;
    const float label_y0 = win.y + kPanelCardTop * m.k;              // 分组标签基线区
    const float card_y0 = label_y0 + m.en_sz + 10.0f * m.k;          // 卡片顶边让开标签
    const float card_pad_x = 16.0f * m.k;
    const float card_pad_y = 10.0f * m.k;

    const float content_x0 = card_x0 + card_pad_x;
    const float content_x1 = card_x1 - card_pad_x;
    const float rows_top = card_y0 + card_pad_y;

    // 卡片分组标签画在卡片上方（它是分区标题，不属于卡片内容），
    // 前面加一个 5x5 实心方块作为 NERV 风格的段落符。
    const float grp_y = label_y0;
    dl->AddRectFilled(ImVec2(content_x0, grp_y + m.en_sz * 0.30f),
                      ImVec2(content_x0 + 5.0f * m.k, grp_y + m.en_sz * 0.30f + 5.0f * m.k),
                      ui::kAmber);
    dl->AddText(ImGui::GetFont(), m.en_sz, ImVec2(content_x0 + 10.0f * m.k, grp_y),
                ui::kAmber, is_cn ? "\u8bbe\u7f6e" : "SETTINGS");

    const float ctrl_w = std::max(90.0f * m.k, (content_x1 - content_x0) * 0.52f);
    const float ctrl_x = content_x1 - ctrl_w;

    int row_no = 0;
    // 记录本组第一行的焦点序号：行可能因数据不可用而不注册焦点，故不能假设面板从 5 开始。
    panel_first_row_ = focus_index;
    // 行渲染：背景/标签/分隔线用 drawlist，控件用 ImGui item 且必须是该行最后绘制的 item，
    // 否则 IsItemFocused() 取到的是别的 item。
    auto draw_row = [&](const char *cn, const char *en, const std::function<void()> &control, bool focusable = true)
    {
        const int my_index = focus_index;
        const float row_y = rows_top + m.row_h * static_cast<float>(row_no);
        // 高亮只认权威光标 nav_cursor_（与 pending_focus_index_），不读 ImGui 的 IsItemFocused()：
        // 菜单窗口带 NoNavInputs，ImGui 不参与导航，高亮必须由语义表驱动，否则会和真实焦点脱节。
        const bool row_focused = (nav_cursor_ == my_index) || (pending_focus_index_ == my_index);
        if (row_focused)
        {
            dl->AddRectFilled(ImVec2(card_x0, row_y), ImVec2(card_x1, row_y + m.row_h), ui::kAmberSoft);
            dl->AddRectFilled(ImVec2(card_x0, row_y), ImVec2(card_x0 + 2.0f, row_y + m.row_h), ui::kAmber);
        }

        const float label_y = row_y + (m.row_h - m.label_sz) * 0.5f;
        dl->AddText(ImGui::GetFont(), m.label_sz, ImVec2(content_x0, label_y), ui::kAmberLabel, cn);
        const ImVec2 cn_size = ImGui::GetFont()->CalcTextSizeA(m.label_sz, 1.0e9f, 0.0f, cn);
        dl->AddText(ImGui::GetFont(), m.en_sz,
                    ImVec2(content_x0 + cn_size.x + 8.0f * m.k, label_y + (m.label_sz - m.en_sz)),
                    ui::kAmberDim, en);

        const float ctrl_y = row_y + (m.row_h - m.ctrl_h) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(ctrl_x, ctrl_y));
        if (focusable)
        {
            // 控件区统一用数据字号：ImGui 的内置下拉箭头半径 = FontSize*0.40，
            // 若沿用 31px 标题字号会画出 24px 宽的巨型三角；24px 下约 19px，与设计稿一致。
            ImGui::PushFont(nullptr, m.data_sz);
            ImGui::PushItemWidth(ctrl_w);
            reg(control);
            ImGui::PopItemWidth();
            ImGui::PopFont();
        }
        else
        {
            // 数据不可用时显示占位符，且不占用焦点序号（与原表格行为一致）。
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(m.data_sz, 1.0e9f, 0.0f, "--");
            dl->AddText(ImGui::GetFont(), m.data_sz,
                        ImVec2(ctrl_x, ctrl_y + (m.ctrl_h - ts.y) * 0.5f), ui::kAmberDim, "--");
        }

        ++row_no;
    };

    // 行间 1px 分隔线在行绘制后补（避免被焦点底遮住）
    auto row_separator = [&](bool is_last)
    {
        if (is_last)
            return;
        const float y = rows_top + m.row_h * static_cast<float>(row_no);
        dl->AddLine(ImVec2(content_x0, y), ImVec2(content_x1, y), ui::kAmberLine, 1.0f);
    };

    const auto &channels = state_.Channels();
    const auto &bandwidths = state_.Bandwidths();
    const auto &sky_modes = state_.SkyModes();
    const auto &ground_modes = state_.GroundModes();
    const auto &bitrates = state_.Bitrates();
    const auto &powers = state_.PowerLevels();
    const auto &mcs_levels = state_.McsLevels();
    const auto &volume_levels = state_.VolumeLevels();
    const auto &buffer_levels = state_.BufferLevels();
    const auto &hdmi_attrs = state_.HdmiAttrs();
    const auto &network_modes = state_.NetworkModes();

    if (rail_group_ == 0) // 链路
    {
        draw_row(is_cn ? "\u4fe1\u9053" : "Channel", "CH", [&]
                 {
                     if (ImGui::BeginCombo("##channel", std::to_string(channels[state_.ChannelIndex()]).c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(channels.size()); ++i)
                         {
                             bool selected = (state_.ChannelIndex() == i);
                             if (ImGui::Selectable(std::to_string(channels[i]).c_str(), selected))
                             {
                                 state_.SetChannelIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u9891\u5bbd" : "Bandwidth", "BW", [&]
                 {
                     if (ImGui::BeginCombo("##bandwidth", bandwidths[state_.BandwidthIndex()]))
                     {
                         for (int i = 0; i < static_cast<int>(bandwidths.size()); ++i)
                         {
                             bool selected = (state_.BandwidthIndex() == i);
                             if (ImGui::Selectable(bandwidths[i], selected))
                             {
                                 state_.SetBandwidthIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u5929\u7a7a\u7aef\u7801\u7387" : "Bitrate", "BITRATE", [&]
                 {
                     if (ImGui::BeginCombo("##bitrate", std::to_string(bitrates[state_.BitrateIndex()]).c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(bitrates.size()); ++i)
                         {
                             bool selected = (state_.BitrateIndex() == i);
                             if (ImGui::Selectable(std::to_string(bitrates[i]).c_str(), selected))
                             {
                                 state_.SetBitrateIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u5929\u7a7a\u7aef\u529f\u7387" : "Air TX Power", "AIR TX", [&]
                 {
                     if (ImGui::BeginCombo("##sky_power", std::to_string(powers[state_.SkyPowerIndex()]).c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(powers.size()); ++i)
                         {
                             bool selected = (state_.SkyPowerIndex() == i);
                             if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected))
                             {
                                 state_.SetSkyPowerIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u5730\u9762\u7aef\u529f\u7387" : "Ground TX Power", "GND TX", [&]
                 {
                     if (ImGui::BeginCombo("##ground_power", std::to_string(powers[state_.GroundPowerIndex()]).c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(powers.size()); ++i)
                         {
                             bool selected = (state_.GroundPowerIndex() == i);
                             if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected))
                             {
                                 state_.SetGroundPowerIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 });
        row_separator(false);

        // MCS 不可用时只显示 "--" 且不注册焦点
        draw_row(is_cn ? "\u5929\u7a7a\u7aef MCS" : "Air MCS", "AIR MCS", [&]
                 {
                     const int current_mcs = mcs_levels[state_.SkyMcsIndex()];
                     if (ImGui::BeginCombo("##sky_mcs", std::to_string(current_mcs).c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(mcs_levels.size()); ++i)
                         {
                             bool selected = (state_.SkyMcsIndex() == i);
                             if (ImGui::Selectable(std::to_string(mcs_levels[i]).c_str(), selected))
                             {
                                 state_.SetSkyMcsIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 }, !mcs_levels.empty());
        row_separator(false);

        draw_row(is_cn ? "\u81ea\u9002\u5e94\u94fe\u8def" : "Adaptive Link", "ADAPT", [&]
                 {
                     bool enabled = state_.AdaptiveLinkEnabled();
                     if (ui::Toggle("##adaptive_link", &enabled, m.k))
                     {
                         state_.SetAdaptiveLinkEnabled(enabled);
                     }
                 });
    }
    else if (rail_group_ == 1) // 图传
    {
        draw_row(is_cn ? "\u5929\u7a7a\u7aef\u89c6\u9891" : "Air Video", "AIR", [&]
                 {
                     if (!sky_modes.empty() &&
                         ImGui::BeginCombo("##sky_mode", FormatVideoModeCompact(sky_modes[state_.SkyModeIndex()]).c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(sky_modes.size()); ++i)
                         {
                             bool selected = (state_.SkyModeIndex() == i);
                             if (ImGui::Selectable(FormatVideoModeLabel(sky_modes[i]).c_str(), selected))
                             {
                                 state_.SetSkyModeIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 }, !sky_modes.empty());
        row_separator(false);

        // 必须保留 >60Hz 的确认链：先记 pending，弹窗确认后才写 state。
        draw_row(is_cn ? "\u5730\u9762\u7aef\u89c6\u9891" : "Ground Video", "GND", [&]
                 {
                     if (!ground_modes.empty() &&
                         ImGui::BeginCombo("##ground_mode", FormatVideoModeCompact(ground_modes[state_.GroundModeIndex()]).c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(ground_modes.size()); ++i)
                         {
                             bool selected = (state_.GroundModeIndex() == i);
                             if (ImGui::Selectable(FormatVideoModeLabel(ground_modes[i]).c_str(), selected))
                             {
                                 const auto &mode = ground_modes[i];
                                 const bool requires_warning = (mode.refresh > 60) && !state_.IsGroundModePersisted(mode.label);
                                 if (requires_warning)
                                 {
                                     pending_high_refresh_index_ = i;
                                     pending_high_refresh_label_ = mode.label;
                                     high_refresh_popup_pending_ = true;
                                 }
                                 else
                                 {
                                     state_.SetGroundModeIndex(i);
                                 }
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 }, !ground_modes.empty());
        row_separator(false);

        draw_row(is_cn ? "\u7f13\u5b58\u7ea7\u522b" : "Buffer Level", "BUFFER", [&]
                 {
                     int idx = state_.BufferLevelIndex();
                     if (idx < 0 || idx >= static_cast<int>(buffer_levels.size()))
                     {
                         idx = 0;
                     }
                     const std::string label = std::to_string(buffer_levels[idx]);
                     if (ImGui::BeginCombo("##buffer_level", label.c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(buffer_levels.size()); ++i)
                         {
                             const std::string item_label = std::to_string(buffer_levels[i]);
                             bool selected = (idx == i);
                             if (ImGui::Selectable(item_label.c_str(), selected))
                             {
                                 state_.SetBufferLevelIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 }, !buffer_levels.empty());
        row_separator(false);

        draw_row(is_cn ? "\u574f\u5305\u7b56\u7565" : "Bad Frame Policy", "BAD FRAME", [&]
                 {
                     const char *label_cn[] = {"\u4e22\u5305", "\u5ffd\u7565"};
                     const char *label_en[] = {"Drop", "Ignore"};
                     const char *current = (state_.BadFrameIndex() == 0)
                                               ? (is_cn ? label_cn[0] : label_en[0])
                                               : (is_cn ? label_cn[1] : label_en[1]);
                     if (ImGui::BeginCombo("##bad_frame", current))
                     {
                         for (int i = 0; i < 2; ++i)
                         {
                             const char *label = is_cn ? label_cn[i] : label_en[i];
                             bool selected = (state_.BadFrameIndex() == i);
                             if (ImGui::Selectable(label, selected))
                             {
                                 state_.SetBadFrameIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 });
    }
    else if (rail_group_ == 2) // 网络
    {
        // 选 ConnectAp 只置弹窗标志，绝不直接写 state（与原实现一致）。
        draw_row(is_cn ? "\u7f51\u7edc\u6a21\u5f0f" : "Network Mode", "MODE", [&]
                 {
                     int idx = state_.NetworkModeIndex();
                     if (idx < 0 || idx >= static_cast<int>(network_modes.size()))
                         idx = 0;
                     const char *label = (idx == static_cast<int>(MenuState::NetworkMode::ConnectAp))
                                             ? (is_cn ? "\u8fde\u63a5\u70ed\u70b9" : "Connect AP")
                                             : "WFB";
                     if (ImGui::BeginCombo("##network_mode", label))
                     {
                         for (int i = 0; i < static_cast<int>(network_modes.size()); ++i)
                         {
                             const char *item_label = (i == static_cast<int>(MenuState::NetworkMode::ConnectAp))
                                                          ? (is_cn ? "\u8fde\u63a5\u70ed\u70b9" : "Connect AP")
                                                          : "WFB";
                             bool selected = (idx == i);
                             if (ImGui::Selectable(item_label, selected))
                             {
                                 if (i == static_cast<int>(MenuState::NetworkMode::ConnectAp))
                                 {
                                     wifi_client_popup_pending_ = true;
                                 }
                                 else if (i != idx)
                                 {
                                     state_.SetNetworkModeIndex(i);
                                 }
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 }, !network_modes.empty());
        row_separator(false);

        draw_row(is_cn ? "\u7f16\u8f91\u70ed\u70b9\u914d\u7f6e" : "Edit AP Config", "AP CONFIG", [&]
                 {
                     const char *label = is_cn ? "\u7f16\u8f91\u70ed\u70b9\u914d\u7f6e" : "Edit AP Config";
                     if (ImGui::Button(label, ImVec2(ctrl_w, m.ctrl_h)))
                     {
                         wifi_client_popup_pending_ = true;
                     }
                 });
    }
    else if (rail_group_ == 3) // 显示与声音
    {
        // 必须保留 HDMI 色彩确认链：先记 pending，弹窗确认后才写 state。
        draw_row(is_cn ? "HDMI \u8272\u5f69" : "HDMI Colorspace", "HDMI", [&]
                 {
                     int idx = state_.HdmiAttrIndex();
                     if (idx < 0 || idx >= static_cast<int>(hdmi_attrs.size()))
                         idx = 0;
                     const std::string value = hdmi_attrs[idx];
                     const char *label = nullptr;
                     if (value == "rgb")
                     {
                         label = "RGB";
                     }
                     else if (value == "422")
                     {
                         label = "YUV422";
                     }
                     else
                     {
                         label = "YUV420";
                     }
                     if (ImGui::BeginCombo("##hdmi_attr", label))
                     {
                         for (int i = 0; i < static_cast<int>(hdmi_attrs.size()); ++i)
                         {
                             const std::string item_value = hdmi_attrs[i];
                             const char *item_label = nullptr;
                             if (item_value == "rgb")
                             {
                                 item_label = "RGB";
                             }
                             else if (item_value == "422")
                             {
                                 item_label = "YUV422";
                             }
                             else
                             {
                                 item_label = "YUV420";
                             }
                             bool selected = (idx == i);
                             if (ImGui::Selectable(item_label, selected))
                             {
                                 if (i != idx)
                                 {
                                     pending_hdmi_attr_index_ = i;
                                     pending_hdmi_attr_value_ = item_value;
                                     hdmi_attr_popup_pending_ = true;
                                 }
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 }, !hdmi_attrs.empty());
        row_separator(false);

        draw_row(is_cn ? "\u58f0\u97f3\u5f00\u5173" : "Sound", "SOUND", [&]
                 {
                     bool enabled = state_.SoundEnabled();
                     if (ui::Toggle("##sound_enable", &enabled, m.k))
                     {
                         state_.SetSoundEnabled(enabled);
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u97f3\u91cf" : "Volume", "VOLUME", [&]
                 {
                     int volume_index = state_.SoundVolumeIndex();
                     if (volume_index < 0 || volume_index >= static_cast<int>(volume_levels.size()))
                     {
                         volume_index = std::max(0, std::min(static_cast<int>(volume_levels.size()) - 1, volume_index));
                     }
                     const std::string label = std::to_string(volume_levels[volume_index]) + "%";
                     if (ImGui::BeginCombo("##sound_volume", label.c_str()))
                     {
                         for (int i = 0; i < static_cast<int>(volume_levels.size()); ++i)
                         {
                             const std::string item_label = std::to_string(volume_levels[i]) + "%";
                             bool selected = (volume_index == i);
                             if (ImGui::Selectable(item_label.c_str(), selected))
                             {
                                 state_.SetSoundVolumeIndex(i);
                             }
                             if (selected)
                                 ImGui::SetItemDefaultFocus();
                         }
                         ImGui::EndCombo();
                     }
                 }, !volume_levels.empty());
    }
    else // 系统
    {
        // 原「CC / 官方」两个 RadioButton 合并为一行下拉，仍只调 SetFirmwareType()。
        draw_row(is_cn ? "\u56fa\u4ef6\u6a21\u5f0f" : "Firmware", "FIRMWARE", [&]
                 {
                     const bool is_cc = state_.GetFirmwareType() == MenuState::FirmwareType::CCEdition;
                     const char *current = is_cc ? "CC (UDP)" : (is_cn ? "\u5b98\u65b9 (SSH)" : "Official (SSH)");
                     if (ImGui::BeginCombo("##firmware", current))
                     {
                         if (ImGui::Selectable("CC (UDP)", is_cc) && !is_cc)
                         {
                             state_.SetFirmwareType(MenuState::FirmwareType::CCEdition);
                         }
                         if (is_cc)
                             ImGui::SetItemDefaultFocus();
                         if (ImGui::Selectable(is_cn ? "\u5b98\u65b9 (SSH)" : "Official (SSH)", !is_cc) && is_cc)
                         {
                             state_.SetFirmwareType(MenuState::FirmwareType::Official);
                         }
                         if (!is_cc)
                             ImGui::SetItemDefaultFocus();
                         ImGui::EndCombo();
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u8bed\u8a00" : "Language", "LANG", [&]
                 {
                     const char *label = state_.GetLanguage() == MenuState::Language::CN ? "\u4e2d\u6587" : "English";
                     if (ImGui::BeginCombo("##lang", label))
                     {
                         if (ImGui::Selectable("\u4e2d\u6587", state_.GetLanguage() == MenuState::Language::CN))
                         {
                             state_.SetLanguage(MenuState::Language::CN);
                         }
                         if (ImGui::Selectable("English", state_.GetLanguage() == MenuState::Language::EN))
                         {
                             state_.SetLanguage(MenuState::Language::EN);
                         }
                         ImGui::EndCombo();
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u7ec8\u7aef" : "Terminal", "TERM", [&]
                 {
                     if (!toggle_terminal_)
                     {
                         ImGui::Dummy(ImVec2(ctrl_w, m.ctrl_h));
                         return;
                     }
                     const char *label = (terminal_visible_ && terminal_visible_())
                                             ? (is_cn ? "\u5173\u95ed\u7ec8\u7aef" : "Hide Terminal")
                                             : (is_cn ? "\u6253\u5f00\u7ec8\u7aef" : "Open Terminal");
                     if (ImGui::Button(label, ImVec2(ctrl_w, m.ctrl_h)))
                     {
                         toggle_terminal_();
                     }
                 }, static_cast<bool>(toggle_terminal_));
        row_separator(false);

        // 录像中按钮文字用 kDanger（全局唯一的危险色）。
        draw_row(is_cn ? "\u5f55\u50cf" : "Recording", "REC", [&]
                 {
                     const bool rec = state_.Recording();
                     const char *label = rec ? (is_cn ? "\u505c\u6b62\u5f55\u50cf" : "Stop Recording")
                                             : (is_cn ? "\u5f00\u542f\u5f55\u50cf" : "Start Recording");
                     if (rec)
                         ImGui::PushStyleColor(ImGuiCol_Text, ui::kDanger);
                     if (ImGui::Button(label, ImVec2(ctrl_w, m.ctrl_h)))
                     {
                         state_.ToggleRecording();
                     }
                     if (rec)
                         ImGui::PopStyleColor();
                 });
        row_separator(false);

        draw_row(is_cn ? "\u66f4\u65b0\u56fa\u4ef6" : "Update Firmware", "UPDATE", [&]
                 {
                     if (ImGui::Button(is_cn ? "\u66f4\u65b0\u56fa\u4ef6" : "Update", ImVec2(ctrl_w, m.ctrl_h)))
                     {
                         namespace fs = std::filesystem;
                         std::string found_path;
                         fs::path flash_path("/flash/update_fpv.tar.gz");
                         std::error_code ec;
                         if (fs::exists(flash_path, ec))
                         {
                             found_path = flash_path.string();
                         }
                         else
                         {
                             fs::path media_root("/media");
                             if (fs::exists(media_root, ec))
                             {
                                 for (const auto &entry : fs::directory_iterator(media_root, ec))
                                 {
                                     if (!entry.is_directory())
                                         continue;
                                     fs::path candidate = entry.path() / "update_fpv.tar.gz";
                                     if (fs::exists(candidate, ec))
                                     {
                                         found_path = candidate.string();
                                         break;
                                     }
                                 }
                             }
                         }
                         if (!found_path.empty())
                         {
                             update_path_ = found_path;
                             update_popup_pending_ = true;
                         }
                         else
                         {
                             update_missing_popup_pending_ = true;
                         }
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u6253\u5f00 KODI" : "Open KODI", "MEDIA", [&]
                 {
                     if (ImGui::Button(is_cn ? "\u6253\u5f00 KODI" : "Open KODI", ImVec2(ctrl_w, m.ctrl_h)))
                     {
                         kodi_popup_requested = true;
                     }
                 });
        row_separator(false);

        draw_row(is_cn ? "\u542f\u52a8\u5230\u5b89\u5353" : "Boot to Android", "ANDROID", [&]
                 {
                     ImGui::PushStyleColor(ImGuiCol_Text, ui::kDanger);
                     if (ImGui::Button(is_cn ? "\u542f\u52a8\u5230\u5b89\u5353" : "Boot to Android", ImVec2(ctrl_w, m.ctrl_h)))
                     {
                         std::system("/sbin/rebootfromnand;reboot");
                         running_flag = false;
                     }
                     ImGui::PopStyleColor();
                 });
    }

    // 卡片描边最后画：行数由 row_no 实际累计得到，不会与硬编码行数漂移。
    const float card_y1 = rows_top + m.row_h * static_cast<float>(row_no) + card_pad_y;
    dl->AddRect(ImVec2(card_x0, card_y0), ImVec2(card_x1, card_y1), ui::kAmberLine, 0.0f, 0, 1.0f);
    ui::CornerBrackets(dl, ImVec2(card_x0, card_y0), ImVec2(card_x1, card_y1), 24.0f * m.k, ui::kAmber, 1.5f);

    // 记住本组停留的行，供 Left/Right 进出面板时恢复；偏移按焦点链计算，不假设行号从 0 起。
    if (panel_first_row_ >= 0 && focused_index >= panel_first_row_)
    {
        const int local = focused_index - panel_first_row_;
        panel_row_memory_[rail_group_] = (local < row_no) ? local : (row_no > 0 ? row_no - 1 : 0);
    }
    if (panel_row_memory_[rail_group_] >= row_no)
    {
        panel_row_memory_[rail_group_] = (row_no > 0) ? (row_no - 1) : -1;
    }
}

void MenuRenderer::DrawMenu(const ImGuiViewport *viewport, bool &running_flag)
{
    const ui::Metrics m = ui::ComputeMetrics(viewport);
    const bool is_cn = state_.GetLanguage() == MenuState::Language::CN;
    bool kodi_popup_requested = false;

    if (!menu_visible_last_)
    {
        // 菜单重新打开：清空权威光标，回到「无焦点」状态，第一次按方向键只建立焦点。
        nav_cursor_ = -1;
        pending_focus_index_ = -1;
    }
    menu_visible_last_ = true;
    if (update_status_)
    {
        int status = update_status_();
        if (status != update_status_last_)
        {
            if (status == 2)
            {
                update_reboot_popup_pending_ = true;
            }
            else if (status == 3)
            {
                update_failed_popup_pending_ = true;
            }
            update_status_last_ = status;
        }
    }

    ui::PushMenuStyle();
    // 贴边：左/上/下贴满视口，宽度 = 图标栏 + 面板。面板 100% 不透明，不再调 SetNextWindowBgAlpha。
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(m.panel_x + m.panel_w, m.panel_h), ImGuiCond_Always);

    // NoNavInputs 是这套语义导航的前提：它让 ImGui 完全不再处理方向键，
    // 焦点只能通过 SetKeyboardFocusHere() 程序化设置，从而不会被 ImGui 的 2D 空间导航抢走。
    // 代价是 Enter/Space 的「按焦点激活」也一并失效，故在 DrawGroupRows 里自行派发 Enter。
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoNavInputs;

    if (ImGui::Begin("GS Control Menu", nullptr, flags))
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 win = ImGui::GetWindowPos();

        const bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool nav_down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false);
        const bool nav_up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false);
        // Enter 本帧是否被按下。注意：不能用 IsKeyPressed(..., repeat=false) 之外的过滤，
        // 这里只在「刚按下」的那一帧为真，避免长按连发把按钮连点。
        const bool enter_pressed = !popup_open &&
                                   (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                    ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));

        // 权威光标是唯一事实来源：rail_group_ 由它推导，避免两个状态互相打架。
        // （光标在图标栏上时，当前分组就是光标所在项；光标在面板/动作条时保持原分组不变。）
        if (nav_cursor_ >= 0 && nav_cursor_ <= 4)
        {
            rail_group_ = nav_cursor_;
        }

        // (a) 焦点注册器：单链，无列概念。注册顺序 == 视觉顺序。
        int focus_index = 0;
        int focused_index = -1;
        int last_index = 0;
        auto register_focus = [&](const std::function<void()> &draw)
        {
            const bool want = (pending_focus_index_ == focus_index);
            if (want)
            {
                ImGui::SetKeyboardFocusHere();
            }
            draw();
            if (want)
            {
                pending_focus_index_ = -1;
            }
            if (ImGui::IsItemFocused())
            {
                focused_index = focus_index;
                // NoNavInputs 关掉了 ImGui 的「按焦点激活」，这里自己补：焦点项收到 Enter 时
                // 用 ActivateItemByID 复刻一次真实按下。
                // 仅当该控件本帧没有自己消费 Enter 才派发 —— 展开中的 Combo 会用 Enter 确认
                // 选中项，此时必须让它自己处理，否则会「选完又立刻重新打开」。
                if (enter_pressed && !ImGui::IsItemActivated() && ImGui::GetItemID() != 0)
                {
                    ImGui::ActivateItemByID(ImGui::GetItemID());
                }
            }
            ++focus_index;
            last_index = focus_index - 1;
        };

        // (b) 左侧图标栏
        // 两趟：先提交全部 item 确定焦点/选中组，再统一绘制，避免焦点变化当帧双高亮。
        dl->AddRectFilled(ImVec2(win.x, win.y), ImVec2(win.x + m.rail_w, win.y + m.panel_h), ui::kBlack);
        SubmitRailItems(m, register_focus);
        // 第二趟：此时 rail_group_ 已确定，画出来的描边/竖条就是本帧最终状态。
        for (int g = 0; g < 5; ++g)
        {
            DrawRailItem(g, m);
        }

        // (c) 面板头部：六边形徽章 + 中文标题 + 英文副标题 + 分段进度条
        const float header_top = win.y + kPanelHeaderTop * m.k;
        const float content_x = win.x + m.panel_x + m.pad;
        const float content_w = m.panel_w - 2.0f * m.pad;

        const ImVec2 badge_c(content_x + 13.0f * m.k, header_top + 11.0f * m.k);
        ui::Hexagon(dl, badge_c, 13.0f * m.k, 11.0f * m.k, ui::kAmber, 1.0f);
        {
            char num[4];
            std::snprintf(num, sizeof(num), "%02d", rail_group_ + 1);
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(m.en_sz, 1.0e9f, 0.0f, num);
            dl->AddText(ImGui::GetFont(), m.en_sz,
                        ImVec2(badge_c.x - ts.x * 0.5f, badge_c.y - ts.y * 0.5f), ui::kAmber, num);
        }

        static const char *kGroupCn[5] = {
            "\u94fe\u8def", "\u56fe\u4f20", "\u7f51\u7edc", "\u663e\u793a\u4e0e\u58f0\u97f3", "\u7cfb\u7edf"};
        static const char *kGroupEn[5] = {"LINK CONTROL", "VIDEO LINK", "NETWORK", "DISPLAY", "SYSTEM"};
        const float title_x = content_x + 34.0f * m.k;
        dl->AddText(ImGui::GetFont(), m.title_sz, ImVec2(title_x, header_top + 1.0f * m.k),
                    ui::kAmberLabel, kGroupCn[rail_group_]);
        dl->AddText(ImGui::GetFont(), m.en_sz, ImVec2(title_x, header_top + m.title_sz + 4.0f * m.k),
                    ui::kAmberDim, kGroupEn[rail_group_]);
        ui::SegmentBar(dl, ImVec2(content_x, header_top + 78.0f * m.k), content_w,
                       3.0f * m.k, 18, 13, ui::kAmber);

        // (d) 分组卡片 + 行
        DrawGroupRows(m, focus_index, focused_index, register_focus,
                      kodi_popup_requested, running_flag);

        // (e) 底部动作条
        const float bar_y = win.y + m.panel_h - kPanelBottomBar * m.k;
        dl->AddLine(ImVec2(win.x + m.panel_x, bar_y),
                    ImVec2(win.x + m.panel_x + m.panel_w, bar_y), ui::kAmberLine, 1.0f);
        dl->AddText(ImGui::GetFont(), m.en_sz,
                    ImVec2(content_x, bar_y + 20.0f * m.k), ui::kAmberDim, "STATE 0.78");

        const float btn_w = 120.0f * m.k;
        const float btn_h = 40.0f * m.k;
        const ImVec2 btn_pos(win.x + m.panel_x + m.panel_w - m.pad - btn_w,
                             bar_y + (kPanelBottomBar * m.k - btn_h) * 0.5f);
        ImGui::SetCursorScreenPos(btn_pos);
        ImGui::PushStyleColor(ImGuiCol_Button, ui::kAmber);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::kAmberData);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::kAmberLabel);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kInk);
        register_focus([&]
                       {
                           if (ImGui::Button(is_cn ? "\u5b8c\u6210" : "EXEC", ImVec2(btn_w, btn_h)))
                           {
                               state_.ToggleMenuVisibility();
                           }
                       });
        ImGui::PopStyleColor(4);

        // (f) 焦点后处理：完全显式的语义导航
        //
        // 菜单窗口带 NoNavInputs，ImGui 不再处理方向键，所以这里就是唯一的导航实现。
        // 移动基准是 nav_cursor_（本类权威光标），而不是 focused_index —— 后者是 ImGui 的
        // 状态，在多控件窗口里会被 2D 空间导航污染（历史缺陷：rail#1 永远够不到）。
        //
        // 语义表：
        //   图标栏(0..4)  Down=下一组(环绕)  Up=上一组(环绕)  Right/Enter=进面板首行  Left=关菜单
        //   面板行        Down=下一行(末行→图标栏当前组)     Up=上一行(首行→图标栏当前组)
        //                 Right=忽略  Left=回图标栏  Enter=交给控件自己
        //   动作条「完成」Down=环绕到图标栏#0  Up=面板末行  Right/Left=忽略  Enter=关菜单
        const bool nav_left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false);
        const bool nav_right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
        const bool nav_enter = enter_pressed;
        auto swallow_nav_key = [&](ImGuiKey key)
        {
            ImGuiIO &io = ImGui::GetIO();
            ImGuiKeyData &kd = io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
            kd.Down = false;
            kd.DownDuration = -1.0f;
            kd.DownDurationPrev = -1.0f;
        };

        // 动作条「完成」按钮的焦点序号 = 链尾
        const int action_index = (panel_first_row_ >= 0) ? last_index : 4;

        if (!popup_open && (nav_down || nav_up || nav_left || nav_right || nav_enter))
        {
            // 无焦点时按任意方向键：建立焦点，停在图标栏第 0 项。
            if (nav_cursor_ < 0)
            {
                rail_group_ = 0;
                pending_focus_index_ = 0;
                swallow_nav_key(nav_down ? ImGuiKey_DownArrow
                                         : nav_up ? ImGuiKey_UpArrow
                                         : nav_left ? ImGuiKey_LeftArrow
                                         : nav_right ? ImGuiKey_RightArrow
                                                     : ImGuiKey_Enter);
            }
            else
            {
                const bool on_rail = (nav_cursor_ <= 4);
                const bool on_action = (nav_cursor_ == action_index) && (action_index > 4);
                const bool on_row = (panel_first_row_ >= 0) && (nav_cursor_ >= panel_first_row_) &&
                                    (nav_cursor_ < action_index);

                if (on_rail)
                {
                    if (nav_down)
                    {
                        rail_group_ = (rail_group_ + 1) % 5;
                        pending_focus_index_ = rail_group_;
                        swallow_nav_key(ImGuiKey_DownArrow);
                    }
                    else if (nav_up)
                    {
                        rail_group_ = (rail_group_ + 4) % 5;
                        pending_focus_index_ = rail_group_;
                        swallow_nav_key(ImGuiKey_UpArrow);
                    }
                    else if (nav_right || nav_enter)
                    {
                        // 进面板首行（优先回到该组上次停留的行）
                        if (panel_first_row_ >= 0)
                        {
                            const int mem = panel_row_memory_[rail_group_];
                            int target = panel_first_row_ + (mem > 0 ? mem : 0);
                            if (target > action_index - 1)
                                target = action_index - 1;
                            pending_focus_index_ = target;
                        }
                        swallow_nav_key(nav_right ? ImGuiKey_RightArrow : ImGuiKey_Enter);
                    }
                    else if (nav_left)
                    {
                        state_.ToggleMenuVisibility();
                        swallow_nav_key(ImGuiKey_LeftArrow);
                    }
                }
                else if (on_row)
                {
                    if (nav_down)
                    {
                        if (nav_cursor_ + 1 < action_index)
                            pending_focus_index_ = nav_cursor_ + 1;
                        else
                            pending_focus_index_ = action_index; // 末行 -> 动作条
                        swallow_nav_key(ImGuiKey_DownArrow);
                    }
                    else if (nav_up)
                    {
                        if (nav_cursor_ - 1 >= panel_first_row_)
                            pending_focus_index_ = nav_cursor_ - 1;
                        else
                            pending_focus_index_ = rail_group_; // 首行 -> 回图标栏当前组
                        swallow_nav_key(ImGuiKey_UpArrow);
                    }
                    else if (nav_left)
                    {
                        pending_focus_index_ = rail_group_;
                        swallow_nav_key(ImGuiKey_LeftArrow);
                    }
                    // nav_right：忽略；nav_enter：交给控件自己（见 DrawGroupRows）
                }
                else if (on_action)
                {
                    if (nav_down)
                    {
                        pending_focus_index_ = 0; // 环绕到图标栏 #0
                        swallow_nav_key(ImGuiKey_DownArrow);
                    }
                    else if (nav_up)
                    {
                        pending_focus_index_ = (action_index - 1 >= panel_first_row_) ? action_index - 1
                                                                                      : panel_first_row_;
                        swallow_nav_key(ImGuiKey_UpArrow);
                    }
                    else if (nav_enter)
                    {
                        // 动作条「完成」的 Enter 由 register_focus 里的 ActivateItemByID 派发，
                        // 这里不能再触发一次，否则一按 Enter 会连开两次菜单。
                        swallow_nav_key(ImGuiKey_Enter);
                    }
                }
            }

            // 每次处理完按键，把权威光标同步到目标。
            // 注意：pending 被 reg 消费后会置回 -1，所以目标在按键处理当时就要记下来，
            // 这里用「按键已处理」标志判断，避免把刚设好的目标又用 -1 覆盖掉。
            if (pending_focus_index_ >= 0)
            {
                nav_cursor_ = pending_focus_index_;
            }
        }

        // 目标在本帧的焦点链里不存在（切组后行数变少等）时丢弃，避免 pending 悬挂导致焦点丢失。
        if (pending_focus_index_ > last_index)
        {
            pending_focus_index_ = -1;
            nav_cursor_ = -1;
        }

        if (high_refresh_popup_pending_ && pending_high_refresh_index_ >= 0)
        {
            ImGui::OpenPopup("confirm_high_refresh");
            high_refresh_popup_pending_ = false;
        }
        if (high_refresh_persist_popup_pending_)
        {
            ImGui::OpenPopup("confirm_high_refresh_persist");
            high_refresh_persist_popup_pending_ = false;
        }
        if (hdmi_attr_popup_pending_ && pending_hdmi_attr_index_ >= 0)
        {
            ImGui::OpenPopup("confirm_hdmi_attr");
            hdmi_attr_popup_pending_ = false;
        }
        if (hdmi_attr_persist_popup_pending_)
        {
            ImGui::OpenPopup("confirm_hdmi_attr_persist");
            hdmi_attr_persist_popup_pending_ = false;
        }
        if (wifi_client_popup_pending_)
        {
            ImGui::OpenPopup("configure_wifi_client");
            wifi_client_popup_pending_ = false;
        }

        if (ui::BeginDialog("confirm_high_refresh", ui::DialogSize::Medium))
        {
            ui::DialogTitle(is_cn ? "\u5237\u65b0\u7387\u8fc7\u9ad8" : "HIGH REFRESH RATE",
                            is_cn ? "HIGH REFRESH RATE" : nullptr);
            const char *msg = is_cn ? "\u8be5\u5206\u8fa8\u7387\u5237\u65b0\u7387\u8d85\u8fc760Hz\uff0c\u53ef\u80fd\u5bfc\u81f4\u9ed1\u5c4f\uff0c\u662f\u5426\u7ee7\u7eed\uff1f"
                                    : "This refresh rate exceeds 60Hz and may cause a black screen. Continue?";
            ui::TextWrappedCJK(msg);
            ImGui::Spacing();
            const int choice = ui::DialogButtons(is_cn ? "\u53d6\u6d88" : "Cancel",
                                                 is_cn ? "\u7ee7\u7eed" : "Continue");
            if (choice == 1)
            {
                pending_high_refresh_index_ = -1;
                pending_high_refresh_label_.clear();
                ImGui::CloseCurrentPopup();
            }
            else if (choice == 2)
            {
                if (pending_high_refresh_index_ >= 0)
                {
                    state_.RequestGroundModeSkipSaveOnce();
                    state_.SetGroundModeIndex(pending_high_refresh_index_);
                    pending_high_refresh_index_ = -1;
                }
                high_refresh_persist_popup_pending_ = true;
                ImGui::CloseCurrentPopup();
            }
            ui::EndDialog();
        }

        if (ui::BeginDialog("confirm_high_refresh_persist", ui::DialogSize::Small))
        {
            ui::DialogTitle(is_cn ? "\u4fdd\u5b58\u8bbe\u7f6e" : "PERSIST SETTING",
                            is_cn ? "PERSIST SETTING" : nullptr);
            const char *msg = is_cn ? "\u662f\u5426\u4ee5\u540e\u9ed8\u8ba4\u4f7f\u7528\u8be5\u9ad8\u5237\u65b9\u6848\uff1f"
                                    : "Use this high refresh rate by default in the future?";
            ui::TextWrappedCJK(msg);
            ImGui::Spacing();
            const int choice = ui::DialogButtons(is_cn ? "\u5426" : "No",
                                                 is_cn ? "\u662f" : "Yes");
            if (choice == 1)
            {
                pending_high_refresh_label_.clear();
                ImGui::CloseCurrentPopup();
            }
            else if (choice == 2)
            {
                if (!pending_high_refresh_label_.empty())
                {
                    state_.RequestGroundModeForceSaveOnce();
                    state_.ForceGroundModeNotifyOnce();
                    state_.SetGroundModeIndex(state_.GroundModeIndex());
                    state_.SetGroundModePersisted(pending_high_refresh_label_, true);
                    pending_high_refresh_label_.clear();
                }
                ImGui::CloseCurrentPopup();
            }
            ui::EndDialog();
        }

        if (ui::BeginDialog("confirm_hdmi_attr", ui::DialogSize::Medium))
        {
            ui::DialogTitle(is_cn ? "\u8272\u5f69\u683c\u5f0f\u5207\u6362" : "COLORSPACE CHANGE",
                            is_cn ? "COLORSPACE CHANGE" : nullptr);
            const char *msg = is_cn ? "\u5207\u6362 HDMI \u8272\u5f69\u683c\u5f0f\u53ef\u80fd\u5bfc\u81f4\u9ed1\u5c4f\uff0c\u662f\u5426\u7ee7\u7eed\uff1f"
                                    : "Switching HDMI colorspace may cause a black screen. Continue?";
            ui::TextWrappedCJK(msg);
            ImGui::Spacing();
            const int choice = ui::DialogButtons(is_cn ? "\u53d6\u6d88" : "Cancel",
                                                 is_cn ? "\u7ee7\u7eed" : "Continue");
            if (choice == 1)
            {
                pending_hdmi_attr_index_ = -1;
                pending_hdmi_attr_value_.clear();
                ImGui::CloseCurrentPopup();
            }
            else if (choice == 2)
            {
                if (pending_hdmi_attr_index_ >= 0)
                {
                    state_.RequestHdmiAttrSkipSaveOnce();
                    state_.SetHdmiAttrIndex(pending_hdmi_attr_index_);
                    pending_hdmi_attr_index_ = -1;
                }
                hdmi_attr_persist_popup_pending_ = true;
                ImGui::CloseCurrentPopup();
            }
            ui::EndDialog();
        }

        if (ui::BeginDialog("confirm_hdmi_attr_persist", ui::DialogSize::Small))
        {
            ui::DialogTitle(is_cn ? "\u4fdd\u5b58\u8bbe\u7f6e" : "PERSIST SETTING",
                            is_cn ? "PERSIST SETTING" : nullptr);
            const char *msg = is_cn ? "\u662f\u5426\u4ee5\u540e\u9ed8\u8ba4\u4f7f\u7528\u8be5\u8272\u5f69\u683c\u5f0f\uff1f"
                                    : "Use this colorspace by default in the future?";
            ui::TextWrappedCJK(msg);
            ImGui::Spacing();
            const int choice = ui::DialogButtons(is_cn ? "\u5426" : "No",
                                                 is_cn ? "\u662f" : "Yes");
            if (choice == 1)
            {
                pending_hdmi_attr_value_.clear();
                ImGui::CloseCurrentPopup();
            }
            else if (choice == 2)
            {
                state_.RequestHdmiAttrForceSaveOnce();
                state_.ForceHdmiAttrNotifyOnce();
                state_.SetHdmiAttrIndex(state_.HdmiAttrIndex());
                pending_hdmi_attr_value_.clear();
                ImGui::CloseCurrentPopup();
            }
            ui::EndDialog();
        }

        // ================= Wi-Fi 连接向导 =================
        // 步骤 0：选网卡 -> 1：扫描/选 AP -> 2：输密码 -> 3：连接结果
        if (ui::BeginDialog("configure_wifi_client", ui::DialogSize::Large))
        {
            ui::DialogTitle(is_cn ? "\u8fde\u63a5\u70ed\u70b9" : "CONNECT TO AP",
                            is_cn ? "CONNECT TO AP" : nullptr);

            // 每帧刷新当前网卡的实时状态（轻量：读 iw link）
            wifi_status_timer_ += ImGui::GetIO().DeltaTime;
            if (wifi_status_timer_ > 1.0f)
            {
                wifi_status_timer_ = 0.0f;
                if (!wifi_.Iface().empty())
                    wifi_live_status_ = WifiController::QueryStatus(wifi_.Iface());
            }

            // 后台扫描结果
            {
                std::vector<WifiAp> got;
                bool scanning = false;
                if (wifi_.PollScan(&got, &scanning))
                {
                    wifi_aps_ = std::move(got);
                    // 重新扫描后 AP 数量可能变少，钳制选中项，防越界
                    if (wifi_ap_idx_ >= static_cast<int>(wifi_aps_.size()))
                        wifi_ap_idx_ = 0;
                }
                wifi_scanning_ = scanning;
            }
            // 后台连接结果
            {
                bool ok = false, running = false;
                std::string msg;
                if (wifi_.PollConnect(&ok, &msg, &running))
                {
                    wifi_connecting_ = false;
                    wifi_result_ok_ = ok;
                    wifi_result_msg_ = msg;
                    wifi_has_result_ = true;
                    wifi_step_ = 3;
                }
                else
                {
                    wifi_connecting_ = running;
                }
            }

            // ---- 步骤指示 ----
            {
                const char *steps_cn[] = {"1 \u9009\u7f51\u5361", "2 \u9009\u70ed\u70b9", "3 \u5bc6\u7801", "4 \u8fde\u63a5"};
                const char *steps_en[] = {"1 Iface", "2 AP", "3 Password", "4 Connect"};
                const char **steps = is_cn ? steps_cn : steps_en;
                for (int i = 0; i < 4; ++i)
                {
                    if (i)
                        ImGui::SameLine();
                    bool cur = (i == wifi_step_);
                    bool done = (i < wifi_step_);
                    // 当前步 A0 实心琥珀，已完成 A1 数据色，未开始 A2 弱化 —— 与整机三档琥珀语言一致
                    ImU32 step_col = cur ? ui::kAmber : (done ? ui::kAmberData : ui::kAmberDim);
                    ImGui::PushStyleColor(ImGuiCol_Text, step_col);
                    ImGui::TextUnformatted(steps[i]);
                    ImGui::PopStyleColor();
                }
            }
            ImGui::Separator();
            ImGui::Spacing();

            // 步骤 1/2 的按钮超过两个，无法塞进 DialogButtons（右对齐双键）。
            // 这里用与 DialogButtons 完全相同的尺寸与配色手绘，保持整机观感统一。
            const ui::Metrics dm = ui::ComputeMetrics(ImGui::GetMainViewport());
            auto ghost_button = [&](const char *label) -> bool
            {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::kAmberSoft);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::kBtnActive);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kAmberData);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                const bool hit = ImGui::Button(label, ImVec2(112.0f * dm.k, 40.0f * dm.k));
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
                return hit;
            };
            // 主按钮（实心琥珀 + 黑字），用于「下一步 / 连接 / 完成」这类推进动作
            auto primary_button = [&](const char *label, bool enabled = true) -> bool
            {
                if (!enabled)
                    ImGui::BeginDisabled();
                ImGui::PushStyleColor(ImGuiCol_Button, ui::kAmber);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::kAmberData);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::kAmberLabel);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kInk);
                const bool hit = ImGui::Button(label, ImVec2(132.0f * dm.k, 40.0f * dm.k));
                ImGui::PopStyleColor(4);
                if (!enabled)
                    ImGui::EndDisabled();
                return hit;
            };

            // ---- 步骤 0：选网卡 ----
            if (wifi_step_ == 0)
            {
                ui::TextWrappedCJK(is_cn
                    ? "\u9009\u62e9\u7528\u4e8e\u8fde\u63a5\u70ed\u70b9\u7684\u65e0\u7ebf\u7f51\u5361\uff08WFB \u865a\u62df\u5361\u4e0d\u5728\u5217\u8868\u4e2d\uff09\u3002"
                    : "Select the wireless interface used to connect the AP (WFB virtual interfaces are excluded).");

                if (wifi_ifaces_.empty())
                {
                    wifi_ifaces_ = WifiController::ListIfaces();
                    if (wifi_iface_idx_ >= static_cast<int>(wifi_ifaces_.size()))
                        wifi_iface_idx_ = 0;
                }

                ImGui::Spacing();
                if (wifi_ifaces_.empty())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
                        is_cn ? "\u6ca1\u6709\u627e\u5230\u53ef\u7528\u7684\u65e0\u7ebf\u7f51\u5361" : "No usable wireless interface");
                    ImGui::Spacing();
                    if (ImGui::Button(is_cn ? "\u5237\u65b0" : "Refresh", ImVec2(120, 0)))
                        wifi_ifaces_ = WifiController::ListIfaces();
                }
                else
                {
                    for (int i = 0; i < static_cast<int>(wifi_ifaces_.size()); ++i)
                    {
                        const WifiIface &it = wifi_ifaces_[i];
                        std::string label = it.name + "  [" + it.driver + "]  " + it.bands;
                        if (!it.ssid.empty())
                            label += std::string("  * ") + it.ssid;
                        if (ImGui::RadioButton(label.c_str(), wifi_iface_idx_ == i))
                            wifi_iface_idx_ = i;
                    }
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                bool can_next = !wifi_ifaces_.empty();
                const int choice = ui::DialogButtons(is_cn ? "\u53d6\u6d88" : "Cancel",
                                                     is_cn ? "\u4e0b\u4e00\u6b65" : "Next", can_next);
                if (choice == 2)
                {
                    wifi_.SetIface(wifi_ifaces_[wifi_iface_idx_].name);
                    wifi_aps_.clear();
                    wifi_scan_requested_ = false;
                    wifi_step_ = 1;
                }
                else if (choice == 1)
                {
                    wifi_step_ = 0;
                    ImGui::CloseCurrentPopup();
                }
            }

            // ---- 步骤 1：扫描并选 AP ----
            else if (wifi_step_ == 1)
            {
                ImGui::Text(is_cn ? "\u7f51\u5361\uff1a%s\uff08%s\uff09" : "Interface: %s (%s)",
                            wifi_.Iface().c_str(),
                            wifi_ifaces_.empty() ? "-" : wifi_ifaces_[wifi_iface_idx_].bands.c_str());

                if (!wifi_scan_requested_)
                {
                    wifi_.StartScan(wifi_.Iface());
                    wifi_scan_requested_ = true;
                }

                ImGui::Spacing();
                if (wifi_scanning_)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s",
                        is_cn ? "\u6b63\u5728\u626b\u63cf..." : "Scanning...");
                }
                else if (wifi_aps_.empty())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
                        is_cn ? "\u672a\u626b\u5230\u70ed\u70b9\uff08\u8be5\u7f51\u5361\u9891\u6bb5\u53ef\u80fd\u4e0d\u8986\u76d6\u76ee\u6807 AP\uff09"
                              : "No AP found (this card's band may not cover the target AP)");
                }
                else
                {
                    ImGui::Text("%s %d", is_cn ? "\u626b\u5230" : "Found", static_cast<int>(wifi_aps_.size()));
                }

                ImGui::BeginChild("ap_list", ImVec2(520, 220), true);
                for (int i = 0; i < static_cast<int>(wifi_aps_.size()); ++i)
                {
                    const WifiAp &ap = wifi_aps_[i];
                    // 信号强度条
                    float q = (ap.signal_dbm <= 0) ? ((ap.signal_dbm + 100) / 70.0f)
                                                   : (ap.signal_dbm / 100.0f);
                    if (q < 0.0f) q = 0.0f;
                    if (q > 1.0f) q = 1.0f;
                    ImVec4 sc = q > 0.6f ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                              : q > 0.3f ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                         : ImVec4(1.0f, 0.45f, 0.35f, 1.0f);
                    // ConnMan 的 Strength 是 0-100，不是 dBm
                    int pct = ap.signal_dbm;
                    if (pct < 0) pct = 0;      // 兼容旧格式(-dBm)
                    if (pct > 100) pct = 100;
                    const char *st_tag = "";
                    if (ap.state == "ready" || ap.state == "online")
                        st_tag = "  [\u5df2\u8fde]";
                    else if (ap.state == "failure")
                        st_tag = "  [\u5931\u8d25]";
                    char line[256];
                    std::snprintf(line, sizeof(line), "%s%s   %d%%%s",
                                  ap.ssid.c_str(), ap.secure ? "  [\u9501]" : "",
                                  pct, st_tag);
                    ImGui::PushStyleColor(ImGuiCol_Text, sc);
                    if (ImGui::Selectable(line, wifi_ap_idx_ == i))
                        wifi_ap_idx_ = i;
                    ImGui::PopStyleColor();
                }
                ImGui::EndChild();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ghost_button(is_cn ? "\u91cd\u65b0\u626b\u63cf" : "Rescan"))
                {
                    wifi_scan_requested_ = false;
                    wifi_aps_.clear();
                }
                ImGui::SameLine();
                bool has_ap = !wifi_aps_.empty();
                if (primary_button(is_cn ? "\u4e0b\u4e00\u6b65" : "Next", has_ap))
                {
                    std::snprintf(wifi_client_ssid_, sizeof(wifi_client_ssid_), "%s",
                                  wifi_aps_[wifi_ap_idx_].ssid.c_str());
                    wifi_pwd_focus_ = 1;   // 首次进入密码页就让输入框拿到键盘焦点
                    wifi_step_ = 2;
                }
                ImGui::SameLine();
                if (ghost_button(is_cn ? "\u4e0a\u4e00\u6b65" : "Back"))
                {
                    wifi_step_ = 0;
                    wifi_ifaces_.clear();
                }
                ImGui::SameLine();
                if (ghost_button(is_cn ? "\u53d6\u6d88" : "Cancel"))
                {
                    wifi_step_ = 0;
                    ImGui::CloseCurrentPopup();
                }
            }

            // ---- 步骤 2：输密码 ----
            else if (wifi_step_ == 2)
            {
                ImGui::Text(is_cn ? "\u76ee\u6807\u70ed\u70b9\uff1a%s" : "Target AP: %s", wifi_client_ssid_);
                ImGui::Spacing();
                ImGui::TextUnformatted(is_cn ? "\u5bc6\u7801" : "Password");
                ImGui::SetNextItemWidth(420);
                if (wifi_pwd_focus_ == 1)
                {
                    ImGui::SetKeyboardFocusHere();
                    wifi_pwd_focus_ = 0;
                }
                bool enter = ImGui::InputText("##password", wifi_client_password_,
                                              sizeof(wifi_client_password_),
                                              ImGuiInputTextFlags_Password |
                                              ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::TextDisabled("%s", is_cn
                    ? "\uff08\u5f00\u653e\u70ed\u70b9\u7559\u7a7a\u5373\u53ef\uff09"
                    : "(leave empty for open network)");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (primary_button(is_cn ? "\u8fde\u63a5" : "Connect") || enter)
                {
                    wifi_has_result_ = false;
                    wifi_result_msg_.clear();
                    wifi_step_ = 3;
                    wifi_.StartConnect(wifi_client_ssid_, wifi_client_password_, wifi_.Iface());
                }
                ImGui::SameLine();
                if (ghost_button(is_cn ? "\u4e0a\u4e00\u6b65" : "Back"))
                    wifi_step_ = 1;
                ImGui::SameLine();
                if (ghost_button(is_cn ? "\u53d6\u6d88" : "Cancel"))
                {
                    wifi_step_ = 0;
                    ImGui::CloseCurrentPopup();
                }
            }

            // ---- 步骤 3：连接中 / 结果 ----
            else
            {
                if (wifi_connecting_ || (!wifi_has_result_ && wifi_result_msg_.empty()))
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s",
                        is_cn ? "\u6b63\u5728\u8fde\u63a5..." : "Connecting...");
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", is_cn
                        ? "\u6b63\u5728\u901a\u77e5 ConnMan \u8fde\u63a5\u70ed\u70b9\u5e76\u7b49\u5f85 DHCP\uff0c\u6700\u591a 30 \u79d2"
                        : "Asking ConnMan to connect and waiting for DHCP (up to 30s)");
                }
                else if (wifi_result_ok_)
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s",
                        is_cn ? "\u8fde\u63a5\u6210\u529f" : "Connected");
                    ImGui::Spacing();
                    ui::TextWrappedCJK(wifi_result_msg_.c_str());
                    ImGui::Spacing();
                    ImGui::Text("%s %d dBm", is_cn ? "\u4fe1\u53f7\uff1a" : "Signal:", wifi_live_status_.signal_dbm);
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                        is_cn ? "\u8fde\u63a5\u5931\u8d25" : "Connection failed");
                    ImGui::Spacing();
                    ui::TextWrappedCJK(wifi_result_msg_.c_str());
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                bool done = wifi_has_result_ && !wifi_connecting_;
                const int choice = ui::DialogButtons(is_cn ? "\u91cd\u8bd5" : "Retry",
                                                     is_cn ? "\u5b8c\u6210" : "Done", done);
                if (choice == 1)
                {
                    wifi_step_ = 2;
                    wifi_has_result_ = false;
                    wifi_pwd_focus_ = 1;
                }
                else if (choice == 2)
                {
                    wifi_step_ = 0;
                    wifi_ifaces_.clear();
                    if (wifi_result_ok_)
                    {
                        state_.ForceNetworkModeNotifyOnce();
                        state_.SetNetworkModeIndex(static_cast<int>(MenuState::NetworkMode::ConnectAp));
                    }
                    ImGui::CloseCurrentPopup();
                }
            }

            ui::EndDialog();
        }

        if (kodi_popup_requested)
        {
            kodi_popup_focus_index_ = 0;
            kodi_popup_focus_dirty_ = true;
            ImGui::OpenPopup("confirm_kodi");
        }
        if (ui::BeginDialog("confirm_kodi", ui::DialogSize::Medium))
        {
            ui::DialogTitle(is_cn ? "\u6253\u5f00 KODI" : "LAUNCH KODI",
                            is_cn ? "LAUNCH KODI" : nullptr);
            const char *msg = is_cn ? "\u6253\u5f00 KODI \u5c06\u5173\u95ed\u56fe\u4f20\u7a0b\u5e8f\uff0c\u662f\u5426\u7ee7\u7eed\uff1f"
                                    : "Opening KODI will close the video link process. Continue?";
            ui::TextWrappedCJK(msg);
            ImGui::Spacing();
            // 原先这里自己用 Up/Down 在「取消/确认」间切换焦点。改用 DialogButtons 后，
            // 弹窗内 ImGui 原生导航生效（弹窗不继承主窗口的 NoNavInputs），
            // Left/Right 切焦点、Enter 触发默认主按钮，行为更标准且少一份状态。
            const int choice = ui::DialogButtons(is_cn ? "\u53d6\u6d88" : "Cancel",
                                                 is_cn ? "\u786e\u8ba4" : "Confirm");
            if (choice == 1)
            {
                ImGui::CloseCurrentPopup();
                kodi_popup_focus_index_ = 0;
                kodi_popup_focus_dirty_ = true;
            }
            else if (choice == 2)
            {
                std::system("bash -lc 'systemctl stop amldigitalfpv || true; systemctl start kodi2'"); // restart kodi and exit
                running_flag = false;
                ImGui::CloseCurrentPopup();
                kodi_popup_focus_index_ = 0;
                kodi_popup_focus_dirty_ = true;
            }
            ui::EndDialog();
        }

        if (update_popup_pending_)
        {
            ImGui::OpenPopup("confirm_update");
            update_popup_pending_ = false;
        }
        if (ui::BeginDialog("confirm_update", ui::DialogSize::Medium))
        {
            ui::DialogTitle(is_cn ? "\u66f4\u65b0\u56fa\u4ef6" : "FIRMWARE UPDATE",
                            is_cn ? "FIRMWARE UPDATE" : nullptr);
            const char *msg = is_cn ? "\u53d1\u73b0\u66f4\u65b0\u5305\uff0c\u662f\u5426\u5f00\u59cb\u66f4\u65b0\uff1f"
                                    : "Update package found. Start update?";
            ui::TextWrappedCJK(msg);
            if (!update_path_.empty())
            {
                ui::TextWrappedCJK(update_path_.c_str());
            }
            ImGui::Spacing();
            const int choice = ui::DialogButtons(is_cn ? "\u53d6\u6d88" : "Cancel",
                                                 is_cn ? "\u786e\u8ba4" : "Confirm");
            if (choice == 1)
            {
                ImGui::CloseCurrentPopup();
            }
            else if (choice == 2)
            {
                if (start_update_)
                {
                    start_update_(update_path_);
                }
                ImGui::CloseCurrentPopup();
            }
            ui::EndDialog();
        }

        if (update_missing_popup_pending_)
        {
            ImGui::OpenPopup("update_not_found");
            update_missing_popup_pending_ = false;
        }
        if (ui::BeginDialog("update_not_found", ui::DialogSize::Small))
        {
            ui::DialogTitle(is_cn ? "\u672a\u627e\u5230\u66f4\u65b0" : "NO UPDATE FOUND",
                            is_cn ? "NO UPDATE FOUND" : nullptr);
            const char *msg = is_cn ? "\u672a\u627e\u5230\u66f4\u65b0\u5305" : "Update package not found";
            ui::TextWrappedCJK(msg);
            ImGui::Spacing();
            if (ui::DialogButtons(nullptr, is_cn ? "\u786e\u5b9a" : "OK") == 2)
            {
                ImGui::CloseCurrentPopup();
            }
            ui::EndDialog();
        }

        if (update_failed_popup_pending_)
        {
            ImGui::OpenPopup("update_failed");
            update_failed_popup_pending_ = false;
        }
        if (ui::BeginDialog("update_failed", ui::DialogSize::Small))
        {
            ui::DialogTitle(is_cn ? "\u66f4\u65b0\u5931\u8d25" : "UPDATE FAILED",
                            is_cn ? "UPDATE FAILED" : nullptr);
            const char *msg = is_cn ? "\u66f4\u65b0\u5931\u8d25" : "Update failed";
            ui::TextWrappedCJK(msg);
            ImGui::Spacing();
            if (ui::DialogButtons(nullptr, is_cn ? "\u786e\u5b9a" : "OK") == 2)
            {
                ImGui::CloseCurrentPopup();
            }
            ui::EndDialog();
        }

        if (update_reboot_popup_pending_)
        {
            ImGui::OpenPopup("update_reboot");
            update_reboot_popup_pending_ = false;
        }
        if (update_status_)
        {
            const int status = update_status_();
            if (status == 1 && !ImGui::IsPopupOpen("update_in_progress"))
            {
                ImGui::OpenPopup("update_in_progress");
            }
        }
        if (ui::BeginDialog("update_reboot", ui::DialogSize::Small))
        {
            ui::DialogTitle(is_cn ? "\u9700\u8981\u91cd\u542f" : "REBOOT REQUIRED",
                            is_cn ? "REBOOT REQUIRED" : nullptr);
            const char *msg = is_cn ? "\u66f4\u65b0\u5b8c\u6210\uff0c\u662f\u5426\u91cd\u542f\uff1f"
                                    : "Update completed. Reboot now?";
            ui::TextWrappedCJK(msg);
            ImGui::Spacing();
            const int choice = ui::DialogButtons(is_cn ? "\u7a0d\u540e" : "Later",
                                                 is_cn ? "\u91cd\u542f" : "Reboot");
            if (choice == 1)
            {
                ImGui::CloseCurrentPopup();
            }
            else if (choice == 2)
            {
                if (request_reboot_)
                {
                    request_reboot_();
                }
                ImGui::CloseCurrentPopup();
            }
            ui::EndDialog();
        }
        if (ui::BeginDialog("update_in_progress", ui::DialogSize::Small))
        {
            ui::DialogTitle(is_cn ? "\u6b63\u5728\u66f4\u65b0" : "UPDATING",
                            is_cn ? "UPDATING" : nullptr);
            // 进度提示：没有可点按钮，由 update_status_ 轮询决定何时关闭（保持原语义）。
            if (update_status_ && update_status_() != 1)
            {
                ImGui::CloseCurrentPopup();
                // Popup will be closed next frame once update completes.
            }
            else
            {
                const char *msg = is_cn ? "\u6b63\u5728\u66f4\u65b0\uff0c\u8bf7\u7a0d\u5019..." : "Updating, please wait...";
                ui::TextWrappedCJK(msg);
            }
            ui::EndDialog();
        }
    }

    ImGui::End();
    ui::PopMenuStyle();
}
