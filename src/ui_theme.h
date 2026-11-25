#pragma once

// ============================================================================
//  NERV 终端风格设计令牌与绘制助手（header-only）
//
//  设计约束：
//   * 面板底为纯黑（100% 不透明），唯一强调色是琥珀三档 A0/A1/A2。
//   * 唯一例外是 kDanger，只用于「录像中」与「启动到安卓」这两处危险态。
//   * 所有圆角为 0，所有描边为 1px —— 终端质感靠直线与分段条，不靠圆角。
//   * 本文件不引入任何新依赖、新资源；图标全部由 ImDrawList 基础图元手绘。
// ============================================================================

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

namespace ui
{

// ---- 色板：纯黑 + 琥珀三档（唯一例外是 kDanger）----
inline constexpr ImU32 kBlack      = IM_COL32(0, 0, 0, 255);         // 面板底，100% 不透明
inline constexpr ImU32 kAmber      = IM_COL32(255, 138, 0, 255);     // A0 结构色：线框/角标/分组标签/焦点竖条/选中反白
inline constexpr ImU32 kAmberData  = IM_COL32(255, 194, 77, 255);    // A1 数据色：所有数值
inline constexpr ImU32 kAmberDim   = IM_COL32(143, 90, 16, 255);     // A2 弱化：英文小字/单位/说明/未选中图标
inline constexpr ImU32 kAmberLabel = IM_COL32(255, 201, 143, 255);   // 中文标签
inline constexpr ImU32 kAmberLine  = IM_COL32(255, 138, 0, 77);      // 30% 分隔线/卡片描边
inline constexpr ImU32 kAmberSoft  = IM_COL32(255, 138, 0, 31);      // 12% 焦点行底
inline constexpr ImU32 kAmberFaint = IM_COL32(255, 138, 0, 51);      // 20% 空进度段
inline constexpr ImU32 kDanger     = IM_COL32(255, 59, 0, 255);      // 只给危险态：录像中 / 启动到安卓
// 控件底一律透明：NERV 面板靠 1px 琥珀描边界定控件，而不是靠深色填充块。
// 悬停/按下只给极轻的琥珀薄雾，避免整块下拉框变成深棕实心块。
inline constexpr ImU32 kBtnFill    = IM_COL32(0, 0, 0, 0);
inline constexpr ImU32 kBtnHover   = IM_COL32(255, 138, 0, 26);
inline constexpr ImU32 kBtnActive  = IM_COL32(255, 138, 0, 46);
inline constexpr ImU32 kInk        = IM_COL32(0, 0, 0, 255);        // 反白字（琥珀实底上的黑字）

// ---- 统一缩放后的度量 ----
// 以 1920x1080 为基准，k = min(vp_w/1920, vp_h/1080)，并 clamp 到 [0.62, 1.0]。
// 这样 1280x720 下 k = 0.667，所有尺寸等比缩小且不会被压得过小。
struct Metrics
{
    float k = 1.0f;          // 统一缩放
    float rail_w = 72.0f;    // 72 * k
    float panel_x = 72.0f;   // rail_w
    float panel_w = 560.0f;  // 560 * k
    float panel_h = 1080.0f; // vp->Size.y（贯穿全高）
    float pad = 28.0f;       // 28 * k  面板左右内边距
    float row_h = 52.0f;     // 52 * k
    float ctrl_h = 40.0f;    // 40 * k
    float label_sz = 20.0f;  // ImGui::GetFontSize() * 0.77
    float en_sz = 15.0f;     // ImGui::GetFontSize() * 0.58
    float title_sz = 21.8f;  // ImGui::GetFontSize() * 0.84
    float data_sz = 20.0f;   // ImGui::GetFontSize() * 0.77
    float btn_sz = 16.6f;    // ImGui::GetFontSize() * 0.64
};

inline Metrics ComputeMetrics(const ImGuiViewport *vp)
{
    Metrics m{};
    const float vp_w = (vp != nullptr && vp->Size.x > 0.0f) ? vp->Size.x : 1920.0f;
    const float vp_h = (vp != nullptr && vp->Size.y > 0.0f) ? vp->Size.y : 1080.0f;

    float k = std::min(vp_w / 1920.0f, vp_h / 1080.0f);
    k = std::max(0.62f, std::min(1.0f, k));

    m.k = k;
    // 面板宽度不用 k 等比缩小：小屏幕上用户离得更远、字号占比更高，
    // 等比缩会让面板过窄而文字偏大。改用 0.5*k+0.5 的柔和插值，
    // 使 720p 下宽度为 560*(0.5*0.667+0.5)=467px（屏宽的 36%），
    // 1080p 下仍是 560px（屏宽的 29%）。
    const float w_lerp = 0.5f * k + 0.5f;
    m.rail_w = std::max(52.0f, 72.0f * k);
    m.panel_x = m.rail_w;
    m.panel_w = 560.0f * w_lerp;
    m.panel_h = vp_h;
    m.pad = std::max(20.0f, 28.0f * k);
    // 行高以字号为准：至少容纳 label_sz 的 2.1 倍，再取 k 缩放的 52px 中较大者。
    const float font_px = ImGui::GetFontSize();
    m.row_h = std::max(font_px * 1.95f, 52.0f * k);
    m.ctrl_h = std::max(font_px * 1.35f, 40.0f * k);

    const float font = ImGui::GetFontSize();
    m.label_sz = font * 0.77f;
    m.en_sz = font * 0.58f;
    m.title_sz = font * 0.84f;
    m.data_sz = font * 0.77f;
    m.btn_sz = font * 0.64f;
    return m;
}

// ---- 六边形：rx/ry 为半宽/半高 ----
// 顶点顺序 (25%,0)(75%,0)(100%,50%)(75%,100%)(25%,100%)(0,50%)
inline void Hexagon(ImDrawList *dl, ImVec2 center, float rx, float ry, ImU32 col, float thickness)
{
    if (dl == nullptr)
        return;
    ImVec2 pts[6] = {
        ImVec2(center.x - 0.50f * rx, center.y - 1.00f * ry),
        ImVec2(center.x + 0.50f * rx, center.y - 1.00f * ry),
        ImVec2(center.x + 1.00f * rx, center.y + 0.00f * ry),
        ImVec2(center.x + 0.50f * rx, center.y + 1.00f * ry),
        ImVec2(center.x - 0.50f * rx, center.y + 1.00f * ry),
        ImVec2(center.x - 1.00f * rx, center.y + 0.00f * ry),
    };
    dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, thickness);
}

