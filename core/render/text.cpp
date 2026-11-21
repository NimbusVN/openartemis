#include "core/render/text.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oa::render {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static std::string_view trimv(std::string_view v) {
    while (!v.empty() && std::isspace((unsigned char)v.front())) v.remove_prefix(1);
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.remove_suffix(1);
    return v;
}

static bool parse_u64s(std::string_view v, uint64_t* out) {
    v = trimv(v);
    if (v.empty()) return false;
    const std::string s(v);
    char* end = nullptr;
    errno = 0;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(d)) return false;
    if (d <= 0.0) {
        *out = 0;
        return true;
    }
    if (d >= 1e12) {
        *out = 1000000000000ull;
        return true;
    }
    *out = uint64_t(std::llround(d));
    return true;
}
static bool parse_dbl(std::string_view v, double* out) {
    v = trimv(v);
    if (v.empty()) return false;
    const std::string s(v);
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(d)) return false;
    *out = d;
    return true;
}

// FontDesc -------------------------------------------------------------
const std::string* FontDesc::get(std::string_view key) const {
    const auto it = raw.find(std::string(key));
    return it == raw.end() ? nullptr : &it->second;
}
std::string FontDesc::get_or(std::string_view key, const std::string& fallback) const {
    const std::string* v = get(key);
    return v ? *v : fallback;
}
double FontDesc::size() const {
    const std::string* s = get("size");
    if (!s) return kDefaultFontSize;
    double d = 0;
    if (!parse_dbl(*s, &d)) return kDefaultFontSize;
    return d;
}
std::string FontDesc::face() const { return get_or("face", ""); }
std::string FontDesc::color() const { return get_or("color", ""); }
double FontDesc::spacetop() const {
    const std::string* s = get("spacetop");
    double d = 0;
    if (s && parse_dbl(*s, &d)) return d;
    return 0.0;
}
double FontDesc::spacemiddle() const {
    const std::string* s = get("spacemiddle");
    double d = 0;
    if (s && parse_dbl(*s, &d)) return d;
    return 0.0;
}
double FontDesc::spacebottom() const {
    const std::string* s = get("spacebottom");
    double d = 0;
    if (s && parse_dbl(*s, &d)) return d;
    return 0.0;
}
double FontDesc::rubysize() const {
    const std::string* s = get("rubysize");
    double d = 0;
    if (s && parse_dbl(*s, &d)) return d;
    return 0.0;
}
void FontDesc::merge_raw(const std::map<std::string, std::string>& p) {
    for (const auto& [k, v] : p) raw[k] = v; // 参数保形
}

// BacklogTag / BacklogPage ---------------------------------------------
// 重放命令串：`name,key,val,key,val`。逗号与反斜杠按框架的写法转义
// （save.iet 自己也是 `gsub("([^\\]),", "%1\\,")`），解释器的 `[tag data]`
// 解析器负责还原。
static std::string escape_field(const std::string& v) {
    std::string r;
    r.reserve(v.size());
    for (char c : v) {
        if (c == '\\') r += "\\\\";
        else if (c == ',') r += "\\,";
        else r += c;
    }
    return r;
}

/// `name` + 参数对。`first` 里的键排在最前面 —— 框架用
/// `string.find(s, "font,__noreprint,1") == 1`（位置 1 = 串首）识别它们。
static std::string build_command(const std::string& name,
                                 const std::map<std::string, std::string>& params,
                                 std::initializer_list<const char*> first = {}) {
    std::string s = name;
    auto emit = [&s](const std::string& k, const std::string& v) {
        s += "," + k + "," + escape_field(v);
    };
    for (const char* k : first) {
        const auto it = params.find(k);
        if (it != params.end()) emit(it->first, it->second);
    }
    for (const auto& [k, v] : params) {
        bool skipped = false;
        for (const char* f : first) {
            if (k == f) {
                skipped = true;
                break;
            }
        }
        if (!skipped) emit(k, v);
    }
    return s;
}

std::string BacklogTag::get_string() const {
    switch (kind) {
        case Kind::Text:
            // 单一语言形态（save.iet 的 saveMakeTagsFromSelectData 也这么写）。
            return "print,data," + escape_field(text);
        case Kind::LineBreak:
            return build_command("rt", params, {"noveldummystart", "noveldummyend"});
        case Kind::Font:
            return build_command("font", font, {"__noreprint", "frexname"});
        case Kind::FontInit:
            return "fontinit";
        case Kind::RubyStart:
            return "ruby,text," + escape_field(text);
        case Kind::RubyEnd:
            return "/ruby";
    }
    return "";
}

bool BacklogPage::has_text() const {
    for (const BacklogTag& t : tags)
        if (t.kind == BacklogTag::Kind::Text) return true;
    return false;
}
std::vector<std::string> BacklogPage::reproduction_tags(bool allfont) const {
    std::vector<std::string> out;
    if (allfont && !page_font.empty()) {
        out.push_back(build_command("font", page_font, {"__noreprint", "frexname"}));
    }
    for (const BacklogTag& t : tags) out.push_back(t.get_string());
    return out;
}

// GlyphIconConfig -------------------------------------------------------
GlyphIconConfig glyph_icon_from_raw(const std::map<std::string, std::string>& raw) {
    GlyphIconConfig c;
    auto non_empty = [&](const char* k) -> const std::string* {
        const auto it = raw.find(k);
        if (it == raw.end()) return nullptr;
        const std::string& v = it->second;
        const std::string_view t = trimv(v);
        return t.empty() ? nullptr : &it->second;
    };
    auto set_d = [&](const char* k, double* d) {
        const std::string* v = non_empty(k);
        if (v) (void)parse_dbl(*v, d);
    };
    if (const std::string* v = non_empty("layer")) c.layer = *v;
    if (const std::string* v = non_empty("rplayer")) c.rplayer = *v;
    set_d("left", &c.left);
    set_d("top", &c.top);
    set_d("rpleft", &c.rpleft);
    set_d("rptop", &c.rptop);
    if (const std::string* v = non_empty("homing")) c.homing = *v == "1";
    return c;
}

// default layout charsets
static const char* kDefaultProhibitHead = "!?%)]},.:;、。，．・：；！？」』）｝〕］】";
static const char* kDefaultProhibitFoot = "([{「『（｛〔［【";
static const char* kDefaultWordparts =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

// 单帧最多推进的逐字时间（长帧不折算成批量出字）。
static constexpr uint64_t kRevealMaxStepMs = 50;

