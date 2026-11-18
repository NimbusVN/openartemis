#pragma once
// oa::render — Artemis 消息层文本引擎。
//
// 存储归属：文本内容挂在**节点**上（Layer::text / text_state.h）。TextEngine
// 只做三件事：
//   1. 编辑当前页（[chgmsg]/[font]/[print]/[rp]/[rt]/[scetween]…）；
//   2. 按绝对时钟推进逐字显示（advance(now_ms)）；
//   3. 维护会话级的排版配置（TextProfile：框架只在启动时设一次 font）。
//
// 未接场景时（单元测试直接 `TextEngine e;`）用内部 map 顶替 —— 存储是
// 可替换的，算法只有一份。
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/render/text_state.h"

namespace oa::render {

/// 文本存储（场景实现；Compositor 提供）。
///
/// 生命周期由节点决定：节点被删，find_text 就返回 nullptr，ensure_text 会
/// 为新节点造一份。TextEngine 不持有"幽灵层"。
class TextStore {
public:
    virtual ~TextStore() = default;
    /// 该 id 节点上的文本；节点不存在或没有文本 → nullptr。
    virtual MessageLayer* find_text(const std::string& id) = 0;
    /// 该 id 节点上的文本，没有就建（节点不存在时由实现决定是否物化）。
    virtual MessageLayer& ensure_text(const std::string& id) = 0;
    /// 所有承载文本的 id（顺序不保证；仅用于遍历/快照）。
    virtual std::vector<std::string> text_ids() const = 0;
};

// ---------------------------------------------------------------------------
// TextEngine：消息层栈 + 各层字体/页面状态 + 事件归约
// ---------------------------------------------------------------------------
class TextEngine {
public:
    TextEngine();
    ~TextEngine();

    /// 接上场景存储。`nullptr` = 断开，回到内部 map。
    void attach(TextStore* store);
    bool attached() const { return store_ != nullptr; }

    void clear_scene();
    /// 把活动层锚回"当前持有可见页内容的层"：页面点击等待释放时调用
    void anchor_active_to_content();
    const FontDesc& default_font() const { return default_font_; }

    // [chgmsg]
    void switch_layer(std::optional<std::string> id, bool stack,
                      std::optional<int> layered);
    void pop_layer();
    /// 节点（及子树）被删：[lydel] 的文本侧收尾。**只丢内容**（页/游标/
    /// 隐藏标记/重打快照），保留会话配置（TextProfile），这样同一 id 的
    /// 下一次 [chgmsg] 还能拿到排版。
    void forget_node(const std::string& id);
    // font 族
    void apply_font(const std::map<std::string, std::string>& p);
    void font_init();
    void font_close();
    void font_default(const std::map<std::string, std::string>& p);
    // 文本：push_text 把内容按 '\n' 归一为 (Text 段 | Newline 单元) 序列
    // （内容内 LF == [rt] 断行语义；CRLF 的 '\r' 丢弃）。
    void push_text(const std::string& content);
    /// 读档恢复的页：这一层下一次 `push_text` 是整页替换而不是追加
    /// （见 MessageLayer::reload_replace_pending）。
    void arm_reload_replace(const std::string& id);
    void line_break(bool omit_blank_line);
    void page_break(std::optional<int> backlog_param);
    // ruby
    void ruby_start(const std::string& annotation);
    void ruby_end();
    // scein/sceout
    void set_text_hidden(bool hidden);
    // 排版配置
    void set_prohibit(const std::string& head, const std::string& foot);
    void set_wordparts(const std::string& parts);
    void set_indent(const std::string& pair);
    // [scetween]
    void apply_scetween(const std::map<std::string, std::string>& p);
    // [glyph]
    void set_glyph_config(const std::map<std::string, std::string>& raw);
    // [backlog]/[writebacklog]
    void apply_backlog_config(const std::map<std::string, std::string>& p, bool clear);
    void set_backlog_write_mode(bool mode);
    // reveal 驱动（绝对时钟）
    /// 把所有**未完成**的页推进到 now_ms。不再接受 delta：一帧卡多长都
    /// 不会让速度永久落后。
    void advance(uint64_t now_ms);
    void reveal_all();
    /// 是否所有承载内容的页都显示完了。只看**有字的页**：空层/隐藏层/测量
    /// 层不参与判定（以前遍历全表，一个残留 pending 就卡死翻页）。
    bool is_reveal_complete() const;
    /// 有页面正在"逐字"（存在非零间隔）——瞬显页不算，避免吃掉点击。
    bool is_reveal_paced() const;
    // click_wait 图标位置（行末/页末）
    std::optional<ClickWaitPlacement> click_wait_placement(bool page_end) const;