inline void HexagonFilled(ImDrawList *dl, ImVec2 center, float rx, float ry, ImU32 col)
{
    if (dl == nullptr)
        return;
    ImVec2 pts[6] = {
        ImVec2(center.x - 0.50f * rx, center.y - 1.00f * ry),
        ImVec2(center.x + 0.50f * rx, center.y - 1.00f * ry),
        ImVec2(center.x + 1.00f * rx, center.y + 0.00f * ry),
        ImVec2(center.x + 0.50f * rx, center.y + 1.00f * ry),
        ImVec2(center.x - 0.50f * rx, center.y + 1.00f * ry),
        ImVec2(center.x - 1.00f * rx, center.y + 0.00f * ry),
    };
    dl->AddConvexPolyFilled(pts, 6, col);
}

// ---- 四角角标：在矩形 [a,b] 的四角各画一个 len 长的 L 形折线 ----
inline void CornerBrackets(ImDrawList *dl, ImVec2 a, ImVec2 b, float len, ImU32 col, float thickness)
{
    if (dl == nullptr)
        return;
    // 左上
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + len, a.y), col, thickness);
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, a.y + len), col, thickness);
    // 右上
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x - len, a.y), col, thickness);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, a.y + len), col, thickness);
    // 左下
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x + len, b.y), col, thickness);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x, b.y - len), col, thickness);
    // 右下
    dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x - len, b.y), col, thickness);
    dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x, b.y - len), col, thickness);
}