// UTF-8 iteration --------------------------------------------------------
bool next_codepoint(std::string_view s, size_t* i, uint32_t* cp) {
    if (*i >= s.size()) return false;
    const uint8_t c = (uint8_t)s[*i];
    if (c < 0x80) {
        *cp = c;
        ++*i;
        return true;
    }
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
    uint32_t v = c & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
    for (int k = 1; k <= extra; ++k) {
        if (*i + (size_t)k >= s.size()) return false;
        v = (v << 6) | ((uint8_t)s[*i + (size_t)k] & 0x3F);
    }
    *i += (size_t)extra + 1;
    *cp = v;
    return true;
}

static bool contains_cp(std::string_view set, uint32_t cp) {
    size_t i = 0;
    uint32_t c = 0;
    while (next_codepoint(set, &i, &c)) {
        if (c == cp) return true;
    }
    return false;
}

// TextEngine -------------------------------------------------------------
TextEngine::TextEngine() {
    layout_cfg_.prohibit_head = kDefaultProhibitHead;
    layout_cfg_.prohibit_foot = kDefaultProhibitFoot;
    layout_cfg_.wordparts = kDefaultWordparts;
    default_font_.raw["face"] = std::string(kDefaultFontFile);
}
TextEngine::~TextEngine() = default;

void TextEngine::attach(TextStore* store) { store_ = store; }

void TextEngine::bump_revision() { ++revision_; }

// 存储访问 ---------------------------------------------------------------
MessageLayer* TextEngine::find_text(const std::string& id) const {
    if (store_) return store_->find_text(id);
    const auto it = own_.find(id);
    return it == own_.end() ? nullptr : &it->second;
}

std::vector<std::string> TextEngine::text_ids() const {
    if (store_) return store_->text_ids();
    std::vector<std::string> ids;
    ids.reserve(own_.size());
    for (const auto& [id, l] : own_) ids.push_back(id);
    return ids;
}

MessageLayer& TextEngine::ensure_text(const std::string& id) {
    if (store_) {
        MessageLayer& l = store_->ensure_text(id);
        // 新建/复活的层：把会话配置装回去（font/盒/scetween）。框架只在
        // 启动时对这些 id 设一次 font，节点却会被 lydel 删了重建 —— 配置
        // 必须跟着 id 活，不能跟着节点死。
        apply_profile(id, &l);
        return l;
    }
    auto it = own_.find(id);
    if (it == own_.end()) {
        MessageLayer l;
        l.id = id;
        l.font = default_font_;
        l.page_font = default_font_.raw;
        it = own_.emplace(id, std::move(l)).first;
        apply_profile(id, &it->second);
    }
    return it->second;
}

TextProfile& TextEngine::profile(const std::string& id) {
    TextProfile& p = profiles_[id];
    if (!p.valid) {
        p.font = default_font_;
        p.valid = true;
    }
    return p;
}

void TextEngine::save_profile(const std::string& id, const MessageLayer& l) {
    TextProfile& p = profile(id);
    p.font = l.font;
    p.font_stack = l.font_stack;
    p.positioned = l.positioned;
    p.left = l.left;
    p.top = l.top;
    p.width = l.width;
    p.height = l.height;
    p.box_width = l.box_width;
    p.entire_x = l.entire_x;
    p.entire_y = l.entire_y;
    p.scetween = l.scetween;
}

void TextEngine::apply_profile(const std::string& id, MessageLayer* l) {
    if (!l) return;
    const auto it = profiles_.find(id);
    if (it == profiles_.end() || !it->second.valid) return;
    const TextProfile& p = it->second;
    // 只填"还没配过"的层：同一个节点已经排版过就别覆盖它的现场。
    if (!l->positioned) {
        l->font = p.font;
        l->font_stack = p.font_stack;
        l->positioned = p.positioned;
        l->left = p.left;
        l->top = p.top;
        l->width = p.width;
        l->height = p.height;
        l->entire_x = p.entire_x;
        l->entire_y = p.entire_y;
        if (l->page_font.empty()) l->page_font = p.font.raw;
    }
    // 框宽与字体盒独立：positioned 层也可能只有 left/top（width=0），
    // 这时仍要靠会话里记住的消息窗框宽折行。
    if (!(l->box_width > 0.0)) l->box_width = p.box_width;
    if (l->scetween.empty()) l->scetween = p.scetween;
}

void TextEngine::clear_scene() {
    active_.reset();
    layer_stack_.clear();
    ruby_open_ = false;
    ruby_annotation_.clear();
    // 按 id 的字体/几何配置 = 会话级，留在 profiles_ 里（load 之后框架自己
    // 会重新 [chgmsg]+[font]；不清是为了让"同一 id 的下一次 chgmsg"有排版）。
    for (const std::string& id : text_ids()) {
        MessageLayer* l = find_text(id);
        if (!l) continue;
        l->page.clear();
        l->page_tags.clear();
        l->page_font = l->font.raw; // 页首快照重置为当前字体
        l->shown = 0;
        l->start_ms = 0;
        l->clock_started = false;
        l->done = false;
        l->char_count = 0;
        l->text_hidden = false;
        l->last_page_key.clear();
        l->last_page_complete = false;
        ++l->generation;
    }
    bump_revision();
}

static bool is_blank_cp(uint32_t cp);

void TextEngine::anchor_active_to_content() {
    // 只认"含非空白字形、未隐藏"的内容层；空白测量层（basefont.font0x
    if (!layer_stack_.empty()) return;
    // 操作界面的文字层（`500.pageno` / `980` / `101.sblock.*`）不是剧情页：
    // 它们可能比正文更长（980 是存档提示），按"最大 char_count"会把活动层
    // 抢到提示层上——之后剧本的 print 全打进提示层，正文停在上一页。
    // 页面/提示根是纯数字（剧情画面根固定是 "1"，`1.80.mw.adv` 不算）。
    auto numeric_page_root = [](const std::string& id) {
        const std::string root = id.substr(0, id.find('.'));
        if (root.empty() || root == "1") return false;
        for (char c : root)
            if (c < '0' || c > '9') return false;
        return true;
    };
    std::string best;
    size_t best_chars = 0;
    for (const std::string& id : text_ids()) {
        if (numeric_page_root(id)) continue;
        const MessageLayer* lp = find_text(id);
        if (!lp) continue;
        const MessageLayer& l = *lp;
        if (l.text_hidden || l.page.empty() || l.char_count == 0) continue;
        bool has_visible = false;
        for (const PageUnit& u : l.page) {
            if (u.kind == PageUnit::Kind::Newline || u.data.empty()) continue;
            size_t i = 0;
            uint32_t cp = 0;
            while (next_codepoint(u.data, &i, &cp)) {
                if (!is_blank_cp(cp)) {
                    has_visible = true;
                    break;
                }
            }
            if (has_visible) break;
        }
        if (!has_visible) continue;
        if (l.char_count > best_chars) {
            best_chars = l.char_count;
            best = id;
        }
    }
    if (best.empty()) return;
    active_ = best;
    layer_stack_.clear();
    bump_revision();
}