    const LayoutConfig& layout_config() const { return layout_cfg_; }

    // --- 查询 ---
    std::string active_layer_id() const;
    const MessageLayer* layer(std::string_view id) const;
    uint64_t revision() const { return revision_; }
    std::vector<std::string> visible_content_layers() const;
    bool active_has_content() const;
    const GlyphIconConfig& glyph_icon() const { return glyph_icon_; }
    // 页面是否含非空白字符（诊断/冒烟：basefont 空白测量层排除）
    bool page_has_visible_text(std::string_view id) const;
    // backlog 查询
    size_t backlog_size() const { return backlog_pages_.size(); }
    std::optional<std::vector<std::string>> backlog_tags(size_t page, bool allfont) const;
    std::optional<std::vector<std::string>> message_tags(std::string_view id,
                                                         bool allfont) const;

    // --- 内部/测试观察 ---
    size_t text_layers() const;
    /// 全部承载文本的 (id, 层)。快照/dump 用；顺序按 id 升序。
    std::vector<std::pair<std::string, const MessageLayer*>> all_layers() const;
    const std::vector<std::string>& layer_stack() const { return layer_stack_; }

private:
    MessageLayer& active_mut();
    void bump_revision();
    std::string anon_id();
    size_t recompute_char_count(MessageLayer& l) const;
    std::vector<BacklogTag> page_tags_of(const MessageLayer& l) const;
    // 存储访问（attach 与否的唯一分叉）
    MessageLayer* find_text(const std::string& id) const;
    MessageLayer& ensure_text(const std::string& id);
    std::vector<std::string> text_ids() const;
    /// 会话配置：apply_font 等写这里，节点重建时 ensure_text 用它恢复。
    TextProfile& profile(const std::string& id);
    void save_profile(const std::string& id, const MessageLayer& l);
    /// 把 profile 还原进一个刚建出来的层。
    void apply_profile(const std::string& id, MessageLayer* l);

    std::optional<std::string> active_;
    std::vector<std::string> layer_stack_;
    TextStore* store_ = nullptr;                 // 非空 = 内容在场景节点上
    mutable std::map<std::string, MessageLayer> own_; // 未接场景时的存储
    std::map<std::string, TextProfile> profiles_;     // id → 会话配置
    FontDesc default_font_;
    uint64_t revision_ = 0;
    bool ruby_open_ = false;
    std::string ruby_annotation_;
    uint64_t anonymous_serial_ = 0;
    /// 最近一次 advance 的时钟（[scetween] 改速度时用它反解 start_ms）。
    uint64_t last_now_ms_ = 0;
    LayoutConfig layout_cfg_;
    // session state
    std::vector<BacklogPage> backlog_pages_;
    bool backlog_allow_ = true;
    bool backlog_write_mode_ = false;
    bool backlog_include_font_ = true;
    GlyphIconConfig glyph_icon_;
};

bool next_codepoint(std::string_view s, size_t* i, uint32_t* cp);
LaidPage layout_page(const MessageLayer& layer, const MetricsFn& metrics,
                     const LayoutConfig& cfg, void* userdata);

} // namespace oa::render