// ---- 分段进度条：segments 个等宽小段，前 filled 个填 on_col，其余填 kAmberFaint ----
inline void SegmentBar(ImDrawList *dl, ImVec2 a, float width, float height,
                       int segments, int filled, ImU32 on_col)
{
    if (dl == nullptr || segments <= 0 || width <= 0.0f || height <= 0.0f)
        return;
    const float seg_w = width / static_cast<float>(segments);
    const float gap = seg_w * 0.35f;
    const float bar_w = std::max(1.0f, seg_w - gap);
    for (int i = 0; i < segments; ++i)
    {
        const float x0 = a.x + seg_w * static_cast<float>(i);
        const ImU32 col = (i < filled) ? on_col : kAmberFaint;
        dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x0 + bar_w, a.y + height), col);
    }
}

// ---- 全局面板样式压栈/出栈（8 个 StyleVar + 9 个 StyleColor）----
inline void PushMenuStyle()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // FramePadding.y drives both the combo height and the size of its arrow.
    // Default (4,3) makes the arrow huge relative to a 31px CJK font; (8,5)
    // gives a 40px control with a ~19px arrow, matching the design sheet.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBlack);
    ImGui::PushStyleColor(ImGuiCol_Border, kAmberLine);
    ImGui::PushStyleColor(ImGuiCol_Text, kAmberData);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBtnFill);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kBtnHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kBtnActive);
    ImGui::PushStyleColor(ImGuiCol_Button, kBtnFill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBtnHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kBtnActive);
}

inline void PopMenuStyle()
{
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(9);
}

// ---- 全局控件配色（在 application.cpp 里 StyleColorsDark() 之后调用一次）----
inline void ApplyGlobalStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.FramePadding = ImVec2(8.0f, 5.0f);
    // 细滚动条：下拉弹层里默认 14px 的浅色滚动条在纯黑面板上非常突兀。
    style.ScrollbarSize = 10.0f;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImColor(kBlack);
    colors[ImGuiCol_PopupBg] = ImColor(kBlack);
    colors[ImGuiCol_Border] = ImColor(kAmberLine);
    colors[ImGuiCol_Text] = ImColor(kAmberData);
    colors[ImGuiCol_TextDisabled] = ImColor(kAmberDim);
    colors[ImGuiCol_FrameBg] = ImColor(kBtnFill);
    colors[ImGuiCol_FrameBgHovered] = ImColor(kBtnHover);
    colors[ImGuiCol_FrameBgActive] = ImColor(kBtnActive);
    colors[ImGuiCol_Button] = ImColor(kBtnFill);
    colors[ImGuiCol_ButtonHovered] = ImColor(kBtnHover);
    colors[ImGuiCol_ButtonActive] = ImColor(kBtnActive);
    colors[ImGuiCol_Header] = ImColor(kAmberSoft);
    colors[ImGuiCol_HeaderHovered] = ImColor(kBtnHover);
    colors[ImGuiCol_HeaderActive] = ImColor(kBtnActive);
    colors[ImGuiCol_Separator] = ImColor(kAmberLine);
    colors[ImGuiCol_CheckMark] = ImColor(kAmber);
    colors[ImGuiCol_SliderGrab] = ImColor(kAmber);
    colors[ImGuiCol_NavCursor] = ImColor(IM_COL32(0, 0, 0, 0));
    // 滚动条：槽透明、滑块琥珀，与整体单色系一致
    colors[ImGuiCol_ScrollbarBg] = ImColor(IM_COL32(0, 0, 0, 0));
    colors[ImGuiCol_ScrollbarGrab] = ImColor(IM_COL32(255, 138, 0, 90));
    colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(IM_COL32(255, 138, 0, 150));
    colors[ImGuiCol_ScrollbarGrabActive] = ImColor(kAmber);
    // 下拉弹层与弹窗内的次级背景
    colors[ImGuiCol_ChildBg] = ImColor(IM_COL32(0, 0, 0, 0));
    colors[ImGuiCol_TableHeaderBg] = ImColor(IM_COL32(0, 0, 0, 0));
    colors[ImGuiCol_ModalWindowDimBg] = ImColor(IM_COL32(0, 0, 0, 110));
}