std::string TextEngine::anon_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const uint64_t nanos =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    char buf[72];
    std::snprintf(buf, sizeof(buf), "chgmsg_%llx_%llu", (unsigned long long)nanos,
                  (unsigned long long)anonymous_serial_++);
    return std::string(buf);
}

size_t TextEngine::recompute_char_count(MessageLayer& l) const {
    size_t n = 0;
    for (const PageUnit& u : l.page) {
        if (u.kind == PageUnit::Kind::Newline) {
            ++n; // 换行字形也计入缓冲长度（shown 语义）
            continue;
        }
        size_t i = 0;
        uint32_t cp = 0;
        while (next_codepoint(u.data, &i, &cp)) ++n;
    }
    l.char_count = n;
    return n;
}

MessageLayer& TextEngine::active_mut() {
    const std::string id = active_.value_or(std::string(kDefaultMessageLayer));
    active_ = id;
    return ensure_text(id);
}

std::string TextEngine::active_layer_id() const {
    return active_.value_or(std::string(kDefaultMessageLayer));
}
const MessageLayer* TextEngine::layer(std::string_view id) const {
    return find_text(std::string(id));
}

void TextEngine::switch_layer(std::optional<std::string> id, bool stack,
                              std::optional<int> layered) {
    if (stack && active_) {
        layer_stack_.push_back(*active_);
    }
    if (!id || id->empty()) id = anon_id();
    active_ = *id;
    MessageLayer& l = active_mut();
    // 切到这一层 = 框架在这一行上开始新的一页；[sceout] 的隐藏标记不能
    // 留到新页（否则文字打出来却看不见）。
    if (l.text_hidden) l.text_hidden = false;
    // layered only changes when the tag explicitly carries it — a
    if (layered.has_value()) l.layered = *layered == 1;

    // NO cross-layer geometry/font inheritance. A message
    bump_revision();
}

void TextEngine::forget_node(const std::string& id) {
    if (id.empty()) return;
    MessageLayer* lp = find_text(id);
    if (!lp) return;
    MessageLayer& l = *lp;
    // 内容：页、逐字游标、隐藏标记、重打快照 —— 全部失去落点。
    l.page.clear();
    l.page_tags.clear();
    l.page_font = l.font.raw; // 页首快照回落到当前字体
    l.generation++;
    l.shown = 0;
    l.start_ms = 0;
    l.clock_started = false;
    l.done = false;
    l.char_count = 0;
    l.text_hidden = false;
    l.last_page_key.clear();
    l.last_page_complete = false;
    // 配置（font / font_stack / left,top,width,height / positioned /
    // scetween）不动：那是 id 级的，等下一次 [chgmsg]（见 apply_profile）。
    save_profile(id, l);
    bump_revision();
}

void TextEngine::pop_layer() {
    if (!layer_stack_.empty()) {
        active_ = layer_stack_.back();
        layer_stack_.pop_back();
    }
    bump_revision();
}

void TextEngine::apply_font(const std::map<std::string, std::string>& p) {
    MessageLayer& l = active_mut();
    // entirexscale / entireyscale: the framework's font rows are authored in an
    // enlarged font space; size/行距/文本框宽都要乘上这个百分比。字体表是
    // **分几段**下发的（底座 cn_tag_font.csv 带 entirexscale，角色窗的
    // cn_tag_font_その他女性.csv 只补 left/top/width/height）。底座把
    // width 从 1550 缩到 1023 之后，角色窗又把 width=570 原样盖回来 ——
    // 折行宽度就变成未缩放的整块纹理宽，中文一句话永远不折（test10
    // s100.txt 的「好棒……」本该 2 行）。因此把当前生效的缩放因子记在层上，
    // 后续**不带** entirexscale 的分段也按同一因子换算它的排版键。
    double sx = 100.0;
    double sy = 100.0;
    const auto sit_x = p.find("entirexscale");
    const auto sit_y = p.find("entireyscale");
    const bool has_sx = sit_x != p.end();
    const bool has_sy = sit_y != p.end();
    if (has_sx) parse_dbl(sit_x->second, &sx);
    if (has_sy) parse_dbl(sit_y->second, &sy);
    if (sx <= 0.0) sx = 100.0;
    if (sy <= 0.0) sy = 100.0;
    if (has_sx) l.entire_x = sx;
    if (has_sy) l.entire_y = sy;
    // 本段没有自带 entirexscale 时，沿用层上记住的因子（底座已经缩放过
    // 一次，这里只对**本段带来的**键再换算一次，不会重复乘）。
    const double fx = has_sx ? sx : l.entire_x;
    const double fy = has_sy ? sy : l.entire_y;
    std::map<std::string, std::string> scaled = p;
    if (fx != 100.0 || fy != 100.0) {
        auto scale_key = [&](const char* key, double factor) {
            const auto it = scaled.find(key);
            if (it == scaled.end()) return;
            double v = 0.0;
            if (!parse_dbl(it->second, &v)) return;
            it->second = std::to_string(v * factor / 100.0);
        };
        scale_key("size", fy);
        scale_key("kerning", fx);
        // width/height describe the TEXT BOX in the enlarged font space (the
        scale_key("width", fx);
        scale_key("height", fy);
        scale_key("spacetop", fy);
        scale_key("spacemiddle", fy);
        scale_key("spacebottom", fy);
        scale_key("outline", fy);
        scale_key("rubysize", fy);
        scaled["entirexscale"] = "100";
        scaled["entireyscale"] = "100";
    }
    const std::map<std::string, std::string>& p_use = scaled;
    const auto sit = p_use.find("stack");
    const bool stacked = sit == p_use.end() || sit->second == "1" || sit->second == "true";
    if (stacked) l.font_stack.push_back(l.font);
    l.font.merge_raw(p_use);
    auto set_d = [&p_use](const char* key, double* dst) {
        const auto it = p_use.find(key);
        if (it == p_use.end()) return;
        (void)parse_dbl(it->second, dst);
    };
    set_d("left", &l.left);
    set_d("top", &l.top);
    set_d("width", &l.width);
    set_d("height", &l.height);
    // 排版键 → 本层获得绘制盒（无盒层只能经脚本显式节点绘制,孤儿打印
    // 不绘制;继承/默认字体不置位,见 MessageLayer::positioned）。
    if (p_use.count("left") || p_use.count("top") || p_use.count("width") ||
        p_use.count("height"))
        l.positioned = true;
    // 再现记录：font 参数原样入本页标签序列（get_message_tags/backlog）
    BacklogTag t;
    t.kind = BacklogTag::Kind::Font;
    t.font = p_use;
    l.page_tags.push_back(std::move(t));
    // 会话配置随 id 存：节点被 [lydel] 删掉再建时，下一次 [chgmsg] 靠这张
    // profile 把 box/font 还回来（框架只在启动时设一次）。
    save_profile(active_layer_id(), l);
    bump_revision();
}

