#pragma once
// oa::render — 文本状态（纯数据）。
//
// 这个头只放**结构**，不放算法：MessageLayer 是"一个节点承载的文本"的
// 全部状态，Layer 直接持有它（见 layer.h 的 `text` 成员）。因此：
//
//   * 节点删 → 文本没了（不存在"藏起来的旧页"）；
//   * 文本的变换/可见性/clip 就是节点的变换/可见性/clip（不存在两套）。
//
// 与"文本配置"的区别：font / 绘制盒 / scetween / glyph 是**会话级**的
// ——框架只在启动时设一次（msg_backlog_init 给 500.100.x 设 font，之后
// push_backlog_line 只 rp + 重打）。它们也跟着节点走，但 TextEngine 在
// 节点重建时会用 `TextProfile`（独立的小表）把它们还回来。
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oa::render {

inline constexpr std::string_view kDefaultMessageLayer = "adv01";
inline constexpr double kDefaultFontSize = 40.0;
/// 缺省字体文件候选（runtime load_default_font）。
inline constexpr std::string_view kDefaultFontFile = "font/sourcehansans-medium.otf";
inline constexpr size_t kBacklogMaxPages = 100;

// ---------------------------------------------------------------------------
// Font settings: 保形参数表 + 少量 typed 取用（FontDesc）
// ---------------------------------------------------------------------------
struct FontDesc {
    std::map<std::string, std::string> raw;
    void merge_raw(const std::map<std::string, std::string>& p);
    const std::string* get(std::string_view key) const;
    std::string get_or(std::string_view key, const std::string& fallback) const;
    double size() const;
    std::string face() const;
    std::string color() const;
    double spacetop() const;
    double spacemiddle() const;
    double spacebottom() const;
    double rubysize() const;
};

// ---------------------------------------------------------------------------
// Page units：一页文本的逻辑单元流（排版输入）
// ---------------------------------------------------------------------------
struct PageUnit {
    enum class Kind { Text, Newline, RubyBase };
    Kind kind = Kind::Text;
    std::string data;
    std::string ruby_annotation;
    FontDesc font;
};

// ---------------------------------------------------------------------------
// scetween / backlog 再现 / glyph 图标
// ---------------------------------------------------------------------------
enum class ScetweenMode {
    In, Out, Show, Hide, BacklogDownIn, BacklogDownOut, BacklogUpIn, BacklogUpOut,
};

struct ScetweenConfig {
    ScetweenMode mode = ScetweenMode::In;
    bool set_add = false; // [scetween mode=add] vs init
    std::string param;    // "alpha"/"left"/...
    std::string ease;
    double diff = 0;
    uint64_t delay_per_char = 0; // [scetween delay=]
    uint64_t time_per_char = 0;  // [scetween time=]
    bool random_delay = false;
    bool is_entrance() const {
        return mode == ScetweenMode::In || mode == ScetweenMode::Show ||
               mode == ScetweenMode::BacklogDownIn || mode == ScetweenMode::BacklogUpIn;
    }
    /// 一个字的间隔（毫秒）。0 = 这一条不参与逐字（瞬时）。
    uint64_t per_char_ms() const { return delay_per_char; }
};

/// backlog 页内再现标签（BacklogTag）。
struct BacklogTag {
    enum class Kind { Text, LineBreak, Font, RubyStart, RubyEnd };
    Kind kind = Kind::Text;
    std::string text;
    std::map<std::string, std::string> font;
    std::string get_string() const;
};

struct BacklogPage {
    std::map<std::string, std::string> page_font;
    std::vector<BacklogTag> tags;
    bool has_text() const;
    std::vector<std::string> reproduction_tags(bool allfont) const;
};

struct GlyphIconConfig {
    std::string layer;    // 行末图标层（缺省禁用）
    std::string rplayer;  // 页末图标层（缺省=layer）
    double left = 0, top = 0, rpleft = 0, rptop = 0;
    bool homing = false;
};

struct ClickWaitPlacement {
    std::string layer_id;
    double left = 0;
    double top = 0;
};

// ---------------------------------------------------------------------------
// Layout（纯排版）
// ---------------------------------------------------------------------------
struct CharMetrics {
    double advance = 0;
    double width = 0;
    double height = 0;
};
using MetricsFn = std::function<CharMetrics(void* userdata, const FontDesc&, uint32_t cp)>;

struct LayoutConfig {
    std::string prohibit_head;
    std::string prohibit_foot;
    std::string wordparts;
    std::string indent_pair;
    int indent_range = 0;
    bool indent_nest = false;
};

struct LaidGlyph {
    uint32_t cp = 0;
    double advance = 0;
    double width = 0;
    double x = 0;
    uint32_t line = 0;
    /// 字体来自**哪一页单元**（layer.page 的下标）。绝不存 FontDesc*：
    /// 排版缓存跨帧存活，页重建后指针就指向已释放的旧页（读档崩溃的根因）。
    size_t unit = 0;
    bool newline = false;
    bool has_ruby = false;
    size_t order = 0; // 页内字形序号（reveal 的 shown 过滤用）
};
struct LaidRuby {
    uint32_t line = 0;
    double x = 0;
    double width = 0;
    std::string text;
    double size = 0;
};
struct LaidPage {
    std::vector<LaidGlyph> glyphs;
    std::vector<LaidRuby> rubies;
    double line_height = 0;
};

/// 一页文本 = 一个**内容实例**。它属于节点（Layer::text），不属于引擎。
///
/// 逐字显示用**绝对时钟**：`shown` 由 (now - start_ms) / per_char 直接算出，
/// 不做 delta 累加。掉帧不会让速度永久落后，改速度只要反解 start_ms 就能从
/// 当前位置无缝继续 —— 不需要"首帧不推进"之类的特例。
struct MessageLayer {
    std::string id;
    bool layered = false;
    /// 该层是否被 [font] 排版键（left/top/width/height 任一）显式配置过绘制
    /// 盒。字体几何是 Artemis 文本的唯一落点来源（每张字表自带 rect）;
    /// 从未排版过的层 = 无绘制盒,只能画在脚本显式创建的节点上
    /// （is_message_layer_drawable 门槛,孤儿打印不绘制）。
    bool positioned = false;
    double left = 0, top = 0, width = 0, height = 0;
    FontDesc font;
    std::vector<FontDesc> font_stack;
    bool text_hidden = false;
    std::vector<PageUnit> page;
    uint64_t generation = 0;

    // ---- 逐字（reveal）--------------------------------------------------
    // shown     = 已显示的字形数（页内序号，含换行）
    // start_ms  = 本页开始显示的时刻（绝对时钟）；改速度时反解它
    // done      = 这一页显示完了（冗余于 shown>=char_count，但读档要存）
    size_t shown = 0;
    uint64_t start_ms = 0;
    bool clock_started = false;
    bool done = false;
    std::vector<ScetweenConfig> scetween;

    std::vector<BacklogTag> page_tags;
    std::map<std::string, std::string> page_font;
    size_t char_count = 0; // 页单元展平字形数（reveal 上限）

    /// [rp] 清掉的那一页：内容指纹 + 它当时是否已经显示完。
    ///
    /// UI 关闭后游戏会把**当前页原样重打一遍**（`[rp]` 后重打原文，见
    /// `advTextRefresh`）。那不是新的一页——重新逐字显示会让文字先消失再
    /// 慢慢打出来。指纹相同且上一页已完成 ⇒ 直接算显示完。
    std::string last_page_key;
    bool last_page_complete = false;

    /// 读档恢复出来的页面：框架的**第一次重绘**要整页替换，不是追加。
    ///
    /// 存档里带了这一页的文字（为了让"不重绘"的读档路径也有字），而
    /// `restoreText() → mw_redraw() → message_control()` 会把**同一页原样
    /// 再打一遍** —— `push_text` 是追加语义，于是同一句在页上出现两次
    /// （test2：「读档以后剧情文字重复显示一句」）。置位后第一次 push 先
    /// 清页再写，重绘 = 替换；没人重绘时恢复出来的那页照常显示。
    bool reload_replace_pending = false;
};

/// 会话级的文本配置（与节点无关，按 id 存活）。
///
/// 框架只在启动时对一批 id 做 [chgmsg] + [font]（backlog 的 500.100.x 就是
/// 这样），之后这些节点可能被 [lydel] 删掉再建 —— 配置必须还在。
struct TextProfile {
    FontDesc font;
    std::vector<FontDesc> font_stack;
    bool positioned = false;
    double left = 0, top = 0, width = 0, height = 0;
    std::vector<ScetweenConfig> scetween;
    bool valid = false;
};

} // namespace oa::render