// ---- NERV 拨动开关 ----
// 造型：36x18 矩形轨道 + 12x12 实心方块滑块（无圆角，与整机直角语言一致）。
// 开：轨道填 kAmber，滑块为黑（反白）；关：轨道仅描边，滑块为 kAmberDim。
// 返回 true 表示值被改变。调用方负责把返回值写回 state。
inline bool Toggle(const char *id, bool *value, float k = 1.0f)
{
    const float w = 36.0f * k;
    const float h = 18.0f * k;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();

    // EnableNav 必须显式传入：InvisibleButton 默认带 ImGuiItemFlags_NoNav，
    // 否则开关永远拿不到焦点，Enter 会落到下一个可聚焦 item（实测会误触
    // 底部「完成」按钮把菜单关掉）。
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h),
                                                ImGuiButtonFlags_EnableNav);
    if (pressed)
        *value = !*value;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + w, p0.y + h);
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();

    const ImU32 frame = (*value || hovered || focused) ? kAmber : IM_COL32(255, 138, 0, 150);
    if (*value)
        dl->AddRectFilled(p0, p1, kAmber);
    dl->AddRect(p0, p1, frame, 0.0f, 0, 1.0f);

    // 滑块颜色：开态用反白黑（轨道已填满琥珀），关态用亮琥珀。
    // 关态若沿用 kAmberDim（深棕）会在黑底上几乎不可见，实测真机上像个残缺的空方块。
    const float knob = 12.0f * k;
    const float inset = 2.0f * k;
    const float kx = *value ? (p1.x - inset - knob) : (p0.x + inset);
    const float ky = p0.y + (h - knob) * 0.5f;
    dl->AddRectFilled(ImVec2(kx, ky), ImVec2(kx + knob, ky + knob),
                      *value ? kInk : kAmber);

    if (focused)
        dl->AddRect(ImVec2(p0.x - 2.0f, p0.y - 2.0f), ImVec2(p1.x + 2.0f, p1.y + 2.0f),
                    kAmber, 0.0f, 0, 1.0f);

    return pressed;
}

// ============================================================================
//  弹窗基座（P2：统一 11 个 modal 的宽度 / 标题 / 按钮排布）
//
//  为什么需要 EndDialog()：
//  弹窗可能在 BeginPopupModal 里提前返回 false（未打开 / 被裁剪），
//  而 PushStyleVar/Color 是全局栈、必须严格配对。因此把「压栈」放在
//  BeginDialog 里、「弹栈」拆成两条互斥出口：
//      * BeginDialog 返回 false → 它自己已经弹栈，调用方直接跳过；
//      * BeginDialog 返回 true  → 调用方【必须】在 EndPopup() 之后调用 EndDialog()。
//  这样无论弹窗开没开，压栈/弹栈都恰好一次，不会残留样式污染后续帧。
// ============================================================================

// 弹窗三档宽度（1080p 基准，内部按 ComputeMetrics 的 k 缩放）
enum class DialogSize
{
    Small,  // 420
    Medium, // 520
    Large   // 640
};

// 压入弹窗专用样式（4 个 StyleVar + 4 个 StyleColor）。
inline void PushDialogStyle(float k)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * k, 18.0f * k));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * k, 10.0f * k));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBlack);
    ImGui::PushStyleColor(ImGuiCol_Border, kAmberLine);
    ImGui::PushStyleColor(ImGuiCol_Text, kAmberData);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kBlack);
}

inline void PopDialogStyle()
{
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(4);
}