void TextEngine::font_init() {
    MessageLayer& l = active_mut();
    l.font = default_font_;
    l.font_stack.clear();
    // 框架的 `scenarioRePrint` 以 `fontinit` 作为"正文到此为止"的界标
    // （`string.find(mes_tag,"fontinit")==1` 就 break）：重放串必须是裸命令，
    // 不能降级成一条 font 参数表。
    BacklogTag t;
    t.kind = BacklogTag::Kind::FontInit;
    l.page_tags.push_back(std::move(t));
    save_profile(active_layer_id(), l);
    bump_revision();
}

void TextEngine::font_close() {
    MessageLayer& l = active_mut();
    if (!l.font_stack.empty()) {
        l.font = l.font_stack.back();
        l.font_stack.pop_back();
        BacklogTag t;
        t.kind = BacklogTag::Kind::Font;
        t.font = l.font.raw;
        l.page_tags.push_back(std::move(t));
        save_profile(active_layer_id(), l);
        bump_revision();
    }
}

void TextEngine::font_default(const std::map<std::string, std::string>& p) {
    default_font_.merge_raw(p);
}

namespace {
/// 一页的纯文本（[print] 的原文；换行算 '\n'）。
std::string page_plain_text(const std::vector<PageUnit>& page) {
    std::string out;
    for (const PageUnit& u : page) {
        if (u.kind == PageUnit::Kind::Newline) {
            out += '\n';
        } else if (u.kind == PageUnit::Kind::Text) {
            out += u.data;
        }
    }
    return out;
}

/// 一页的**内容指纹**：字形 + 每段的字号/颜色 + 注音。
///
/// 用来识别"同一页原样重打"（关掉 UI 后 `[rp]` + 重打原文）。指纹在
/// 每次 push_text 之后重算、与上一页比较 —— 一页分几段打印也能认出来
/// （只有最后一段打完，整页指纹才会与上一页相等）。
std::string page_key_of(const std::vector<PageUnit>& page, const FontDesc& def_font) {
    std::string k;
    for (const PageUnit& u : page) {
        k += (u.kind == PageUnit::Kind::Newline) ? std::string("\n") : u.data;
        k += '\x1f';
        k += u.font.get_or("size", "");
        k += '\x1e';
        k += u.font.get_or("color", "");
        k += '\x1d';
        if (u.kind == PageUnit::Kind::RubyBase) {
            k += 'r';
            k += u.ruby_annotation;
        }
    }
    if (page.empty()) k += def_font.get_or("size", "");
    return k;
}
} // namespace

void TextEngine::arm_reload_replace(const std::string& id) {
    if (id.empty()) return;
    ensure_text(id).reload_replace_pending = true;
}

void TextEngine::push_text(const std::string& content) {
    MessageLayer& l = active_mut();
    if (l.reload_replace_pending) {
        // 读档恢复的那一页遇到框架的重绘：整页替换（不追加）。
        l.reload_replace_pending = false;
        l.page.clear();
        l.page_tags.clear();
        l.page_font = l.font.raw;
        l.generation++;
        l.shown = 0;
        l.start_ms = 0;
        l.clock_started = false;
        l.done = false;
        l.text_hidden = false;
        l.char_count = 0;
        l.last_page_key.clear();
        l.last_page_complete = false;
    }
    // 追加前已经排队的字形数：决定这次 push 是"开新页"还是"往当前页续写"。
    const size_t old_count = l.char_count;
    // 内容内换行（LF）语义归一。push_text 把 content 按 '\n' 切成
    auto push_segment = [&](const std::string& data) {
        PageUnit u;
        u.kind = ruby_open_ ? PageUnit::Kind::RubyBase : PageUnit::Kind::Text;
        u.data = data;
        if (ruby_open_) u.ruby_annotation = ruby_annotation_;
        u.font = l.font;
        l.page.push_back(std::move(u));
        BacklogTag t;
        t.kind = BacklogTag::Kind::Text;
        t.text = data;
        l.page_tags.push_back(std::move(t));
    };
    if (content.empty()) {
        push_segment(content);
        recompute_char_count(l);
        bump_revision();
        return;
    }
    size_t pos = 0;
    for (;;) {
        const size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) {
            if (pos < content.size()) push_segment(content.substr(pos)); // 尾段(空则跳过)
            break;
        }
        size_t end = nl;
        if (end > pos && content[end - 1] == '\r') --end; // CRLF: '\r' 丢弃
        if (end > pos) push_segment(content.substr(pos, end - pos));
        PageUnit nu;
        nu.kind = PageUnit::Kind::Newline;
        nu.font = l.font;
        l.page.push_back(std::move(nu));
        BacklogTag nt;
        nt.kind = BacklogTag::Kind::LineBreak;
        l.page_tags.push_back(std::move(nt));
        pos = nl + 1;
    }
    recompute_char_count(l);
    // 整页指纹与"刚被 [rp] 清掉的那一页"相同，且那一页当时已经显示完
    // ⇒ 这是原样重打，直接算显示完（不重新逐字、不闪一下）。
    if (l.last_page_complete && !l.last_page_key.empty() &&
        page_key_of(l.page, l.font) == l.last_page_key) {
        l.shown = l.char_count;
        l.done = true;
        l.clock_started = false;
        bump_revision();
        return;
    }
    // 往当前页**续写**（[print] 可以多次、页尾还常跟一次 [rt]+[print]）：
    // 新字从"旧内容已经显示到的位置"开始逐字。若旧内容早就显示完，
    // 不能让它把新字一起算作已完成 —— 那样 char_count 涨了而 shown 不动，
    // is_reveal_complete 就永远等不到（翻页卡死）。
    if (l.char_count > old_count) {
        if (l.shown >= old_count) {
            l.shown = old_count;
            l.done = false;
            // advance 见到 !clock_started 会用 shown 反解 start_ms，
            // 从"第 old_count+1 个字"的节拍继续（不是从页首重来）。
            l.clock_started = false;
        }
    }
    bump_revision();
}

void TextEngine::line_break(bool omit_blank_line,
                            const std::map<std::string, std::string>& tag_params) {
    MessageLayer& l = active_mut();
    const bool page_empty = l.page.empty();
    const bool last_is_newline = !l.page.empty() && l.page.back().kind == PageUnit::Kind::Newline;
    if (omit_blank_line && (page_empty || last_is_newline)) return;
    PageUnit u;
    u.kind = PageUnit::Kind::Newline;
    u.font = l.font;
    l.page.push_back(std::move(u));
    BacklogTag t;
    t.kind = BacklogTag::Kind::LineBreak;
    t.params = tag_params;
    // omitblankline 只是引擎侧的排版开关；重放时保留也没有副作用，
    // 但框架的 dummy 标记必须排在最前（build_command 的 first 规则）。
    l.page_tags.push_back(std::move(t));
    bump_revision();
}