// 菜单风格弹窗容器。返回 true 表示弹窗已打开、可以画内容；
// 此时调用方【必须】自己调用 ImGui::EndPopup()，然后调用 EndDialog()。
inline bool BeginDialog(const char *id, DialogSize size)
{
    const Metrics m = ComputeMetrics(ImGui::GetMainViewport());

    float w = 420.0f;
    if (size == DialogSize::Medium)
        w = 520.0f;
    else if (size == DialogSize::Large)
        w = 640.0f;
    w *= m.k;

    PushDialogStyle(m.k);

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // 固定宽度 + 自动高度：x 给真实像素宽，y=0 让 ImGui 按内容自适应。
    // 刻意不用 AlwaysAutoResize —— 它会让宽度被内容撑开，首次与后续显示宽度不一致。
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f), ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal(id, nullptr, flags))
    {
        // 弹窗未打开：立刻弹栈，保证配对。
        PopDialogStyle();
        return false;
    }
    return true;
}

// 与 BeginDialog() 返回 true 配对：结束弹窗并弹出样式。
inline void EndDialog()
{
    ImGui::EndPopup();
    PopDialogStyle();
}

// 弹窗标题块：左侧 5x5 实心琥珀方块 + 标题（kAmber / title_sz），
// 可选副标题（kAmberDim / en_sz），下方一条 1px kAmberLine 贯通内容宽。
inline void DialogTitle(const char *title, const char *subtitle = nullptr)
{
    const Metrics m = ComputeMetrics(ImGui::GetMainViewport());
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    const float sq = 5.0f * m.k;
    const float gap = 10.0f * m.k;
    const float text_x = p.x + sq + gap;

    // 方块与标题文字垂直居中对齐
    const float cy = p.y + m.title_sz * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, cy - sq * 0.5f), ImVec2(p.x + sq, cy + sq * 0.5f), kAmber);
    dl->AddText(ImGui::GetFont(), m.title_sz, ImVec2(text_x, p.y), kAmber, title);

    float y = p.y + m.title_sz;
    if (subtitle != nullptr && subtitle[0] != '\0')
    {
        y += 2.0f * m.k;
        dl->AddText(ImGui::GetFont(), m.en_sz, ImVec2(text_x, y), kAmberDim, subtitle);
        y += m.en_sz;
    }

    // 分隔线贯通内容宽
    y += 8.0f * m.k;
    const float content_w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + content_w, y), kAmberLine, 1.0f);

    // 把光标推到分隔线下方
    ImGui::Dummy(ImVec2(content_w, (y - p.y) + 10.0f * m.k));
}

// 右对齐按钮行。cancel_label 传 nullptr 表示只有一个主按钮（此时它靠右）。
// 返回 1 = 取消被按，2 = 主按钮被按，0 = 都没按。
inline int DialogButtons(const char *cancel_label, const char *ok_label,
                         bool ok_enabled = true, bool focus_primary = false)
{
    const Metrics m = ComputeMetrics(ImGui::GetMainViewport());

    const float ok_w = 132.0f * m.k;
    const float cancel_w = 112.0f * m.k;
    const float h = 40.0f * m.k;
    const float gap = 12.0f * m.k;

    const float avail = ImGui::GetContentRegionAvail().x;
    const float total = (cancel_label != nullptr ? cancel_w + gap : 0.0f) + ok_w;
    if (avail > total)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - total));
    }

    int result = 0;

    if (cancel_label != nullptr)
    {
        // 幽灵按钮：透明底 + 1px 描边（描边色取 kAmberLine，比正文弱一档）
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAmberSoft);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kBtnActive);
        ImGui::PushStyleColor(ImGuiCol_Text, kAmberData);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        const bool pressed = ImGui::Button(cancel_label, ImVec2(cancel_w, h));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        if (pressed)
            result = 1;

        // 主按钮不可用时把默认焦点给取消，避免 Enter 落空
        if (!ok_enabled)
            ImGui::SetItemDefaultFocus();

        ImGui::SameLine(0.0f, gap);
    }

    // 主按钮：实心琥珀 + 黑字
    ImGui::PushStyleColor(ImGuiCol_Button, kAmber);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAmberData);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAmberLabel);
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    if (!ok_enabled)
        ImGui::BeginDisabled();
    // 请求焦点必须在 Button 之前：SetKeyboardFocusHere() 只对下一个
    // ItemAdd 生效，放在 Button 之后会指向后续的元素。
    if (focus_primary && ok_enabled)
        ImGui::SetKeyboardFocusHere();
    const bool ok_pressed = ImGui::Button(ok_label, ImVec2(ok_w, h));
    if (ok_enabled)
        ImGui::SetItemDefaultFocus(); // Enter 直接触发主按钮
    if (!ok_enabled)
        ImGui::EndDisabled();
    ImGui::PopStyleColor(4);

    if (ok_pressed)
        result = 2;

    return result;
}