void TextEngine::page_break(std::optional<int> backlog_param) {
    MessageLayer& l = active_mut();
    // rp 参数 → 入库：1/0 无视 writebacklog；缺省按 write_mode
    auto should_store = [&]() -> bool {
        if (backlog_param.has_value()) return backlog_param.value() == 1;
        return backlog_write_mode_;
    };
    if (should_store()) {
        // 回顾页跳转重放：这一页已经在表里，只为它办一次"跳过入库"。
        // 别的层（名字层）的 [rp backlog=0] 不会匹配，标记继续等正文层。
        bool skip_archive = false;
        if (skip_archive_ && active_layer_id() == skip_archive_layer_ &&
            l.generation == skip_archive_generation_) {
            skip_archive = true;
            skip_archive_ = false;
        }
        BacklogPage page;
            page.page_font = l.page_font;
            page.tags = l.page_tags;
            // 回顾表 = 这一页的**重放命令**。跳转时由框架载入自己的存档
            // 载体并重放/快进，引擎不复制画面。
        // 入不入库只看 [writebacklog]（should_store）与本页有没有字。
        //
        // `[backlog allow=0]` 是**禁用引擎内蔵のバックログUI**（自前 UI を
        // 持つゲームが必ず呼ぶ：test10 的 backlog.iet *enable 就是这一句），
        // 不是"别记录"。把它当成记录开关会让自前バックログ永远拿到
        // get_backlog_size=0 ⇒ 点 LOG 只弹出"没有回顾"的提示，页面不出现。
        if (skip_archive) {
            // 重放页：表里已经有了，不重复入库。
        } else if (!page.has_text()) {
            // 不入库
        } else {
            if (!backlog_include_font_) {
                page.page_font.clear();
                page.tags.erase(
                    std::remove_if(page.tags.begin(), page.tags.end(),
                                   [](const BacklogTag& t) {
                                       return t.kind == BacklogTag::Kind::Font ||
                                              t.kind == BacklogTag::Kind::FontInit;
                                   }),
                    page.tags.end());
            }
            backlog_pages_.push_back(std::move(page));
            while (backlog_pages_.size() > kBacklogMaxPages) {
                backlog_pages_.erase(backlog_pages_.begin());
            }
        }
    }
    // 清页前记下这一页的指纹与"是否已显示完"：紧接着如果是原样重打
    // （关 UI 后的重绘），push_text 会命中它。
    l.last_page_key = page_key_of(l.page, l.font);
    l.last_page_complete = l.done && l.char_count > 0;
    l.page.clear();
    l.page_tags.clear();
    l.page_font = l.font.raw; // 以当前字体重置页首快照
    l.generation++;
    l.shown = 0;
    l.start_ms = 0;
    l.clock_started = false;
    l.done = false;
    // [sceout] hides the text that is on the page. A page break drops that
    // text, so the flag must not outlive it: the next [print] starts a fresh
    // page that has to be readable. (test13 backlog quick-jump: opening a UI
    // [sceout]s the message layers, the jump path skips the normal UI-close
    // [msg_show], and the replay repaints the page with [rp]+[print] — the
    // stale flag kept that repainted page invisible.)
    l.text_hidden = false;
    l.char_count = 0;
    ruby_open_ = false;
    bump_revision();
}

void TextEngine::ruby_start(const std::string& annotation) {
    ruby_open_ = true;
    ruby_annotation_ = annotation;
    BacklogTag t;
    t.kind = BacklogTag::Kind::RubyStart;
    t.text = annotation;
    MessageLayer& l = active_mut();
    l.page_tags.push_back(std::move(t));
    bump_revision();
}
void TextEngine::ruby_end() {
    ruby_open_ = false;
    ruby_annotation_.clear();
    BacklogTag t;
    t.kind = BacklogTag::Kind::RubyEnd;
    MessageLayer& l = active_mut();
    l.page_tags.push_back(std::move(t));
    bump_revision();
}

void TextEngine::set_text_hidden(bool hidden) {
    active_mut().text_hidden = hidden;
    bump_revision();
}

void TextEngine::set_prohibit(const std::string& head, const std::string& foot) {
    if (!head.empty()) layout_cfg_.prohibit_head = head;
    if (!foot.empty()) layout_cfg_.prohibit_foot = foot;
}
void TextEngine::set_wordparts(const std::string& parts) { layout_cfg_.wordparts = parts; }
void TextEngine::set_indent(const std::string& pair) { layout_cfg_.indent_pair = pair; }

void TextEngine::set_message_box_width(const std::string& id, double w) {
    MessageLayer* l = find_text(id);
    if (!l) return;
    if (l->box_width == w) return;
    l->box_width = w;
    save_profile(id, *l);
    bump_revision();
}

namespace {
/// 这一层当前生效的"入场逐字间隔"（毫秒/字）。0 = 瞬时。
///
/// 只看入场方向（is_entrance() == !text_hidden）：[sceout] 之后是出场那套
/// 参数，不该拿来推进显示。多条入口 scetween 取第一条有间隔的（框架下发
/// 的就是一条 alpha + 可选一条 top，间隔相同）。
uint64_t entrance_per_char(const MessageLayer& l) {
    for (const ScetweenConfig& c : l.scetween) {
        if (c.is_entrance() == l.text_hidden) continue;
        if (c.per_char_ms() > 0) return c.per_char_ms();
    }
    return 0;
}
} // namespace