// ---- CJK 友好自动折行 ----
// ImGui 自带的 TextWrapped 按任意码点断行，中文的「，。？！」会被甩到行首，
// 这是中文排版明令禁止的。这里自己做一次按宽度贪心 + 禁则修正的折行：
//   * 行首禁止字符：，。、；：？！）】｝」』》〉”’…—～·%℃ 以及半角对应符号
//   * 行尾禁止字符：（【｛「『《〈“ 以及半角开括号
// 遇到违规断点就把断点前移一个字符（押し下げ），宁可让上一行略窄也不让标点顶头。
inline void TextWrappedCJK(const char *text)
{
    if (text == nullptr || *text == '\0')
    {
        return;
    }

    const float max_w = ImGui::GetContentRegionAvail().x;
    if (max_w <= 1.0f)
    {
        ImGui::TextUnformatted(text);
        return;
    }

    ImFont *font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();

    // 按 UTF-8 码点切分（中文无组合字符，逐个码点即可）。
    std::vector<std::string> cell;
    std::vector<float> adv;
    for (const char *p = text; *p != '\0';)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        int len = 1;
        if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        std::string g(p, static_cast<size_t>(len));
        cell.push_back(g);
        adv.push_back(font->CalcTextSizeA(fs, FLT_MAX, 0.0f, g.c_str()).x);
        p += len;
    }

    const auto is_no_start = [](const std::string &g)
    {
        static const char *kSet[] = {"，", "。", "、", "；", "：", "？", "！", "）", "】", "｝",
                                     "」", "』", "》", "〉", "”", "’", "…", "—", "～", "·",
                                     "℃", "%", ",", ".", ";", ":", "?", "!", ")", "]"};
        for (const char *s : kSet)
            if (g == s)
                return true;
        return false;
    };
    const auto is_no_end = [](const std::string &g)
    {
        static const char *kSet[] = {"（", "【", "｛", "「", "『", "《", "〈", "“", "(", "[", "{"};
        for (const char *s : kSet)
            if (g == s)
                return true;
        return false;
    };

    std::string out;
    const int n = static_cast<int>(cell.size());
    int i = 0;
    while (i < n)
    {
        if (cell[i] == "\n")
        {
            out += '\n';
            ++i;
            continue;
        }

        const int line_start = i;
        float w = 0.0f;
        while (i < n && cell[i] != "\n")
        {
            if (w + adv[i] > max_w && i > line_start)
            {
                break;
            }
            w += adv[i];
            ++i;
        }

        if (i < n && cell[i] != "\n")
        {
            // 禁则修正：断点前移，使标点不落在行首、开括号不留在行尾。
            while (i - 1 > line_start && is_no_start(cell[i]))
            {
                --i;
            }
            while (i - 1 > line_start && is_no_end(cell[i - 1]))
            {
                --i;
            }
        }

        for (int k = line_start; k < i; ++k)
        {
            out += cell[k];
        }
        if (i < n && cell[i] != "\n")
        {
            out += '\n';
        }
    }

    ImGui::TextUnformatted(out.c_str());
}


// ============================================================================
//  OSD 矢量图标
//  原来用的是 /storage/digitalfpv/icons/*.png（彩色蓝色系），和琥珀单色系冲突。
//  这里全部改成 ImDrawList 手绘，颜色由调用方给，天然跟随主题。
//  尺寸 s 是图标的外接方边长，绘制以 c 为中心。
// ============================================================================
enum class OsdIcon
{
    Antenna,
    Gps,
    Home,
    Speed,
    BatteryCell,
    BatteryPack,
    Monitor,
    TempAir,
    TempGround,
    Wifi
};