// [scetween]：type/mode/param/ease/diff/delay/time/randomdelay
// （ScetweenConfig::from_params）
void TextEngine::apply_scetween(const std::map<std::string, std::string>& p) {
    auto get = [&p](const char* k) -> std::string {
        const auto it = p.find(k);
        return it == p.end() ? std::string() : it->second;
    };
    ScetweenConfig c;
    const std::string type = get("type");
    c.mode = (type == "out") ? ScetweenMode::Out
              : (type == "show") ? ScetweenMode::Show
              : (type == "hide") ? ScetweenMode::Hide
              : (type == "backlog_down_in") ? ScetweenMode::BacklogDownIn
              : (type == "backlog_down_out") ? ScetweenMode::BacklogDownOut
              : (type == "backlog_up_in") ? ScetweenMode::BacklogUpIn
              : (type == "backlog_up_out") ? ScetweenMode::BacklogUpOut
                                          : ScetweenMode::In;
    c.set_add = get("mode") == "add";
    c.param = get("param");
    c.ease = get("ease");
    (void)parse_dbl(get("diff"), &c.diff);
    (void)parse_u64s(get("delay"), &c.delay_per_char);
    (void)parse_u64s(get("time"), &c.time_per_char);
    c.random_delay = get("randomdelay") == "1";
    MessageLayer& l = active_mut();
    if (!c.set_add) {
        const ScetweenMode m = c.mode;
        l.scetween.erase(std::remove_if(l.scetween.begin(), l.scetween.end(),
                                        [m](const ScetweenConfig& x) { return x.mode == m; }),
                         l.scetween.end());
    }
    l.scetween.push_back(std::move(c));
    // 改速度（config 页就是这么干的：`[scetween mode=init]` 清掉旧的，
    // 再按新 delay 下发）。正在逐字的页要从**当前位置**继续，所以把
    // start_ms 反解成"用新速度走完已显示的字数正好是现在"。
    //
    // 反解而不是重置：重置会让 start_ms = now，画面上的字一下子退回到
    // 第 1 个（用户看到的就是"调完速度文字不见了/又从头打"）。
    if (!l.done && l.clock_started && l.char_count > 0) {
        const uint64_t per = entrance_per_char(l);
        if (per > 0) {
            const uint64_t behind = l.shown > 1 ? uint64_t(l.shown - 1) * per : 0;
            l.start_ms = last_now_ms_ > behind ? last_now_ms_ - behind : 0;
        }
    }
    save_profile(active_layer_id(), l);
    bump_revision();
}

void TextEngine::set_glyph_config(const std::map<std::string, std::string>& raw) {
    glyph_icon_ = glyph_icon_from_raw(raw);
    bump_revision();
}

void TextEngine::apply_backlog_config(const std::map<std::string, std::string>& p, bool clear) {
    auto get = [&p](const char* k) -> const std::string* {
        const auto it = p.find(k);
        return it == p.end() ? nullptr : &it->second;
    };
    if (const std::string* v = get("allow")) backlog_allow_ = *v != "0";
    if (const std::string* v = get("includefont")) backlog_include_font_ = *v != "0";
    if (clear) backlog_pages_.clear();
    bump_revision();
}

void TextEngine::set_backlog_write_mode(bool mode) {
    backlog_write_mode_ = mode;
    bump_revision();
}

void TextEngine::advance(uint64_t now_ms) {
    last_now_ms_ = now_ms;
    bool changed = false;
    for (const std::string& id : text_ids()) {
        MessageLayer* lp = find_text(id);
        if (!lp) continue;
        MessageLayer& l = *lp;
        if (l.done || l.char_count == 0) continue;
        if (!l.clock_started) {
            // 起点 = "第 shown 个字的时刻"。首帧 shown=0 → 就是现在；
            // 页内续写（shown=old_count）→ 反解回去，节拍从续写处继续。
            const uint64_t per_now = entrance_per_char(l);
            const uint64_t behind =
                (per_now > 0 && l.shown > 1) ? uint64_t(l.shown - 1) * per_now : 0;
            l.start_ms = now_ms > behind ? now_ms - behind : 0;
            l.clock_started = true;
        }
        const uint64_t per = entrance_per_char(l);
        // 没有逐字参数（或间隔为 0）：整页瞬时显示。不再有"首帧不推进"的
        // 特例 —— 首帧本来就没过去时间，算出来就是 1 个字。
        const size_t shown = per == 0
                                 ? l.char_count
                                 : std::min<size_t>(
                                       1 + size_t((now_ms > l.start_ms ? now_ms - l.start_ms : 0) / per),
                                       l.char_count);
        if (shown != l.shown) {
            l.shown = shown;
            changed = true;
        }
        if (l.shown >= l.char_count) {
            l.done = true;
            changed = true;
        }
    }
    // 只有真的推进了才失效帧计划（静态帧跳过不被逐字常驻破坏）
    if (changed) bump_revision();
}

bool TextEngine::is_reveal_paced() const {
    for (const std::string& id : text_ids()) {
        const MessageLayer* lp = find_text(id);
        if (!lp) continue;
        const MessageLayer& l = *lp;
        if (l.done || l.char_count == 0) continue;
        if (entrance_per_char(l) > 0) return true;
    }
    return false;
}

void TextEngine::reveal_all() {
    for (const std::string& id : text_ids()) {
        MessageLayer* lp = find_text(id);
        if (!lp) continue;
        MessageLayer& l = *lp;
        if (l.char_count == 0) {
            // 空页没有可显示的字：直接算完成，别让它挡住翻页。
            l.done = true;
            l.clock_started = false;
            continue;
        }
        l.shown = l.char_count;
        l.done = true;
        l.clock_started = false;
    }
    bump_revision();
}

bool TextEngine::is_reveal_complete() const {
    for (const std::string& id : text_ids()) {
        const MessageLayer* lp = find_text(id);
        if (!lp) continue;
        const MessageLayer& l = *lp;
        // 空页 / 隐藏页 / 尚未打印的层不参与判定。以前遍历全表问
        // 的"未完成"标志，任何一页残留都让翻页永远等下去。
        if (l.char_count == 0 || l.text_hidden) continue;
        if (!l.done) return false;
    }
    return true;
}

std::optional<ClickWaitPlacement> TextEngine::click_wait_placement(bool page_end) const {
    // click_wait_icon_placement：行末/页末图层与偏移；两处都缺省禁用。
    const std::string layer_id = page_end
                                     ? (glyph_icon_.rplayer.empty() ? glyph_icon_.layer
                                                                    : glyph_icon_.rplayer)
                                     : glyph_icon_.layer;
    if (layer_id.empty()) return std::nullopt;
    const double dx = page_end ? glyph_icon_.rpleft : glyph_icon_.left;
    const double dy = page_end ? glyph_icon_.rptop : glyph_icon_.top;
    const std::string lid = active_.value_or(std::string(kDefaultMessageLayer));
    const MessageLayer* l = layer(lid);
    if (!l) return std::nullopt;
    // 本实现不追踪字形末位置（无 glyph 缓存几何），用文本区几何近似：
    // 图标放在该层文本区末尾（left+width, top）。
    ClickWaitPlacement p;
    p.layer_id = layer_id;
    p.left = l->left + (l->width > 0 ? l->width : 0) + dx;
    p.top = l->top + dy;
    return p;
}

std::vector<std::string> TextEngine::visible_content_layers() const {
    std::vector<std::string> out;
    for (const std::string& id : text_ids()) {
        const MessageLayer* lp = find_text(id);
        if (!lp) continue;
        const MessageLayer& l = *lp;
        if (l.text_hidden) continue;
        bool has = false;
        for (const PageUnit& u : l.page) {
            if (u.kind != PageUnit::Kind::Newline && !u.data.empty()) {
                has = true;
                break;
            }
        }
        if (has && l.shown > 0) out.push_back(id);
    }
    return out;
}