inline void OsdGlyph(ImDrawList *dl, OsdIcon icon, ImVec2 c, float s, ImU32 col)
{
    const float h = s * 0.5f;
    switch (icon)
    {
    case OsdIcon::Antenna:
    {
        // 竖杆 + 底座 + 左右各两道弧（信号辐射）
        dl->AddLine(ImVec2(c.x, c.y - h * 0.85f), ImVec2(c.x, c.y + h * 0.45f), col, 1.5f);
        dl->AddLine(ImVec2(c.x - h * 0.5f, c.y + h * 0.78f),
                    ImVec2(c.x + h * 0.5f, c.y + h * 0.78f), col, 1.5f);
        dl->PathArcTo(ImVec2(c.x, c.y - h * 0.2f), h * 0.55f, -0.85f, 0.85f, 10);
        dl->PathStroke(col, 0, 1.3f);
        dl->PathArcTo(ImVec2(c.x, c.y - h * 0.2f), h * 0.85f, -0.8f, 0.8f, 12);
        dl->PathStroke(col, 0, 1.1f);
        dl->PathArcTo(ImVec2(c.x, c.y - h * 0.2f), h * 0.55f, 3.1416f - 0.85f, 3.1416f + 0.85f, 10);
        dl->PathStroke(col, 0, 1.3f);
        dl->PathArcTo(ImVec2(c.x, c.y - h * 0.2f), h * 0.85f, 3.1416f - 0.8f, 3.1416f + 0.8f, 12);
        dl->PathStroke(col, 0, 1.1f);
        break;
    }
    case OsdIcon::Gps:
    {
        // 十字准星 + 中心圆
        dl->AddCircle(c, h * 0.62f, col, 14, 1.4f);
        dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x - h * 0.55f, c.y), col, 1.4f);
        dl->AddLine(ImVec2(c.x + h * 0.55f, c.y), ImVec2(c.x + h, c.y), col, 1.4f);
        dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y - h * 0.55f), col, 1.4f);
        dl->AddLine(ImVec2(c.x, c.y + h * 0.55f), ImVec2(c.x, c.y + h), col, 1.4f);
        dl->AddCircleFilled(c, h * 0.14f, col, 8);
        break;
    }
    case OsdIcon::Home:
    {
        // 屋顶（三角）+ 屋身（矩形）+ 门洞，和 GPS 准星明确区分
        dl->AddTriangle(ImVec2(c.x, c.y - h * 0.95f),
                        ImVec2(c.x - h * 0.85f, c.y - h * 0.1f),
                        ImVec2(c.x + h * 0.85f, c.y - h * 0.1f), col, 1.4f);
        dl->AddRect(ImVec2(c.x - h * 0.6f, c.y - h * 0.1f),
                    ImVec2(c.x + h * 0.6f, c.y + h * 0.9f), col, 0.0f, 0, 1.4f);
        dl->AddRectFilled(ImVec2(c.x - h * 0.18f, c.y + h * 0.34f),
                          ImVec2(c.x + h * 0.18f, c.y + h * 0.9f), col);
        break;
    }
    case OsdIcon::Speed:
    {
        // 三条速度线 + 右向箭头
        for (int i = 0; i < 3; ++i)
        {
            const float y = c.y - h * 0.5f + h * 0.5f * static_cast<float>(i);
            const float len = h * (1.0f - 0.22f * static_cast<float>(i));
            dl->AddLine(ImVec2(c.x - h, y), ImVec2(c.x - h + len, y), col, 1.4f);
        }
        dl->AddTriangleFilled(ImVec2(c.x + h, c.y),
                              ImVec2(c.x + h * 0.42f, c.y - h * 0.42f),
                              ImVec2(c.x + h * 0.42f, c.y + h * 0.42f), col);
        break;
    }
    case OsdIcon::BatteryCell:
    case OsdIcon::BatteryPack:
    {
        const bool pack = (icon == OsdIcon::BatteryPack);
        // 外壳 + 正极凸头
        dl->AddRect(ImVec2(c.x - h * 0.85f, c.y - h * 0.5f),
                    ImVec2(c.x + h * 0.62f, c.y + h * 0.5f), col, 0.0f, 0, 1.4f);
        dl->AddRectFilled(ImVec2(c.x + h * 0.62f, c.y - h * 0.22f),
                          ImVec2(c.x + h * 0.85f, c.y + h * 0.22f), col);
        // 内部电量条：单节 1 格、总电 2 格
        const int bars = pack ? 3 : 1;
        const float inner_w = h * 1.47f / static_cast<float>(bars);
        for (int i = 0; i < bars; ++i)
        {
            const float x0 = c.x - h * 0.85f + h * 0.12f + inner_w * static_cast<float>(i);
            dl->AddRectFilled(ImVec2(x0, c.y - h * 0.28f),
                              ImVec2(x0 + inner_w - h * 0.14f, c.y + h * 0.28f), col);
        }
        break;
    }
    case OsdIcon::Monitor:
    {
        dl->AddRect(ImVec2(c.x - h, c.y - h * 0.72f),
                    ImVec2(c.x + h, c.y + h * 0.36f), col, 0.0f, 0, 1.4f);
        dl->AddLine(ImVec2(c.x, c.y + h * 0.36f), ImVec2(c.x, c.y + h * 0.75f), col, 1.4f);
        dl->AddLine(ImVec2(c.x - h * 0.45f, c.y + h * 0.78f),
                    ImVec2(c.x + h * 0.45f, c.y + h * 0.78f), col, 1.4f);
        break;
    }
    case OsdIcon::TempAir:
    case OsdIcon::TempGround:
    {
        // 温度计：球泡 + 杆 + 刻度；地面端多一条基座横线
        const float bx = c.x - h * 0.45f;
        dl->AddCircle(ImVec2(bx, c.y + h * 0.52f), h * 0.34f, col, 12, 1.4f);
        dl->AddLine(ImVec2(bx - h * 0.16f, c.y + h * 0.42f),
                    ImVec2(bx - h * 0.16f, c.y - h * 0.72f), col, 1.3f);
        dl->AddLine(ImVec2(bx + h * 0.16f, c.y + h * 0.42f),
                    ImVec2(bx + h * 0.16f, c.y - h * 0.72f), col, 1.3f);
        dl->AddLine(ImVec2(bx - h * 0.16f, c.y - h * 0.72f),
                    ImVec2(bx + h * 0.16f, c.y - h * 0.72f), col, 1.3f);
        if (icon == OsdIcon::TempAir)
        {
            // 空气：右侧两道波纹
            dl->AddLine(ImVec2(c.x + h * 0.22f, c.y - h * 0.35f),
                        ImVec2(c.x + h * 0.95f, c.y - h * 0.35f), col, 1.2f);
            dl->AddLine(ImVec2(c.x + h * 0.35f, c.y - h * 0.02f),
                        ImVec2(c.x + h * 0.95f, c.y - h * 0.02f), col, 1.2f);
        }
        else
        {
            // 地面：底部基座
            dl->AddLine(ImVec2(c.x - h * 0.95f, c.y + h * 0.9f),
                        ImVec2(c.x + h * 0.95f, c.y + h * 0.9f), col, 1.4f);
        }
        break;
    }
    case OsdIcon::Wifi:
    {
        for (int i = 0; i < 3; ++i)
        {
            const float r = h * (0.35f + 0.3f * static_cast<float>(i));
            dl->PathArcTo(ImVec2(c.x, c.y + h * 0.55f), r, 3.1416f + 0.7f, 3.1416f + 2.4416f, 12);
            dl->PathStroke(col, 0, 1.3f);
        }
        dl->AddCircleFilled(ImVec2(c.x, c.y + h * 0.6f), h * 0.12f, col, 8);
        break;
    }
    }
}

} // namespace ui