bool TextEngine::active_has_content() const {
    const MessageLayer* l = layer(active_layer_id());
    if (!l || l->text_hidden) return false;
    for (const PageUnit& u : l->page) {
        if (u.kind != PageUnit::Kind::Newline && !u.data.empty()) return true;
    }
    return false;
}

static bool is_blank_cp(uint32_t cp) {
    return cp == 0x20 || cp == 0x3000 || cp == 0x9 || cp == 0xA || cp == 0xD;
}

bool TextEngine::page_has_visible_text(std::string_view id) const {
    const MessageLayer* l = layer(id);
    if (!l) return false;
    for (const PageUnit& u : l->page) {
        if (u.kind == PageUnit::Kind::Newline || u.data.empty()) continue;
        size_t i = 0;
        uint32_t cp = 0;
        while (next_codepoint(u.data, &i, &cp)) {
            if (!is_blank_cp(cp)) return true;
        }
    }
    return false;
}

std::optional<std::vector<std::string>> TextEngine::backlog_tags(size_t page,
                                                                 bool allfont) const {
    if (page >= backlog_pages_.size()) return std::nullopt;
    return backlog_pages_[page].reproduction_tags(allfont);
}
std::optional<std::vector<std::string>> TextEngine::message_tags(std::string_view id,
                                                                 bool allfont) const {
    const MessageLayer* l = layer(id);
    if (!l) return std::nullopt;
    std::vector<std::string> out;
    if (allfont && !l->page_font.empty()) {
        out.push_back(BacklogTag{BacklogTag::Kind::Font, "", l->page_font}.get_string());
    }
    for (const BacklogTag& t : l->page_tags) out.push_back(t.get_string());
    return out;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
namespace {
struct InGlyph {
    uint32_t cp = 0;
    double advance = 0;
    double width = 0;
    const FontDesc* font = nullptr; // 仅 layout_page 内部临时用
    size_t unit = 0;                // layer.page 下标（写进 LaidGlyph）
    bool newline = false;
    bool is_base = false;
    std::string ruby_text;
};
std::vector<uint32_t> cps_of(std::string_view s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    uint32_t cp = 0;
    while (next_codepoint(s, &i, &cp)) out.push_back(cp);
    return out;
}
} // namespace

LaidPage layout_page(const MessageLayer& layer, const MetricsFn& metrics,
                     const LayoutConfig& cfg, void* userdata) {
    LaidPage out;
    std::vector<InGlyph> glyphs;
    std::vector<std::pair<size_t, size_t>> keep_ranges;
    for (size_t ui = 0; ui < layer.page.size(); ++ui) {
        const PageUnit& u = layer.page[ui];
        if (u.kind == PageUnit::Kind::Newline) {
            InGlyph g;
            g.newline = true;
            g.font = &u.font;
            g.unit = ui;
            glyphs.push_back(g);
            continue;
        }
        if (u.data.empty()) continue;
        const size_t start = glyphs.size();
        for (const uint32_t cp : cps_of(u.data)) {
            if (cp == '\n') {
                // 防御：若内容内 LF 因任何未来路径绕过 push 层归一
                // （push_text 已把 '\n' 切成 Newline 单元），布局层仍按显式
                // 断行字形处理——强制断行、advance 0、不度量（绘制面已跳过
                // newline 字形；行高/顺序/reveal 语义与 [rt] 单元一致）。
                InGlyph g;
                g.newline = true;
                g.font = &u.font;
                g.unit = ui;
                glyphs.push_back(g);
                continue;
            }
            InGlyph g;
            g.cp = cp;
            g.unit = ui;
            g.font = &u.font;
            g.is_base = u.kind == PageUnit::Kind::RubyBase;
            g.ruby_text = u.ruby_annotation;
            glyphs.push_back(g);
        }
        if (u.kind == PageUnit::Kind::RubyBase) {
            keep_ranges.push_back({start, glyphs.size()});
        }
    }
    if (glyphs.empty()) return out;

    // 逐字形步进/宽度来自宿主度量；正文行高按 text_line_metrics 公式
    // ：body_height = 层当前字体 size（ab_glyph
    // PxScale::from(sz) 下 sf.height ≡ sz），而非字形 typographic 高度或
    // 1.4×size 系数——否则 spacetop/spacemiddle/spacebottom 完全失效且
    // 行距/字形观感整体偏大（旧实现缺陷）。
    for (InGlyph& g : glyphs) {
        if (g.newline) continue;
        CharMetrics m;
        if (metrics) {
            m = metrics(userdata, *g.font, g.cp);
        } else {
            // 无宿主度量回调（不应发生）：按字号方块近似，仅保布局不塌缩。
            m.advance = g.font->size();
            m.width = g.font->size();
        }
        // 步进不含 kerning 参数:该键仅存/回显,glyph 排版从不读取
        // (layout_glyphs 只累加字形自带 advance_x)。
        g.advance = m.advance > 0.0 ? m.advance : 0.0;
        // 宽度=墨迹框（含 0 墨迹字形如空格 → 不参与换行判定；同 width=px_bounds）。
        g.width = m.width;
    }
    const double body_height = layer.font.size();
    const double spacetop = layer.font.spacetop();
    const double ruby_height = std::max(0.0, layer.font.rubysize());
    const double spacemiddle = layer.font.spacemiddle();
    const double spacebottom = layer.font.spacebottom();
    out.line_height = std::max(1.0, spacetop + ruby_height + spacemiddle + body_height + spacebottom);

    // 折行宽度 = 消息窗**文本区**宽，而不是整块图像纹理宽。取值优先级：
    //   1) 字体表显式文本宽（layer.width，[font width=]，CSV 的 width）；
    //   2) 绑定节点框宽（layer.box_width，宿主按消息窗纹理/裁剪宽扣掉左右
    //      内边距后写入）；
    //   3) 舞台宽 − 文本左偏移（纯兜底，避免无字体表的孤儿层退化成 1e18）。
    // 0/负值一律视为“该来源没给”，继续往下掉。
    double wrap_width = layer.width;
    if (!(wrap_width > 0.0)) wrap_width = layer.box_width;
    if (!(wrap_width > 0.0) && cfg.stage_width > 0.0)
        wrap_width = cfg.stage_width - std::max(0.0, layer.left);
    if (!(wrap_width > 0.0)) wrap_width = 1.0e18;
    const bool unbounded = wrap_width >= 1.0e17;
    const double line_width = wrap_width;
    const std::vector<uint32_t> pair_cps = cps_of(cfg.indent_pair);
    auto indent_close_of = [&pair_cps](uint32_t c) -> std::optional<uint32_t> {
        for (size_t k = 0; k + 1 < pair_cps.size(); k += 2) {
            if (pair_cps[k] == c) return pair_cps[k + 1];
        }
        return std::nullopt;
    };

    out.glyphs.reserve(glyphs.size());
    double indent_x = 0;
    std::vector<std::pair<uint32_t, double>> indent_stack;
    size_t line_start = 0;
    uint32_t line = 0;
    size_t order = 0;
    while (line_start < glyphs.size()) {
        double cx = indent_x;
        size_t break_at = glyphs.size();
        bool forced = false;
        size_t i = line_start;
        while (i < glyphs.size()) {
            const InGlyph& g = glyphs[i];
            if (g.newline) {
                break_at = i + 1;
                forced = true;
                break;
            }
            if (!unbounded && g.width > 0.0 && cx + g.advance > line_width &&
                i > line_start) {
                if (contains_cp(cfg.prohibit_head, g.cp)) {
                    cx += g.advance;
                    ++i;
                    continue;
                }
                size_t b = i;
                for (const auto& [rs, re] : keep_ranges) {
                    if (rs < b && b < re && rs > line_start) {
                        b = rs;
                        break;
                    }
                }
                if (b == i && contains_cp(cfg.wordparts, g.cp)) {
                    size_t j = i;
                    while (j > line_start && contains_cp(cfg.wordparts, glyphs[j - 1].cp))
                        --j;
                    if (j > line_start) b = j;
                }
                while (b > line_start + 1 && contains_cp(cfg.prohibit_foot, glyphs[b - 1].cp))
                    --b;
                break_at = b;
                break;
            }
            cx += g.advance;
            ++i;
        }
        cx = indent_x;
        size_t chars_in_line = 0;
        for (size_t k = line_start; k < break_at; ++k) {
            const InGlyph& g = glyphs[k];
            LaidGlyph lg;
            lg.cp = g.cp;
            lg.advance = g.advance;
            lg.width = g.width;
            lg.x = cx;
            lg.line = line;
            lg.unit = g.unit;
            lg.newline = g.newline;
            lg.has_ruby = g.is_base;
            lg.order = order++;
            out.glyphs.push_back(lg);
            if (g.newline) continue;
            if (const auto close = indent_close_of(g.cp)) {
                const bool within =
                    cfg.indent_range <= 0 || chars_in_line < (size_t)cfg.indent_range;
                if (within && (indent_stack.empty() || cfg.indent_nest)) {
                    indent_stack.push_back({*close, indent_x});
                    indent_x = cx + g.advance;
                }
            } else if (!indent_stack.empty() && indent_stack.back().first == g.cp) {
                indent_x = indent_stack.back().second;
                indent_stack.pop_back();
            }
            cx += g.advance;
            ++chars_in_line;
        }
        ++line;
        line_start = break_at;
        if (!forced && !unbounded && break_at >= glyphs.size()) break;
    }

    // 行对齐（align_layout）：align 键 center/right/
    // equalize 时按行内容宽（首字形 x .. 末字形 x+advance）对行宽居中/右/均排。
    // 仅当层宽 >0 且当前层字体声明了非左对齐时生效（TextAlignment::from）。
    if (!out.glyphs.empty() && !unbounded) {
        const std::string align = layer.font.get_or("align", "left");
        if (align != "left" && !align.empty()) {
            const double lw = wrap_width;
            // 收集每行非换行字形的首/末下标
            std::map<uint32_t, std::pair<size_t, size_t>> line_bounds; // first,last
            for (size_t i = 0; i < out.glyphs.size(); ++i) {
                if (out.glyphs[i].newline) continue;
                const uint32_t ln = out.glyphs[i].line;
                auto it = line_bounds.find(ln);
                if (it == line_bounds.end()) line_bounds[ln] = {i, i};
                else it->second.second = i;
            }
            auto apply_off = [&](const std::pair<size_t, size_t>& b, double off) {
                for (size_t i = b.first; i <= b.second; ++i) out.glyphs[i].x += off;
            };
            for (const auto& [ln, b] : line_bounds) {
                const double left = out.glyphs[b.first].x;
                const double right = out.glyphs[b.second].x + out.glyphs[b.second].advance;
                const double content = (right - left) > 0.0 ? (right - left) : 0.0;
                const double spare = (lw - content) > 0.0 ? (lw - content) : 0.0;
                const size_t n = b.second - b.first + 1;
                if (align == "center") {
                    apply_off(b, spare * 0.5 - left);
                } else if (align == "right") {
                    apply_off(b, spare - left);
                } else if (align == "equalize" && n > 1) {
                    const double step = spare / double(n - 1);
                    for (size_t i = b.first; i <= b.second; ++i)
                        out.glyphs[i].x += step * double(i - b.first) - left;
                } else if (align == "equalize") {
                    apply_off(b, -left);
                }
            }
        }
    }

    for (const auto& [rs, re] : keep_ranges) {
        if (rs >= re || rs >= out.glyphs.size()) continue;
        const LaidGlyph& first = out.glyphs[rs];
        const LaidGlyph* last = &first;
        for (size_t idx = rs + 1; idx < re && idx < out.glyphs.size(); ++idx) {
            if (out.glyphs[idx].line == first.line) last = &out.glyphs[idx];
        }
        LaidRuby r;
        r.line = first.line;
        r.x = first.x;
        // 正文区间右端 = 末字形笔端（x+advance；同 base_x1 =
        // laid[last].x + text_buffer[last].advance_x）。
        r.width = (last->x + last->advance) - first.x;
        r.text = glyphs[rs].ruby_text;
        const FontDesc* bf = glyphs[rs].unit < layer.page.size()
                                 ? &layer.page[glyphs[rs].unit].font
                                 : nullptr;
        r.size = 0;
        if (bf) {
            const std::string* rs2 = bf->get("rubysize");
            if (rs2) (void)parse_dbl(*rs2, &r.size);
            if (r.size <= 0) r.size = bf->size() * 0.5;
        }
        out.rubies.push_back(std::move(r));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 观察面
// ---------------------------------------------------------------------------
size_t TextEngine::text_layers() const { return text_ids().size(); }

std::vector<std::pair<std::string, const MessageLayer*>> TextEngine::all_layers() const {
    std::vector<std::pair<std::string, const MessageLayer*>> out;
    std::vector<std::string> ids = text_ids();
    std::sort(ids.begin(), ids.end());
    out.reserve(ids.size());
    for (const std::string& id : ids) {
        const MessageLayer* l = find_text(id);
        if (l) out.emplace_back(id, l);
    }
    return out;
}

} // namespace oa::render
