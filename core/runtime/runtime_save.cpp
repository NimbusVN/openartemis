// Save-domain glue.
//
// Save/load domain: binary SaveData through an injected SaveStore.
#include "core/runtime/runtime_internal.h"
#include "core/runtime/runtime_save.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <cstring>
#include <limits>

#include "core/util/binary_stream.h"
#include "core/runtime/runtime_save_official.h"

#include "core/media/image.h"
#include "core/render/layer.h"
#include "core/render/text.h"

namespace oa::runtime {

namespace {
constexpr const char* kSavegFile = "saveg.dat";
constexpr const char* kSystemFile = "system.dat";
constexpr const char* kAutosaveFile = "autosave.dat";

/// 引擎自己的域存档（g./s. 两个变量域）。
///
/// **不能再用 system.dat / saveg.dat**：那两个名字是**官方引擎**的
/// （BOWX / BOWG 容器，原生 exe 写、原生 exe 读）。本引擎的 syssave() 曾经直接
/// 盖在上面——`preserve_official_original()`（`.official.bak`）就是这个破坏的
/// 补丁。名字一旦抢过来，玩家搬来的官方系统档（设置、全局进度、已读表）就没了。
/// 引擎改用自己的一组名字，两边互不覆盖。
///
/// 注意：游戏自己的 `sys` 表 / `gscr` 是另两个文件（iMel: conf.dat / global.dat，
/// 由游戏的 `syssave()` / `system_save()` 用 pluto 写），与这里无关。
constexpr const char* kSavegOaFile = "saveg.oa.dat";
constexpr const char* kSystemOaFile = "system.oa.dat";

/// 判断一份文档是不是 OASB 容器（本引擎的 pluto 与域文档都用它当壳）。
/// 只用于"这份旧文件我们不认识，别动它"的日志分支。
bool is_oasb_document(const std::string& doc) {
    return doc.size() >= 4 && oa::util::is_magic(
                                   reinterpret_cast<const uint8_t*>(doc.data()));
}

bool is_input_wait(const oa::runtime::Wait* w) {
    return w && (w->waiting_click() || !w->keys.empty());
}

/// Drop dangerous/Windows-only segments from the ini SAVEPATH.
std::string sanitize_segments(const std::string& raw) {
    if (raw.empty()) return "save";
    std::string normalized = raw;
    for (char& c : normalized)
        if (c == '\\') c = '/';
    std::vector<std::string> cleaned;
    size_t start = 0;
    while (start <= normalized.size()) {
        const size_t slash = normalized.find('/', start);
        const std::string seg = normalized.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? normalized.size() + 1 : slash + 1;
        std::string t = seg;
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (t.empty() || t == "." || t == ".." || t.back() == ':') continue;
        cleaned.push_back(t);
    }
    if (cleaned.empty()) return "save";
    std::string out;
    for (size_t i = 0; i < cleaned.size(); ++i) {
        if (i) out += '/';
        out += cleaned[i];
    }
    return out;
}

bool clean_rel_path(const std::string& file, std::string* out) {
    if (file.empty()) return false;
    std::string n = file;
    while (!n.empty() && n.front() == '/') n.erase(n.begin());
    for (char& c : n)
        if (c == '\\') c = '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= n.size()) {
        const size_t slash = n.find('/', start);
        const std::string seg = n.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? n.size() + 1 : slash + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == ".." || seg.find(':') != std::string::npos) return false;
        parts.push_back(seg);
    }
    if (parts.empty()) return false;
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) joined += '/';
        joined += parts[i];
    }
    *out = joined;
    return true;
}
long long save_i64(const std::string& v, long long dflt) {
    if (v.empty()) return dflt;
    try {
        size_t used = 0;
        const long long r = std::stoll(v, &used);
        return used == v.size() ? r : dflt;
    } catch (...) {
        return dflt;
    }
}

} // namespace

std::string sanitize_savepath(const std::string& raw) { return sanitize_segments(raw); }

std::string GameRuntime::RuntimeState::qualify_save_file(const std::string& file) const {
    std::string clean;
    if (!clean_rel_path(file, &clean)) return std::string();
    if (save_.savepath_.empty() || clean == save_.savepath_ || clean.rfind(save_.savepath_ + "/", 0) == 0) {
        return clean;
    }
    return save_.savepath_ + "/" + clean;
}

bool GameRuntime::RuntimeState::save_file_exists(const std::string& file) const {
    const std::string path = qualify_save_file(file);
    if (path.empty() || !save_.save_store_) return false;
    return save_.save_store_->exists(path);
}

// ---------------------------------------------------------------------------
// System saves
//
// 名字归属（重构后）：
//   system.dat / saveg.dat       → 官方引擎（BOWX / BOWG）
//   conf.dat / global.dat        → 游戏（pluto）
//   system.oa.dat / saveg.oa.dat → 本引擎（g./s. 变量域的 OASB 文档）
//
// 读的一侧只保留一条**只读**迁移入口：没有自己的域文档时，把官方 BOWX/BOWG
// 导入到 g./s.（这是"官方存档能直接用"的读半边）。旧版本引擎留在
// system.dat / saveg.dat 上的 OASB 原样不动——见下面 OASB 分支。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 既読行（already read）—— g.readlines
//
// 形は「1 脚本 1 行」のテキスト表：
//     <script>\t<line>,<line>,...\n
// 原版の BOWG も同じ形（脚本 → 既読行の集合）なので、意味はそのまま。
// 規模は 1 万行でも 100KB 程度で、ドメイン文書の 1 変数に収まる。
// ---------------------------------------------------------------------------
namespace {

std::string encode_read_lines(
    const std::map<std::string, std::set<uint32_t>>& lines) {
    std::string out;
    for (const auto& [script, rows] : lines) {
        if (script.empty() || rows.empty()) continue;
        out += script;
        out += '\t';
        bool first = true;
        for (const uint32_t row : rows) {
            if (!first) out += ',';
            out += std::to_string(row);
            first = false;
        }
        out += '\n';
    }
    return out;
}

std::map<std::string, std::set<uint32_t>> decode_read_lines(const std::string& text) {
    std::map<std::string, std::set<uint32_t>> out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        const std::string script = line.substr(0, tab);
        auto& rows = out[script];
        size_t p = tab + 1;
        while (p < line.size()) {
            const size_t comma = line.find(',', p);
            const size_t stop = comma == std::string::npos ? line.size() : comma;
            if (stop > p) {
                try {
                    rows.insert(static_cast<uint32_t>(std::stoul(line.substr(p, stop - p))));
                } catch (const std::exception&) {
                    // 壊れた 1 行は捨てる（表全体を失うよりまし）
                }
            }
            if (comma == std::string::npos) break;
            p = comma + 1;
        }
    }
    return out;
}

} // namespace

void GameRuntime::RuntimeState::load_read_state() {
    if (!interpreter_) return;
    const auto v = interpreter_->variables().get("g.readlines");
    if (!v || v->kind != oa::runtime::ValueKind::String) return;
    interpreter_->set_read_lines(decode_read_lines(v->str_val));
    std::fprintf(stderr, "[runtime] read-state: %zu script(s) loaded\n",
                 interpreter_->read_lines().size());
}

void GameRuntime::RuntimeState::save_read_state() {
    if (!interpreter_) return;
    interpreter_->set_variable(
        "g.readlines",
        oa::runtime::Value::make_string(encode_read_lines(interpreter_->read_lines())));
}

void GameRuntime::RuntimeState::sysload() {
    if (!save_.save_store_) return;
    struct Slot {
        bool is_global;
        const char* oa_file;
        const char* legacy_file;
    };
    for (const Slot& slot : {Slot{true, kSavegOaFile, kSavegFile},
                             Slot{false, kSystemOaFile, kSystemFile}}) {
        const std::string oa_path = qualify_save_file(slot.oa_file);
        if (oa_path.empty()) continue;
        // ① 自己的域文档（权威）。
        if (const auto bytes = save_.save_store_->read(oa_path)) {
            try {
                const auto map = oa::runtime::decode_domain_map(
                    std::string(bytes->begin(), bytes->end()));
                const std::string prefix = slot.is_global ? "g." : "s.";
                for (const auto& [k, v] : map) interpreter_->set_variable(prefix + k, v);
                std::fprintf(stderr, "[runtime] sysload %s (%zu entries)\n",
                             oa_path.c_str(), map.size());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[runtime] sysload decode %s failed: %s\n",
                             oa_path.c_str(), e.what());
            }
            continue;
        }
        // ② 没有自己的文档：看一眼旧名字，只做**导入**，绝不回写。
        const std::string legacy = qualify_save_file(slot.legacy_file);
        if (legacy.empty()) continue;
        const auto bytes = save_.save_store_->read(legacy);
        if (!bytes) continue;
        const std::string doc(bytes->begin(), bytes->end());
        // 官方（原生引擎）的 BOW* 容器：另一个格式家族（runtime_save_official.h）。
        if (oa::runtime::is_official_save(doc)) {
            try {
                const auto off = oa::runtime::decode_official_save(doc);
                const auto stats = oa::runtime::import_official_variables(
                    off, [this](const std::string& name, oa::runtime::Value v) {
                        interpreter_->set_variable(name, v);
                    });
                std::fprintf(stderr,
                             "[runtime] sysload %s: official %s v%u -> %zu variable(s), "
                             "%zu host-path key(s) skipped, %zu Pluto table(s) converted\n",
                             legacy.c_str(), slot.is_global ? "BOWG" : "BOWX", off.version,
                             stats.applied, stats.skipped_host_path, stats.pluto_converted);
                if (stats.pluto_failed) {
                    std::fprintf(stderr,
                                 "[runtime] sysload %s: %zu official Pluto table(s) could not "
                                 "be converted (kept as raw bytes)\n",
                                 legacy.c_str(), stats.pluto_failed);
                }
                if (!off.read_lines.empty()) {
                    // 原版的既読表（BOWG 的 read_lines）直接并进本引擎的
                    // 已读表：官方存档里的"读过"进度不能丢。
                    interpreter_->merge_read_lines(off.read_lines);
                    std::fprintf(stderr,
                                 "[runtime] sysload %s: %zu script(s) / %zu read-line(s) "
                                 "merged into the read table\n",
                                 legacy.c_str(), off.read_lines.size(), stats.read_lines);
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[runtime] sysload official decode %s failed: %s\n",
                             legacy.c_str(), e.what());
            }
            continue;
        }
        // 旧版本引擎写的 OASB（占据着这个文件名）不去动它：OASB 容器同时是
        // 本引擎 `pluto.persist` 的载体，仅凭魔数**分不出**"引擎的域文档"和
        // "游戏自己写的 sys 表"——搬错一边就是删玩家数据。旧域文档不导入，
        // 下一次 syssave 会用 *.oa.dat 重新建立；游戏的文件原样留在原地。
        if (is_oasb_document(doc)) {
            std::fprintf(stderr,
                         "[runtime] sysload: %s is an OASB document from an older build; "
                         "left untouched (the engine now writes %s)\n",
                         legacy.c_str(), oa_path.c_str());
            continue;
        }
    }
}

bool GameRuntime::RuntimeState::syssave() {
    // 契约（重构后）：system.dat / saveg.dat 归游戏与官方引擎，本引擎一个字节都不写。
    //
    // 引擎只把自己的 g./s. 域写进 system.oa.dat / saveg.oa.dat。
    // 游戏自己的 sys 表 / gscr（conf.dat、global.dat）由游戏的 system_save() 写；
    // 数据里 `[system_save]` 总是排在 `[save]` 前面，所以这里**不要**再调一次
    // 游戏的落盘函数——那会重复弹通知、重复写盘。
    if (!save_.save_store_ || !interpreter_) return false;
    save_read_state(); // 既読表 → g.readlines（global 域として落ちる）
    const auto& vars = interpreter_->variables();
    const std::map<std::string, oa::runtime::Value> global(vars.global.begin(),
                                                          vars.global.end());
    const std::map<std::string, oa::runtime::Value> system(vars.system.begin(),
                                                          vars.system.end());
    bool all_ok = true;
    const auto write_map = [&](const char* file, const std::map<std::string,
                                                                oa::runtime::Value>& m) {
        const std::string path = qualify_save_file(file);
        if (path.empty()) return;
        // Strip the domain prefix off each key.
        std::map<std::string, oa::runtime::Value> stripped;
        for (const auto& [k, v] : m) {
            // s.status.* 是每帧刷新的瞬态（skip/avoid 标志），不进档：
            // 下次启动由 sync_control_status 重新摊平。
            if (k.rfind("s.status.", 0) == 0) continue;
            stripped[k.size() > 2 ? k.substr(2) : k] = v;
        }
        const std::string doc = oa::runtime::encode_domain_map(stripped);
        const std::vector<uint8_t> bytes(doc.begin(), doc.end());
        // 诊断：路径/条目数只在 OA_SAVE_TRACE 下打印（正常启动不刷屏）。
        static const bool trace = std::getenv("OA_SAVE_TRACE") != nullptr;
        if (trace) {
            std::fprintf(stderr, "[runtime] syssave %s (%zu entries, %zu bytes)\n",
                         path.c_str(), stripped.size(), bytes.size());
        }
        if (!save_.save_store_->write(path, bytes)) {
            if (trace) std::fprintf(stderr, "[runtime] syssave %s FAILED\n", path.c_str());
            all_ok = false;
        }
    };
    write_map(kSavegOaFile, global);
    write_map(kSystemOaFile, system);
    return all_ok;
}

// ---------------------------------------------------------------------------
// Numbered saves / loads
// ---------------------------------------------------------------------------

namespace {

/// 有界 tag 队列排空的轮数上限（= 原静默 guard 128）。
constexpr size_t kTagDrainGuard = 128;

/// 层 id 合法性（A 面只存名字；恢复必须容忍"读档后该 id 不
/// 存在"，但**畸形 id 不许进 ensure_path**——那条路会物化空名/错名节点）。
/// 判据来自 Compositor::ensure_path 的前缀切分：id 自身或任一前缀为空 =
/// 物化出空名节点。空 id / 前导 '.' / 空段（".."）都命中；尾点（"a."）是
/// 合法的嵌套 id（ensure_path 对它有一致的解释），不拒。
bool layer_id_ok(const std::string& id) {
    if (id.empty()) return false;
    if (id.front() == '.') return false;
    if (id.find("..") != std::string::npos) return false;
    return true;
}

/// 宿主供帧层旧存档的保留命名空间
/// 前缀。**仅用于自检记录**（跨版本兼容当前不做，见 runtime_internal.h 的
/// 版本门契约）：不迁移、不改写 props，只在日志里点出来。
bool legacy_reserved_file(const std::string& file) {
    return file.rfind("__video_layer__:", 0) == 0 ||
           file.rfind("__emote_layer__:", 0) == 0;
}

/// Flatten a message layer's page into the one string the engine can re-issue:
/// Text/RubyBase units keep their bytes, a Newline unit becomes '\n' (push_text
/// splits it back into the same unit sequence).
namespace {

// 框架的 CSV 按钮页（存档/读档/设置/回想）：根 id 是纯数字（500/600/700…），
// 子树里带 csvbtn 生成的 `name=` 点击行。这些页面是**打开着的操作界面**，
// 不属于剧情状态。存档时把它们的层一起存下来，读档后没人会关掉（框架的
// `restore` 只按 `scr.uifunc` 关 UI，而存档时它已经是 nil），于是整页按钮、
// 槽位编号和时间会盖在剧情画面上——用户报的"读档以后有文字残留"。
// 剧情层的根 id 一般带点（1.80 / 10.0），不会被这条规则误伤。
//
// **画面根 "1" 不是页面根**：Artemis 的整幅画面（背景/立绘/消息窗）都挂在
// 顶层 id "1" 下（框架自己就是 `[lydel id="1"]` 清画面，见 alldelete /
// adv_reset）。而 1.80 消息窗的按钮也是 csvbtn 生成的 `name=` 点击行，于是
// "1" 的子树里同样"有点击行带 name"——只按这条判据，整个 "1" 会被当成 CSV
// 页面，存档里的背景/立绘全部消失（用户报的"存档读档后背景丢失"）。
// 页面根只会是 400/500/600/700 这类旁挂的顶层数字 id，画面根永远是 "1"。
constexpr const char* kSceneRootId = "1";

bool is_csv_button_page_root(const oa::render::Compositor& scene, const std::string& id) {
    if (id.empty() || id.find('.') != std::string::npos) return false;
    if (id == kSceneRootId) return false;
    for (char c : id)
        if (c < '0' || c > '9') return false;
    const std::string prefix = id + ".";
    for (const oa::render::Layer* l : scene.draw_order()) {
        if (l->id.rfind(prefix, 0) != 0) continue;
        const auto it = l->event_handlers.find("click");
        if (it == l->event_handlers.end()) continue;
        // 两种按钮工厂的签名：
        //   name=<命令>        —— iMel/FPM 的 csvbtn
        //   com=<按钮名>       —— Artemis 系的 csvbtn2（save/load 页的
        //                          `com=bt_save01` / `com=sys_ret`）
        // 只认 name 的话，Artemis 的存档页会被当成剧情层整页存进存档，
        // 读档后存档界面的日期/槽位文字盖在画面上（test13）。
        if (it->second.params.count("name") || it->second.params.count("com")) return true;
    }
    return false;
}

// 子树里的任意一层是否落在"操作界面"根上。
bool under_csv_button_page(const oa::render::Compositor& scene, const std::string& id) {
    std::string cur = id;
    while (true) {
        if (is_csv_button_page_root(scene, cur)) return true;
        const auto dot = cur.rfind('.');
        if (dot == std::string::npos) return false;
        cur = cur.substr(0, dot);
    }
}

/// 存档瞬间的**临时 UI**：读档后由框架的收尾重新生成，不属于剧情画面。
/// 不剔掉它们，读档后会把"1番スロットをロードしました"、渐黑遮罩、存档页
/// 背景一起盖回剧情上。
bool is_transient_ui_id(const std::string& id) {
    const auto at = [&](const char* prefix) { return id.rfind(prefix, 0) == 0; };
    return id == "980" || at("980.") || id == "nowsaving" || at("loadmask") ||
           id == "99" || at("99.") || at("101.") || at("1000.") || at("80.") ||
           id == "110" || at("110.");
}

} // namespace

void collect_text_snapshot(oa::runtime::PageState* data, const oa::render::TextEngine& text,
                           const oa::render::Compositor& scene, bool skip_operation_pages = true) {
    data->has_text = true;
    const std::string active = text.active_layer_id();
    for (const auto& [id, mlp] : text.all_layers()) {
        const oa::render::MessageLayer& ml = *mlp;
        // 文本内容就存在节点上：节点没了 = 内容没了（[lydel] 之后不会有
        // 幽灵页）。
        if (scene.find(id) == nullptr) continue;
        // 操作界面（csvbtn 预建的 500.bt.N.bg.M 一类的槽位文字）不是画面
        // 上的东西：它们的排版参数是待用的，读档时 ensure_message_scene_node
        // 会把它们复活成可见 —— 那就是"读档后存档页的编号留在了剧情上"。
        if (skip_operation_pages &&
            (under_csv_button_page(scene, id) || is_transient_ui_id(id)))
            continue;
        std::string flat;
        for (const auto& u : ml.page) {
            if (u.kind == oa::render::PageUnit::Kind::Newline) flat.push_back('\n');
            else flat += u.data;
        }
        // 没有内容、又没有绘制盒的层（框架预建的槽位排版记录）不存。
        if (flat.empty() && !ml.positioned) continue;
        oa::runtime::TextLayerSnap s;
        s.id = id;
        s.active = (id == active);
        s.positioned = ml.positioned;
        s.layered = ml.layered ? 1 : 0;
        s.left = ml.left;
        s.top = ml.top;
        s.width = ml.width;
        s.height = ml.height;
        s.font = ml.font.raw;
        s.text = std::move(flat);
        s.revealed = ml.done;
        data->text_layers.push_back(std::move(s));
    }
}

/// Rebuild the message layers' pages and re-bind them to the restored scene.
/// The scene snapshot deliberately skips the engine message slots, so the
/// binding has to be re-made here exactly like the [chgmsg] handler does.
void restore_text_snapshot(oa::render::TextEngine* text, oa::render::Compositor* scene,
                           const oa::runtime::PageState& data) {
    if (!data.has_text || data.text_layers.empty()) return;
    text->clear_scene();
    std::string active;
    for (const auto& s : data.text_layers) {
        if (s.id.empty()) continue;
        std::map<std::string, std::string> font = s.font;
        if (s.positioned) {
            font["left"] = std::to_string(s.left);
            font["top"] = std::to_string(s.top);
            font["width"] = std::to_string(s.width);
            font["height"] = std::to_string(s.height);
        }
        text->switch_layer(s.id, /*stack=*/false,
                           s.layered >= 0 ? std::optional<int>(s.layered) : std::nullopt);
        if (!font.empty()) text->apply_font(font);
        if (!s.text.empty()) {
            text->push_text(s.text);
            // 恢复出来的这页是"存档时的样子"。框架的恢复流程随后会把当前页
            // 原样重打一遍（restoreText → mw_redraw → message_control），
            // push_text 是追加语义 —— 不拦一下就变成同一句出现两次。
            text->arm_reload_replace(s.id);
        }
        if (s.revealed) text->reveal_all();
        const oa::render::MessageLayer* ml = text->layer(s.id);
        const bool layered = ml && ml->layered;
        const std::string scene_id = scene->ensure_message_scene_node(s.id, layered);
        if (s.active) active = s.id;
    }
    if (!active.empty()) {
        text->switch_layer(active, /*stack=*/false, std::nullopt);
        scene->set_default_message_layer(active);
    }
}

void collect_scene_snapshot(oa::runtime::PageState* data, const oa::render::Compositor& scene,
                            bool skip_operation_pages = true) {
    data->has_scene = true;
    const oa::render::Layer& root = scene.root_props();
    data->root_props = root.props;
    for (const oa::render::Layer* l : scene.draw_order()) {
        // 打开中的操作界面不存。**剧情页快照例外**（skip=false）：剧情画面里
        // 的 dock（根 id `110`）也带 csvbtn 的 `com=` 点击行，按这条规则会被
        // 整棵剔掉 —— 读档后 dock 永远不出现，玩家进不去存档页（test10）。
        if (skip_operation_pages &&
            (under_csv_button_page(scene, l->id) || is_transient_ui_id(l->id)))
            continue;
        oa::runtime::LayerSnap snap;
        snap.id = l->id;
        snap.props = l->props;
        for (const auto& [type, row] : l->event_handlers) {
            oa::runtime::LayerEventHandlerSnap h;
            h.type = type;
            h.enabled = row.enabled;
            h.penetration = row.penetration;
            h.handler = row.handler;
            h.file = row.file;
            h.label = row.label;
            h.call = row.call;
            h.params = row.params;
            h.filter_params = row.filter_params;
            snap.handlers.push_back(std::move(h));
        }
        data->layers.push_back(std::move(snap));
    }
}

/// 回顾表：脚本的 `backlog.iet` 通过 `get_backlog_size` / `backlog_tags`
/// 重建回顾页。存档不带它，读档后回顾页就是空壳（用户报的"回顾没有字"）。
void collect_backlog_snapshot(oa::runtime::SaveData* data,
                              const oa::render::TextEngine& text,
                              bool include_current_page = false) {
    data->has_backlog = true;
    data->backlog_write_mode = text.backlog_write_mode();
    data->backlog_allow = text.backlog_allow();
    data->backlog.clear();
    data->backlog.reserve(text.backlog_pages().size());
    for (const auto& page : text.backlog_pages()) {
        oa::runtime::BacklogPageSnap bp;
        bp.page_font = page.page_font;
        bp.tags.reserve(page.tags.size());
        for (const auto& t : page.tags) {
            oa::runtime::BacklogTagSnap ts;
            ts.kind = int(t.kind);
            ts.text = t.text;
            ts.font = t.font;
            ts.params = t.params;
            bp.tags.push_back(std::move(ts));
        }
        data->backlog.push_back(std::move(bp));
    }
    // 当前这一页还没走 `[rp]`（存档/回顾跳转发生在页尾停驻），所以它不在
    // `backlog_pages()` 里。但"每页的回顾用档"必须包含玩家正看的这一页：
    // 读档后框架用 `get_backlog_size`/`backlog_tags` 重建回顾页，最后一页
    // 就是它；框架的 `scenario_reprint` 用 `[rp backlog=0]` 重画这一页，
    // 不会补一次入库。把当前页按 `[rp]` 入库时的同一形状（page_tags +
    // page_font）补在表尾，只有表和当前页不一致时才补。
    if (!include_current_page) return;
    const std::string active = text.active_layer_id();
    const oa::render::MessageLayer* ml = text.layer(active);
    if (ml && !ml->page_tags.empty()) {
        std::string cur;
        for (const auto& t : ml->page_tags) cur += t.text;
        std::string last;
        if (!data->backlog.empty())
            for (const auto& t : data->backlog.back().tags) last += t.text;
        if (!cur.empty() && cur != last) {
            oa::runtime::BacklogPageSnap bp;
            bp.page_font = ml->page_font;
            bp.tags.reserve(ml->page_tags.size());
            for (const auto& t : ml->page_tags) {
                oa::runtime::BacklogTagSnap ts;
                ts.kind = int(t.kind);
                ts.text = t.text;
                ts.font = t.font;
                ts.params = t.params;
                bp.tags.push_back(std::move(ts));
            }
            data->backlog.push_back(std::move(bp));
        }
    }
}

void restore_backlog_snapshot(oa::render::TextEngine* text,
                              const oa::runtime::SaveData& data) {
    if (!data.has_backlog) return;
    std::vector<oa::render::BacklogPage> pages;
    pages.reserve(data.backlog.size());
    for (const auto& bp : data.backlog) {
        oa::render::BacklogPage page;
        page.page_font = bp.page_font;
        page.tags.reserve(bp.tags.size());
        for (const auto& ts : bp.tags) {
            oa::render::BacklogTag t;
            t.kind = oa::render::BacklogTag::Kind(ts.kind);
            t.text = ts.text;
            t.font = ts.font;
            t.params = ts.params;
            page.tags.push_back(std::move(t));
        }
        pages.push_back(std::move(page));
    }
    text->set_backlog_pages(std::move(pages));
    text->set_backlog_flags(data.backlog_write_mode, data.backlog_allow);
}

void collect_audio_snapshot(oa::runtime::PageState* data, const oa::media::AudioEngine& audio) {
    data->has_audio = true;
    const auto& st = audio.state();
    const auto snap_channel = [](const oa::media::SoundChannel& ch) {
        oa::runtime::AudioChannelSnap s;
        s.id = ch.id;
        s.file = ch.file;
        s.loop_play = ch.loop_play;
        s.gain = ch.raw_gain;
        s.pan = ch.raw_pan;
        s.skippable = ch.skippable;
        return s;
    };
    if (st.bgm_channel && st.bgm_channel->playing) {
        data->audio.bgm = snap_channel(*st.bgm_channel);
    }
    for (const auto& [id, ch] : st.se_channels) {
        (void)id;
        if (ch.playing) data->audio.se.push_back(snap_channel(ch));
    }
    for (const auto& [id, ch] : st.voice_channels) {
        (void)id;
        if (ch.playing) data->audio.voice.push_back(snap_channel(ch));
    }
    std::sort(data->audio.se.begin(), data->audio.se.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(data->audio.voice.begin(), data->audio.voice.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
}

/// 场景快照恢复的显式处理统计（失效 id 跳过/重复 id 去重都必须
/// 有账可查；"缺失即物化"的入口 set_props 在这里一次都不用）。
struct SceneRestoreStats {
    size_t created = 0;
    size_t skipped_invalid = 0; // 畸形 id：不物化，跳过
    size_t skipped_dup = 0;     // 重复 id：首个生效，其余跳过（避免逐键合并）
};

SceneRestoreStats restore_scene_snapshot(oa::render::Compositor* scene,
                                        const oa::runtime::PageState& data,
                                        bool clear_scene = true,
                                        bool restore_handlers = true) {
    SceneRestoreStats st;
    if (!data.has_scene) return st;
    if (clear_scene) scene->clear_scene();
    if (!data.root_props.empty()) scene->set_root_props(data.root_props);
    std::set<std::string> seen;
    for (const auto& ls : data.layers) {
        // A 面数据里的 id 只当数据看：畸形 → 显式跳过 + 报告（不 ensure_path）。
        if (!layer_id_ok(ls.id)) {
            ++st.skipped_invalid;
            std::fprintf(stderr,
                         "[runtime] load: layer snapshot id '%s' is malformed; "
                         "skipped (no node materialized)\n",
                         ls.id.c_str());
            continue;
        }
        // 重复 id：collect_scene_snapshot 不会产出（draw_order 唯一），手写/损坏
        // 档可能有 ⇒ 首个生效（确定性），其余跳过并报告。
        if (!seen.insert(ls.id).second) {
            ++st.skipped_dup;
            std::fprintf(stderr,
                         "[runtime] load: duplicate layer snapshot id '%s'; "
                         "first occurrence wins, later one skipped\n",
                         ls.id.c_str());
            continue;
        }
        if (scene->find(ls.id)) {
            // merge 模式（onLoad 之后）：保留队列 tag/框架新建的层，只把
            // 快照里的属性盖回去（消息窗边框、已读图标、dock 按钮状态）。
            //
            // **显隐不盖**：快照是在存档/读档界面里取的，那一刻 UI 会把
            // 消息窗容器（1.80）显式隐藏（msg_hide），而框架的 onLoad 会
            // 按页面状态重建窗口并把它显回来。把快照里的 visible=0 盖回去
            // 就等于读档后整条消息窗/按钮栏永久消失（t13 的 auto/skip/
            // 紧急回避全部点不到）。显隐以 onLoad 重建的结果为准。
            std::map<std::string, std::string> props = ls.props;
            props.erase("visible");
            scene->set_props(ls.id, props);
        } else {
            scene->create(ls.id, ls.props);
        }
        ++st.created;
        if (!restore_handlers) continue;
        for (const auto& h : ls.handlers) {
            oa::render::LayerEventHandler row;
            row.enabled = h.enabled;
            row.penetration = h.penetration;
            row.handler = h.handler;
            row.file = h.file;
            row.label = h.label;
            row.call = h.call;
            row.params = h.params;
            row.filter_params = h.filter_params;
            scene->restore_event_handler_row(ls.id, h.type, std::move(row));
        }
    }
    return st;
}

void restore_audio_snapshot(oa::media::AudioEngine* audio, const oa::runtime::AudioSnap& snap) {
    audio->stop_all_sounds();
    if (snap.bgm) {
        oa::media::BgmConfig cfg;
        cfg.loop_play = snap.bgm->loop_play;
        cfg.gain = snap.bgm->gain;
        cfg.pan = snap.bgm->pan;
        audio->play_bgm(snap.bgm->file, cfg);
        if (snap.bgm->loop_play) {
            if (auto loop_file = oa::media::ab_loop_file(snap.bgm->file)) {
                if (audio->state().bgm_channel) {
                    audio->state().bgm_channel->loop_file = std::move(loop_file);
                }
            }
        }
    }
    for (const auto& se : snap.se) {
        oa::media::SeConfig cfg;
        cfg.loop_play = se.loop_play;
        cfg.gain = se.gain;
        cfg.pan = se.pan;
        cfg.skippable = se.skippable;
        audio->play_se(se.id, se.file, cfg);
    }
    for (const auto& v : snap.voice) {
        oa::media::SeConfig cfg;
        cfg.loop_play = v.loop_play;
        cfg.gain = v.gain;
        cfg.pan = v.pan;
        cfg.skippable = v.skippable;
        audio->play_voice(v.id, v.file, cfg);
    }
}

} // namespace

bool GameRuntime::RuntimeState::save_game_to(const std::string& file,
                                            const oa::runtime::SaveOrigin* origin) {
    if (!save_.save_store_ || !interpreter_) return false;

    // 1) 框架的 onSave（iMel `setOnSave` / FPM·t13 `store`）先跑：这是框架
    //    **声明自己还要写什么**的唯一入口（iMel 写 g.latestsave、FPM 把
    //    lua.scr/qjb/log 落进变量）。它排下的 tag 留在队列里，等这一条
    //    `[save]` 回到脚本流后按原顺序执行 —— 不能在这里 drain（会在
    //    run() 内部重入解释器）。
    std::vector<oa::runtime::Instruction> pending = interpreter_->take_tag_queue();
    // onSave 写进 local 域的变量（FPM 的 lua.scr/log/qjb、iMel 的 g.latestsave）
    // 是**存档内容本身**，必须随档落盘。UI 存档下面会用页锚点变量域做底
    // （避免存档页/dialog 的 scratch 混进来），所以先记下 handler 这一拍
    // 新增/改写的键，随后覆盖到最终变量表上。
    const std::map<std::string, oa::runtime::Value> vars_before_on_save =
        interpreter_->variables().local;
    (void)interpreter_->fire_event("onSave", {{"file", file}});
    std::map<std::string, oa::runtime::Value> on_save_vars;
    for (const auto& [k, v] : interpreter_->variables().local) {
        const auto prev = vars_before_on_save.find(k);
        if (prev == vars_before_on_save.end() || !(prev->second == v))
            on_save_vars.emplace(k, v);
    }
    interpreter_->restore_tag_queue(std::move(pending));

    oa::runtime::SaveData data;
    // 2) 位置。规则只有一条：**记"从哪条脚本指令重放能回到玩家看到的这一页"**。
    //
    //    a. 剧情页落定（park 活着 / 回顾跳转载体 / 框架 UI 存档）：
    //       恢复点 = 这一页的**脚本起点**（页锚点：script/line/call_stack）。
    //       读档从页首重放这一页，停在页尾的 `[@]`；回顾快进也在这一页的
    //       `backlogjump_countup` 命中目标索引。用停驻行（页尾之后）会让
    //       快进多走一页。
    //    b. `[save]` 在框架脚本里但不是页面存档（save.iet 的
    //       「ロード時はここから再開」）：恢复点 = 执行现场（内联 tag 是
    //       line+1），读档后框架自己的收尾接着跑。
    //    c. 其它（宿主自发的精确存档）：当前脚本位置。
    //
    //    `backlog_N.dat` 是框架"每页一份"的回顾载体；`save_N.dat` 的
    //    `[save]` 发生在 save.iet（自己的 UI 流程）里，读档后要按**页面**
    //    重画窗口，所以两者都走 a。
    const bool backlog_carrier = file.find("backlog_") != std::string::npos;
    const bool mel_ui_save =
        origin && oa::runtime::is_framework_script(origin->script) && !backlog_carrier;
    const bool story_park_save =
        interpreter_->page_anchor_valid() &&
        !oa::runtime::is_framework_script(interpreter_->page_anchor_script());
    if (story_park_save) {
        // UI 存档还要在 load 之后重画窗口（见 page_anchor 标记）；回顾载体
        // 由 skipstart 自己接管，不重复。
        data.page_anchor = !backlog_carrier;
        data.current_script = interpreter_->page_anchor_script();
        data.current_line = interpreter_->page_anchor_line();
        for (const auto& f : interpreter_->page_anchor_stack())
            data.call_stack.emplace_back(f.script, f.return_line);
    } else if (origin && !origin->script.empty()) {
        data.current_script = origin->script;
        data.current_line = origin->line;
        data.call_stack = origin->call_stack;
        if (oa::runtime::is_framework_script(data.current_script))
            data.story_call_line = origin->story_call_line;
    } else if (const std::string* cur = interpreter_->current_script()) {
        data.current_script = *cur;
        data.current_line = interpreter_->current_line();
        for (const auto& f : interpreter_->call_stack())
            data.call_stack.emplace_back(f.script, f.return_line);
    }
    // 3) 变量：**只有无前缀 local**。g./s. 各有自己的文档（saveg/system），
    //    t. 是 scratch —— 官方 BOWS 不存它。存了它，t10 读档后 t.saveslot
    //    非 0，save.iet 会走"保存成功"分支而不是"刚读完档"分支。
    //
    //    框架 UI 存档（save.iet 里的 `[save]`）取**页锚点落定时的变量域**：
    //    此刻的 local 表混着存档页/对话框的 scratch（`meswindow.num` 会变成
    //    存档页的 MW04，r_voice/mouth_* 停在 UI 上），而读档后框架按这些
    //    页面变量决定重画哪套窗口。页面档（backlog 载体/宿主停驻存档）本来
    //    就在页面上，直接用当前域。
    const bool page_vars_ok = interpreter_->page_anchor_valid() &&
                              !oa::runtime::is_framework_script(
                                  interpreter_->page_anchor_script());
    if (mel_ui_save && page_vars_ok) {
        data.local_variables = interpreter_->page_start_vars();
        // 页锚点变量域是"这一页开始时的状态"；onSave 刚写进去的载荷
        // （lua.scr/log/qjb 等）属于本次存档，不能被页变量盖掉。
        for (const auto& [k, v] : on_save_vars) data.local_variables[k] = v;
    } else {
        data.local_variables = interpreter_->variables().local;
    }
    // 这一页的消息窗 id 是页面级配置（`[adv mw="..."]`）。框架读档后用它
    // 选 sysbtn/窗口图片目录；存档页会临时改写 `meswindow.num`，所以单独
    // 记一份页面值（不是画面快照，只是一条页面变量）。旧档没有 → 空。
    if (page_vars_ok) {
        const auto& pv = interpreter_->page_start_vars();
        const auto it = pv.find("meswindow.num");
        if (it != pv.end()) data.page_meswindow = it->second.to_display();
    }
    // 4) 画面/文本/音频 = 官方 BOWS 的字段 26/24/25/29（存档格式的一部分，
    //    读档时原样交还；不是"页快照"）。
    collect_scene_snapshot(&data, scene(), /*skip_operation_pages=*/true);
    collect_text_snapshot(&data, text(), scene(), /*skip_operation_pages=*/true);
    collect_audio_snapshot(&data, media_.audio_);
    // 回顾跳转载体是"每页一份"：它必须把玩家正看的这一页也算进表尾。
    // 普通存档沿用框架已经归档的回顾表（多补一份会让读档后的表多一页）。
    collect_backlog_snapshot(&data, text(), backlog_carrier);

    if (std::getenv("OA_SAVE_TRACE")) {
        std::fprintf(stderr,
                     "[runtime] save data: scene=%d layers=%zu text=%d textlayers=%zu "
                     "vars=%zu @ %s:%zu\n",
                     int(data.has_scene), data.layers.size(), int(data.has_text),
                     data.text_layers.size(), data.local_variables.size(),
                     data.current_script.c_str(), data.current_line);
        std::fprintf(stderr, "[runtime] story_call_line=%zu\n", data.story_call_line);
        if (const auto* w = current_wait())
            std::fprintf(stderr, "[runtime] save wait owner=%s:%zu click=%d\n",
                         w->owner_script.c_str(), w->owner_line, int(w->waiting_click()));
        for (size_t i = 0; i < park_stack_.size(); ++i) {
            const auto& pc = park_stack_[i];
            if (!pc.wait) continue;
            std::fprintf(stderr, "[runtime] save park_stack[%zu] owner=%s:%zu click=%d\n", i,
                         pc.wait->owner_script.c_str(), pc.wait->owner_line,
                         int(pc.wait->waiting_click()));
        }
        std::fprintf(stderr, "[runtime] save stable=%s:%zu\n",
                     interpreter_->stable_script().c_str(), interpreter_->stable_line());
        if (const oa::runtime::Script* sc = interpreter_->get_script(data.current_script)) {
            if (data.current_line < sc->instructions.size()) {
                const auto& ins = sc->instructions[data.current_line];
                std::fprintf(stderr, "[runtime] save resume instruction: src=%zu tag=%s\n",
                             ins.line, ins.tag.c_str());
            }
        }
        for (const auto& [fs, fl] : data.call_stack)
            std::fprintf(stderr, "[runtime] save frame: %s:%zu\n", fs.c_str(), fl);
    }
    const std::string doc = data.encode();
    const std::string path = qualify_save_file(file);
    if (path.empty()) {
        std::fprintf(stderr, "[runtime] save: illegal file name '%s'\n", file.c_str());
        return false;
    }
    if (!save_.save_store_->write(path, std::vector<uint8_t>(doc.begin(), doc.end()))) {
        std::fprintf(stderr, "[runtime] save: write failed: %s\n", path.c_str());
        return false;
    }
    std::fprintf(stderr, "[runtime] saved %s (%zu local vars, script %s:%zu)\n",
                 path.c_str(), data.local_variables.size(), data.current_script.c_str(),
                 data.current_line);
    return true;
}

// Load hardening: clear list / atomic failure / self-check / bounded drain.

size_t GameRuntime::RuntimeState::reset_ephemeral_on_load() {
    // [C] ephemeral clear list. Must stay aligned with reset_domains();
    // scene clears before the B-preserve input registry write-back.
    // Keep: interpreter tag queue, host pending_events_, script config rows.
    size_t cleared = 0;
    const auto clr = [&cleared] { ++cleared; };

    // ---- 媒体面（资源态：播放器/解码通道/宿主供帧绑定）----
    media_.audio_.stop_all_sounds(); clr();
    if (media_.media_players_) { media_.media_players_->stop_all(); clr(); }
    media_.video_.stop_all_videos(); clr();
    media_.emote_layers_.clear(); clr();
    media_.emote_view_cache_.clear();
    media_.pending_emote_methods_.clear(); clr();

    // ---- 文本 / 场景容器 ----
    text().clear_scene(); clr();
    scene().clear_scene(); clr();
    drop_pending_tween_cancels(); clr();
    transition().clear(); clr();
    pointer_.hovered_.clear(); pointer_.drag_layer_.clear(); clr();
    reset_park_contexts(); clr();
    save_.screenshot_.valid = false; clr();
    renderer_.stop_all_delete();
    park_.wait.reset(); clr();
    park_.tail.clear(); clr();
    park_.pending.clear(); clr();
    park_.pending_custom.clear(); clr();
    park_.pending_sync_tweens.clear(); clr();
    save_.autosaved_current_wait_ = false;
    save_.autosave_last_seq_ = 0;

    // ---- 控制域（模式闩；配置面 [* allow] 属 ② 保留，不在此）----
    control_.skip_active_ = false; clr();
    control_.skip_enabled_ = false; clr();
    // exskip only drops the flag: end_debug_skip() would replay a quickjump
    // and overwrite the restored position.
    control_.exskip_active_ = false; clr();
    control_.control_skip_pressed_ = false; clr();
    control_.control_skip_blocked_ = false; clr();
    control_.last_control_skip_effective_ = false; clr();
    control_.automode_ = false; clr();
    control_.automode_syncse_.clear(); clr();
    control_.hide_active_ = false; clr();
    control_.hide_snapshot_.clear(); clr();
    control_.script_status_ = 0; clr();
    control_.active_wait_icon_.clear(); clr();

    // ---- 指针 / 按键瞬时态（[C]；每 tick 由 FrameInput 重算的派生量）----
    input_state_.overrides_.clear(); input_state_.prev_overrides_.clear(); clr();
    input_state_.keys_down_.clear(); input_state_.down_edges_.clear(); input_state_.up_edges_.clear(); clr();
    input_state_.decide_edge_ = false; clr();

    // Keep: script config rows, physical pointer mirror, exit latch,
    // host pending_events_, interpreter tag queue (see docs/RUNTIME_STATE.md).
    return cleared;
}

bool GameRuntime::RuntimeState::preflight_load_position(const oa::runtime::SaveData& data,
                                                       std::string* why) {
    if (!interpreter_) {
        if (why) *why = "no interpreter";
        return false;
    }
    // 空 current_script = "未定位"（合法：只有变量/场景的档）。
    if (data.current_script.empty()) return true;
    std::string err;
    if (!interpreter_->try_load_script(data.current_script, &err)) {
        if (why) *why = "script '" + data.current_script + "' unavailable: " + err;
        return false;
    }
    const oa::runtime::Script* s = interpreter_->get_script(data.current_script);
    if (s && data.current_line >= s->instructions.size()) {
        // Out-of-range line = script-completed (defined); report only.
        std::fprintf(stderr,
                     "[runtime] load: preflight: current_line %zu is beyond script '%s' "
                     "end (%zu instructions); the position resolves to script-completed\n",
                     data.current_line, data.current_script.c_str(), s->instructions.size());
    }
    return true;
}

size_t GameRuntime::RuntimeState::self_check_save_data(const oa::runtime::SaveData& data) const {
    // Log-only tolerance for bad A-side data; returns the issue count.
    size_t issues = 0;
    std::set<std::string> ids;
    for (const auto& ls : data.layers) {
        if (!layer_id_ok(ls.id)) {
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: layer id '%s' is malformed "
                         "(empty id / leading '.' / empty path segment); it will be "
                         "skipped, no node materialized\n",
                         ls.id.c_str());
            continue;
        }
        if (!ids.insert(ls.id).second) {
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: duplicate layer id '%s'; the first "
                         "occurrence wins\n",
                         ls.id.c_str());
        }
        const auto fit = ls.props.find("file");
        if (fit != ls.props.end() && legacy_reserved_file(fit->second)) {
            // 旧存档形态（宿主供帧层的保留命名空间）。跨版本兼容不做
            // ⇒ **只记录不改写**（不迁移、不剥前缀）。
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: legacy reserved file prop on layer "
                         "'%s' (legacy save form); kept verbatim — cross-version "
                         "migration is out of scope\n",
                         ls.id.c_str());
        }
    }
    if (!data.has_scene) {
        ++issues;
        std::fprintf(stderr,
                     "[runtime] load: self-check: save has no scene snapshot; the scene "
                     "is cleared and only script/Lua state is restored\n");
    } else if (data.layers.empty()) {
        ++issues;
        std::fprintf(stderr,
                     "[runtime] load: self-check: empty scene snapshot (0 layers)\n");
    }
    if (data.has_audio && data.audio.empty()) {
        ++issues;
        std::fprintf(stderr, "[runtime] load: self-check: empty audio snapshot\n");
    }
    if (data.current_script.empty() && !data.call_stack.empty()) {
        ++issues;
        std::fprintf(stderr,
                     "[runtime] load: self-check: %zu call-stack frame(s) with an empty "
                     "current script\n",
                     data.call_stack.size());
    }
    for (const auto& [script, line] : data.call_stack) {
        (void)line;
        if (script.empty()) {
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: call-stack frame with an empty "
                         "script name (frame is kept verbatim)\n");
            break;
        }
    }
    const auto check_ch = [&issues](const oa::runtime::AudioChannelSnap& c, const char* kind) {
        if (c.file.empty()) {
            ++issues;
            std::fprintf(stderr,
                         "[runtime] load: self-check: %s channel '%s' has an empty file; "
                         "replay will be a no-op\n",
                         kind, c.id.c_str());
        }
    };
    if (data.has_audio) {
        if (data.audio.bgm) check_ch(*data.audio.bgm, "bgm");
        for (const auto& c : data.audio.se) check_ch(c, "se");
        for (const auto& c : data.audio.voice) check_ch(c, "voice");
    }
    return issues;
}

size_t GameRuntime::RuntimeState::drain_tag_queue_bounded(const char* phase) {
    size_t rounds = 0;
    while (interpreter_->has_queued_tags() && rounds < kTagDrainGuard) {
        (void)interpreter_->run_queued();
        ++rounds;
    }
    if (interpreter_->has_queued_tags()) {
        // 旧实现是 `guard++ < 128` 的静默截断：超限时剩下的 tag 直接留到下一
        // tick（甚至被后续清场带走）。现在截断**可观测**（日志 + 计数）。
        ++tag_drain_truncations_;
        std::fprintf(stderr,
                     "[runtime] %s: tag queue drain hit the %zu-round guard; %zu tag(s) "
                     "still queued (they run on the next drain round)\n",
                     phase, kTagDrainGuard, interpreter_->queued_tag_count());
    }
    return rounds;
}

LoadResult GameRuntime::RuntimeState::load_game(const std::string& file, int64_t trans_type) {
    // 失败语义：**原子**。位置预检在任何清场之前完成；任一失败
    // ⇒ 运行时状态不被改动，调用方按 LoadResult 分支（不再"只打日志"）。
    if (!save_.save_store_ || !interpreter_) {
        std::fprintf(stderr, "[runtime] load: no save store / interpreter; load unavailable\n");
        return LoadResult::MissingFile;
    }
    const std::string path = qualify_save_file(file);
    const auto bytes = path.empty() ? std::optional<std::vector<uint8_t>>() : save_.save_store_->read(path);
    if (!bytes) {
        std::fprintf(stderr, "[runtime] load: missing save file: %s\n", path.c_str());
        return LoadResult::MissingFile;
    }
    oa::runtime::SaveData data;
    try {
        data = oa::runtime::SaveData::decode(std::string(bytes->begin(), bytes->end()));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[runtime] load: corrupt save %s: %s\n", path.c_str(), e.what());
        return LoadResult::Corrupt;
    }
    // ① A 面自检（只 log 不 throw）。
    load_self_check_issues_ += self_check_save_data(data);
    // 位置预检：**必须先于任何清场**（否则 = 场景已清、快照已灌、位置失败的
    // 半恢复）。
    {
        std::string why;
        if (!preflight_load_position(data, &why)) {
            std::fprintf(stderr,
                         "[runtime] load: position preflight failed (%s); runtime state "
                         "unchanged\n",
                         why.c_str());
            return LoadResult::PositionUnavailable;
        }
    }

    // ② 桶特判保留的现场：全局输入注册表必须在清场
    // 之前取，场景快照恢复之后回写（顺序契约 1）。诊断：在飞动画（冻结语义，
    // 见文件头"在飞动画契约"）在清场里连同节点消亡，只记不重放。
    const auto live_input = scene().input_handlers();
    const bool had_tweens = scene().has_tweens();
    const bool had_anime = scene().has_anime();

    // [C] 桶读档清场（逐项见 reset_ephemeral_on_load）。
    const size_t cleared = reset_ephemeral_on_load();
    // 回顾表不属于"打开着的 UI"，是读过的剧情行：整份还原（含
    // writebacklog 开关，否则读档后新页不再入表，回顾页永远停在旧内容）。
    restore_backlog_snapshot(&text(), data);
    // 场景先按快照装一次：② 桶保留的、读档前排下的 tag（FPM 的 estag 链）
    // 会在这之后立刻执行，它们必须能看到恢复后的场景（save_compat S7）。
    // onLoad 之后还会再装一次（框架会重建消息窗/dock）。
    // ASB/Lua 框架的 bar/按钮 handler 必须由 onLoad 重新注册；快照里的
    // 旧 handler 绑定的是已经结束的 Lua 会话（t13 auto/skip 点不动）。
    // iMel（system/msg.iet）的按钮 handler 是脚本行，恢复它可以让场景立即可用；
    // ASB/Lua 框架的 handler 绑定已经结束的 Lua 会话，必须由 onLoad 重新注册。
    const bool mel_pages = interpreter_->get_script("system/msg.iet") != nullptr ||
                           interpreter_->get_script("system/config.iet") != nullptr;
    SceneRestoreStats rst = restore_scene_snapshot(
        &scene(), data, /*clear_scene=*/true, /*restore_handlers=*/mel_pages);
    for (const auto& [k, row] : live_input) {
        scene().restore_input_row(k.first, k.second, row);
    }

    // 变量：只替换 local 域；g./s. 是当前会话的系统状态，t. 是框架在
    // `[load]` 之前刚设好的 scratch（t.autoskip_mute / t.se_load …），
    // 都不能被存档覆盖。旧档里混进来的 t.* 在读侧已经过滤。
    interpreter_->variables().local.clear();
    for (const auto& [k, v] : data.local_variables) {
        interpreter_->set_variable(k, v);
    }
    // `s.current_message_layer` 属于 system 域（不进存档），但它是框架
    // **本轮 load 收尾的分支依据**：onLoad 的 chgmsg/sysbtn、backlogjump_skipstart
    // 的 `$s.current_message_layer == 'adv'` 决定重画哪一套窗口。用存档里
    // 消息层的 active 标记先把它归位，再让 tag 队列跑 —— 否则队列先跑、
    // 变量后补，框架会认为当前是别的窗口，整段重画被跳过（回顾跳转后
    // 消息窗框/名字栏消失，只剩一行字）。
    const bool imel_pages = interpreter_->get_script("system/msg.iet") != nullptr;
    if (imel_pages)
        for (const auto& ts : data.text_layers) {
            if (!ts.active || ts.id.empty()) continue;
            interpreter_->set_variable("s.current_message_layer",
                                       oa::runtime::Value::make_string(ts.id));
            break;
        }
    // 页面级消息窗 id（页面变量；旧档没有就保留现场值）。框架的 sysbtn
    // 目录/窗口重画在 onLoad 的 tag 队列里就要用到它。
    if (imel_pages && !data.page_meswindow.empty())
        interpreter_->set_variable("meswindow.num",
                                   oa::runtime::Value::make_string(data.page_meswindow));
    // `mwcheck` 是框架"这一页有没有消息窗"的页面标记（menu leave 用它决定
    // 重建 sysbtn 还是隐藏）。它写在菜单/宏的局部作用域里，平铺变量域在
    // `[save]` 时看不到；读档时按存档里的消息层内容恢复：有可见的正文页
    // ⇒ 这一页的窗口是显示的。
    std::vector<oa::runtime::CallFrame> stack;
    for (const auto& [script, line] : data.call_stack) {
        oa::runtime::CallFrame f;
        f.script = script;
        f.return_line = line;
        stack.push_back(std::move(f));
    }
    // 官方 BOWS 的 frame.current：存档发生在框架脚本里时，剧情帧的
    // return_line 指向"这一页之后"；回到帧的 call_line（宏调用行）才会
    // 把这一页重走一遍并停在 [@]。
    if (data.story_call_line > 0) {
        for (size_t i = stack.size(); i-- > 0;) {
            if (!stack[i].script.empty() && !oa::runtime::is_framework_script(stack[i].script)) {
                std::fprintf(stderr,
                             "[runtime] load: story frame %s return_line %zu -> %zu (call_line)\n",
                             stack[i].script.c_str(), stack[i].return_line,
                             data.story_call_line);
                // 帧对外报的位置 = frame.current（call_line）；重放点也用
                // call_line，返回点则保持真实值 —— 两者不能混。
                stack[i].call_line = data.story_call_line;
                stack[i].return_line = data.story_call_line;
                break;
            }
        }
    }
    try {
        interpreter_->restore_position(data.current_script, data.current_line, stack);
    } catch (const std::exception& e) {        // 预检已保证脚本可载入 ⇒ 这里只在引擎内部不一致（如内存故障）时到达。
        // 不静默：明确报错 + 返回可判定的失败码（此时状态 = 已清场并已按快照
        // 恢复，没有第二个可回滚的现场，文档里逐字写明）。
        std::fprintf(stderr,
                     "[runtime] load: position restore failed AFTER a successful preflight "
                     "(%s); scene is the restored snapshot but the position is unset\n",
                     e.what());
        return LoadResult::PositionUnavailable;
    }
    static const bool loaddbg = std::getenv("OA_LOADDBG") != nullptr;
    if (loaddbg) {
        std::fprintf(stderr, "[loaddbg] after restore_position: %s:%zu queued=%zu\n",
                     interpreter_->current_script() ? interpreter_->current_script()->c_str() : "",
                     interpreter_->current_line(), interpreter_->queued_tag_count());
    }

    // onLoad (restore): unpack restored variables back into the Lua tables.
    // FPM handler `restore(e, p)` reads p.file too (fileio.lua:12) — same
    // calllua shape as onSave.
    // 丢掉上一次会话残留的 estag 队列（`scr.est`）：它是框架"正在进行的
    // UI/动画流程"的执行栈，不属于存档内容。留着它，读档后玩家第一次点击就会
    // 让 estag_call 弹出旧的 dialog_return / uitrans —— 而 restore 里的
    // adv_flagreset() 已经把 flg 清成 {}，于是一头撞上 `flg.dlg == nil`
    // （test2: estag_call ... attempt to index local 'dlgp' (a nil value)），
    // 异常吞掉这一拍，表现就是"点了不动"。放在 restore 之前清，让框架自己
    // 重建需要的那一层。
    try {
        interpreter_->lua_bridge().run_code(
            "if type(scr) == \"table\" then scr.est = nil end\n", "load_drop_estag");
    } catch (const std::exception&) {
        // 没有 scr 的框架（iMel）本来就没事。
    }
    // 框架注册的 onLoad（iMel setOnLoad / FPM·t13 restore）。名字由
    // e:setEventHandler 声明，引擎不硬编码 `restore`。
    {
        const bool fired = interpreter_->fire_event("onLoad", {{"file", file}});
        if (std::getenv("OA_LOADDBG"))
            std::fprintf(stderr, "[loaddbg] onLoad handler fired=%d\n", int(fired));
    }
    // tag 队列不清（② 特判保留）——这里排空的是"读档链自己排的" tag，
    // 顺序契约 2：Lua onLoad → 队列排空 → 引擎音频快照。有界排空可观测。
    (void)drain_tag_queue_bounded("load");
    if (std::getenv("OA_LOADDBG")) {
        try {
            interpreter_->lua_bridge().run_code(
                "if type(scr) ~= 'table' then error('scr is '..type(scr)) end\n"
                "if type(scr.adv) ~= 'table' then error('scr.adv is '..type(scr.adv)) end\n",
                "oa_dbg_scr");
            std::fprintf(stderr, "[loaddbg] scr.adv ok\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[loaddbg] %s\n", e.what());
        }
    }
    // Reload sys/gscr/conf Lua tables from the (restored) g.* stash vars.
    try {
        interpreter_->lua_bridge().run_code(
            "if type(fload_pluto) == \"function\" and type(init) == \"table\" then\n"
            "  if init.save_system then sys = fload_pluto(init.save_system) or sys or {} end\n"
            "  if init.save_global then gscr = fload_pluto(init.save_global) or gscr or {} end\n"
            "  if init.save_config then conf = fload_pluto(init.save_config) or conf or {} end\n"
            "end\n",
            "reload_persistent");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[runtime] load: persistent tables reload skipped: %s\n",
                     e.what());
    }

    // The framework's own onLoad (`restore`) rebuilds the message window from
    // the saved Lua tables — set_message_window2() resets the layers, so a text
    // snapshot written before it would be wiped (test13: the loaded page had
    // the cursor on it but the message window was empty). Apply the snapshot
    // after the handler and its queued tags have run: the page the reader saw
    // is the one that has to come back.
    // 场景也一起延后安装（onLoad 会重建消息窗/ dock），先装场景再装文本。
    if (mel_pages) {
        // iMel：onLoad 会重建消息窗/dock，场景快照要再盖一次（只盖属性，
        // handler 用 onLoad 新注册的）。
        rst = restore_scene_snapshot(&scene(), data, /*clear_scene=*/false,
                                     /*restore_handlers=*/false);
        for (const auto& [k, row] : live_input) {
            scene().restore_input_row(k.first, k.second, row);
        }
        renderer_.request_repaint();
    }
    // ASB/Lua 框架：保持旧顺序（场景只在 onLoad 前装一次），它们的 bar/按钮
    // handler 依赖 onLoad 自己的注册时机。
    // 文本面 = 存档格式里的消息层数据（BOWS 24/25）：框架的 `scenario_reprint`
    // 读 `get_message_tags`，引擎只交还数据，不贴任何排版结果。
    restore_text_snapshot(&text(), &scene(), data);
    // `s.current_message_layer` 是 system 域（槽位存档不存 system），但它是
    // **页状态**：读档前它停在 ly dialog 的 `1000.50`（槽位确认框），不修正
    // 的话框架之后所有 `$s.current_message_layer == 'adv'` 分支都走错——
    // 消息窗/名字栏不再重画，菜单里的 msg 也打到对话框层上。
    if (const std::string restored_active = text().active_layer_id(); !restored_active.empty()) {
        interpreter_->set_variable("s.current_message_layer",
                                   oa::runtime::Value::make_string(restored_active));
    }
    if (data.page_anchor && interpreter_->get_script("system/msg.iet") != nullptr &&
        interpreter_->get_script("system/sysbtn.iet") != nullptr) {
        // 框架的 load 收尾：位置已回到这一页的起点，但存档页/对话框打开时
        // 折掉的窗口状态（dock、消息窗框/名字栏）要按恢复后的变量重画。
        // 这些都是**框架脚本自己的重建入口**，引擎只把它们排进队列，
        // 不往场景里塞任何层。
        auto enqueue = [&](std::string tag, std::map<std::string, std::string> p = {}) {
            interpreter_->enqueue_tag(std::move(tag), std::move(p));
        };
        for (const char* n : {"t.sysbtn_submenu", "sysbtn_mode", "sysbtn_exist"})
            enqueue("var", {{"name", n}, {"system", "delete"}});
        for (const char* id : {"loadmask", "980", "90"})
            enqueue("lydel", {{"id", id}});
        enqueue("call", {{"file", "system/msg.iet"}, {"label", "adv_mwredraw"}});
        enqueue("call", {{"file", "system/msg.iet"}, {"label", "adv_namebar_redraw"}});
        enqueue("call", {{"file", "system/sysbtn.iet"}, {"label", "sysbtn_init"}});
        enqueue("call", {{"file", "system/sysbtn.iet"}, {"label", "sysbtn_set"}});
    }
    renderer_.request_repaint();

    if (data.has_audio) restore_audio_snapshot(&media_.audio_, data.audio);
    sync_system_audio_volumes();

    // [load type=..] runs one transition after the restore (type 0 instant).
    if (trans_type == 1 || trans_type == 2) {
        std::map<std::string, std::string> params;
        params["type"] = std::to_string(trans_type);
        params["input"] = "1";
        transition_begin(params);
    }
    std::fprintf(stderr,
                 "[runtime] loaded %s (script %s:%zu; cleared %zu ephemeral slot(s), "
                 "restored %zu layer(s)%s%s, in-flight tween=%d anime=%d dropped)\n",
                 path.c_str(), data.current_script.c_str(), data.current_line, cleared,
                 rst.created,
                 rst.skipped_invalid ? " + skipped-invalid" : "",
                 rst.skipped_dup ? " + skipped-duplicate" : "",
                 had_tweens ? 1 : 0, had_anime ? 1 : 0);
    if (rst.skipped_invalid || rst.skipped_dup) {
        std::fprintf(stderr,
                     "[runtime] load: %zu malformed and %zu duplicate layer snapshot(s) "
                     "were skipped (explicit handling; no ghost nodes)\n",
                     rst.skipped_invalid, rst.skipped_dup);
    }
    return LoadResult::Ok;
}

bool GameRuntime::RuntimeState::load_game_from(const std::string& file, int64_t trans_type) {
    return load_game(file, trans_type) == LoadResult::Ok;
}

// ---------------------------------------------------------------------------
// Event dispatch
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::dispatch_save_events() {
    if (pending_events_.empty()) return;
    // Iterate an entry snapshot: load/onLoad may push new events. New events
    // stay at the queue tail for the next dispatch round.
    std::deque<oa::runtime::Event> in;
    in.swap(pending_events_);
    std::deque<oa::runtime::Event> keep;
    for (const auto& e : in) {
        const bool is_save =
            e.tag == "save" || e.tag == "load" || e.tag == "syssave" ||
            e.tag == "autosave" || e.tag == "takess" || e.tag == "savess" ||
            e.tag == "file";
        if (!is_save) {
            keep.push_back(e);
            continue;
        }
        handle_save_tag(e);
    }
    for (auto& e : pending_events_) keep.push_back(std::move(e));
    pending_events_.swap(keep);
}

void GameRuntime::RuntimeState::handle_save_tag(const oa::runtime::Event& e) {
    auto get = [&e](const char* k) -> std::string {
        const auto it = e.params.find(k);
        return it == e.params.end() ? std::string() : it->second;
    };
    if (e.tag == "save") {
        const std::string file = get("file");
        if (file.empty()) {
            (void)syssave(); // no-file [save] == syssave
        } else {
            (void)save_game_to(file, e.save_origin.get());
        }
        return;
    }
    if (e.tag == "syssave") {
        (void)syssave();
        return;
    }
    if (e.tag == "load") {
        const std::string file = get("file");
        int64_t type = -1; // none
        const std::string t = get("type");
        if (!t.empty()) {
            try {
                type = std::stoll(t);
            } catch (...) {
                type = -1;
            }
        }
        if (file.empty()) {
            std::fprintf(stderr, "[runtime] load: empty file ignored\n");
            return;
        }
        // 读档结果是可判定的，调用方**必须**分支处理（旧实现
        // ignore 返回值 ⇒ 预检/损坏失败时只有一行日志，界面以为已经读档）。
        switch (load_game(file, type)) {
            case LoadResult::Ok:
                break;
            case LoadResult::MissingFile:
                std::fprintf(stderr, "[runtime] load: FAILED (file missing); state unchanged\n");
                break;
            case LoadResult::Corrupt:
                std::fprintf(stderr, "[runtime] load: FAILED (corrupt/unsupported save); "
                                     "state unchanged\n");
                break;
            case LoadResult::PositionUnavailable:
                std::fprintf(stderr, "[runtime] load: FAILED (position unavailable); "
                                     "no half-restore\n");
                break;
        }
        return;
    }
    if (e.tag == "autosave") {
        const std::string allow = get("allow");
        save_.autosave_allow_ = allow.empty() ? 1 : int(save_i64(allow, 1));
        return;
    }
    if (e.tag == "takess") {
        // [takess]：把玩家此刻看到的画面存下来给缩略图用。
        //
        // 直接读渲染器的 stage 内容 —— 那就是**上一帧已经呈现出来**的画面，
        // 也正是玩家看到的画面。不需要"记 pending、等本帧画完再抓"。
        capture_screenshot();
        return;
    }
    if (e.tag == "savess") {
        handle_save_screenshot(get("file"), get("width"), get("height"));
        return;
    }
    if (e.tag == "file") {
        handle_file_operation(e.params);
        return;
    }
}

void GameRuntime::RuntimeState::handle_file_operation(const std::map<std::string, std::string>& params) {
    auto get = [&params](const char* k) -> std::string {
        const auto it = params.find(k);
        return it == params.end() ? std::string() : it->second;
    };
    const std::string command = get("command");
    if (command == "delete") {
        const std::string path = qualify_save_file(get("target"));
        if (!path.empty() && save_.save_store_) save_.save_store_->remove(path);
        return;
    }
    if (command == "copy" || command == "move") {
        const std::string src = qualify_save_file(get("src"));
        const std::string dst = qualify_save_file(get("dst"));
        if (!src.empty() && !dst.empty() && save_.save_store_) {
            if (auto data = save_.save_store_->read(src)) {
                if (save_.save_store_->write(dst, *data)) {
                    if (command == "move") save_.save_store_->remove(src);
                }
            }
        }
    }
}

namespace {
/// 可分离双线性重采样（Triangle 的实用等价：两遍一维线性插值；
/// imageops FilterType::Triangle，）。dims 相等时原样拷贝。
std::vector<uint8_t> resize_rgba(const uint8_t* src, uint32_t sw, uint32_t sh,
                                 uint32_t dw, uint32_t dh) {
    std::vector<uint8_t> out(size_t(dw) * dh * 4);
    if (sw == dw && sh == dh) {
        std::memcpy(out.data(), src, out.size());
        return out;
    }
    // Horizontal pass into tmp (dw x sh).
    std::vector<uint8_t> tmp(size_t(dw) * sh * 4);
    for (uint32_t y = 0; y < sh; ++y) {
        const uint8_t* row = src + size_t(y) * sw * 4;
        for (uint32_t x = 0; x < dw; ++x) {
            const double fx = (double(x) + 0.5) * double(sw) / double(dw) - 0.5;
            double sx = fx < 0 ? 0 : fx;
            if (sx > sw - 1) sx = sw - 1;
            const uint32_t x0 = uint32_t(sx);
            const uint32_t x1 = x0 + 1 < sw ? x0 + 1 : x0;
            const double t = sx - x0;
            uint8_t* d = tmp.data() + (size_t(y) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const double v = row[x0 * 4 + c] * (1.0 - t) + row[x1 * 4 + c] * t;
                d[c] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5));
            }
        }
    }
    // Vertical pass.
    for (uint32_t y = 0; y < dh; ++y) {
        const double fy = (double(y) + 0.5) * double(sh) / double(dh) - 0.5;
        double sy = fy < 0 ? 0 : fy;
        if (sy > sh - 1) sy = sh - 1;
        const uint32_t y0 = uint32_t(sy);
        const uint32_t y1 = y0 + 1 < sh ? y0 + 1 : y0;
        const double t = sy - y0;
        for (uint32_t x = 0; x < dw; ++x) {
            uint8_t* d = out.data() + (size_t(y) * dw + x) * 4;
            const uint8_t* p0 = tmp.data() + (size_t(y0) * dw + x) * 4;
            const uint8_t* p1 = tmp.data() + (size_t(y1) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const double v = p0[c] * (1.0 - t) + p1[c] * t;
                d[c] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5));
            }
        }
    }
    return out;
}
} // namespace

void GameRuntime::RuntimeState::capture_screenshot() {
    save_.screenshot_.valid = false;
    if (!save_.capture_fn_) return;
    uint32_t w = 0, h = 0;
    if (auto rgba = save_.capture_fn_(&w, &h)) {
        save_.screenshot_.valid = !rgba->empty() && w > 0 && h > 0 &&
                            rgba->size() == size_t(w) * h * 4;
        save_.screenshot_.w = w;
        save_.screenshot_.h = h;
        save_.screenshot_.rgba = save_.screenshot_.valid ? std::move(*rgba) : std::vector<uint8_t>();
    }
}

void GameRuntime::RuntimeState::handle_save_screenshot(const std::string& file, const std::string& width,
                                         const std::string& height) {
    if (file.empty()) return;
    // Target size from the [savess] params; falls back to the captured frame
    // size, then the placeholder size.
    uint32_t w = 320;
    uint32_t h = 180;
    if (save_.screenshot_.valid && save_.screenshot_.w > 0) w = save_.screenshot_.w;
    if (save_.screenshot_.valid && save_.screenshot_.h > 0) h = save_.screenshot_.h;
    if (!width.empty()) {
        try {
            const long v = std::stol(width);
            if (v > 0) w = uint32_t(v);
        } catch (...) {
        }
    }
    if (!height.empty()) {
        try {
            const long v = std::stol(height);
            if (v > 0) h = uint32_t(v);
        } catch (...) {
        }
    }
    std::vector<uint8_t> rgba;
    if (save_.screenshot_.valid && !save_.screenshot_.rgba.empty()) {
        rgba = resize_rgba(save_.screenshot_.rgba.data(), save_.screenshot_.w, save_.screenshot_.h, w, h);
    } else {
        // Deterministic placeholder: slate background with a border.
        rgba.assign(size_t(w) * h * 4, 0x2A);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t(y) * w + x) * 4;
                if (y == 0 || y == h - 1 || x == 0 || x == w - 1) {
                    rgba[i] = rgba[i + 1] = rgba[i + 2] = uint8_t('o');
                }
                rgba[i + 3] = 255;
            }
        }
    }
    std::string name = file;
    const bool has_png = name.size() > 4 && name.rfind(".png") == name.size() - 4;
    if (!has_png) name += ".png";
    const std::vector<uint8_t> png = oa::media::encode_png(w, h, rgba);
    if (png.empty()) return;
    const std::string path = qualify_save_file(name);
    if (path.empty() || !save_.save_store_) return;
    if (!save_.save_store_->write(path, png)) return;
    // 缩略图是**原地覆盖**写的（同名文件换内容）：层按文件名缓存贴图，覆盖后
    // 框架重建缩略图层也会命中旧图。让渲染器按新文件重新上传（层 file 键不带
    // 扩展名）。
    std::string asset = path;
    if (asset.size() > 4 && asset.rfind(".png") == asset.size() - 4)
        asset.resize(asset.size() - 4);
    renderer_.forget_asset(asset);
}

void GameRuntime::RuntimeState::maybe_autosave_for_wait() {
    // Park identity is a monotonic sequence, not a value signature: two
    // consecutive pages ending in an identical [click]/[@] are distinct parks
    // and must each get their autosave.
    const uint64_t seq = park_.wait ? park_.wait->seq : 0;
    if (seq != save_.autosave_last_seq_) {
        save_.autosave_last_seq_ = seq;
        save_.autosaved_current_wait_ = false;
    }
    // `[autosave allow]` 的既有语义：只有 2 = 引擎自动存档。iMel 的每页
    // allow=1 是"允许框架自己的 *オートセーブ 在场景点落盘"，不是让引擎
    // 在每个内部停驻都写一份（那会把 macro.iet 的每次 [stop] 都存成档）。
    if (save_.autosave_allow_ == 2 && is_input_wait(park_.wait ? &*park_.wait : nullptr) &&
        !save_.autosaved_current_wait_ && save_.save_store_ && save_.save_store_->persistent()) {
        save_.autosaved_current_wait_ = true;
        (void)save_game_to(kAutosaveFile);
    }
}

} // namespace oa::runtime

// ---------------------------------------------------------------------------
// Save format implementation: the
// binary codec for SaveData / sys-domain documents.
// ---------------------------------------------------------------------------

namespace oa::runtime {

namespace {

using oa::util::FormatError;
using oa::util::Reader;
using oa::util::Writer;

// Payload field names.
constexpr const char* kLocalVars = "local_variables";
constexpr const char* kCurrentScript = "current_script";
constexpr const char* kCurrentLine = "current_line";
constexpr const char* kCallStack = "call_stack";
constexpr const char* kStoryCallLine = "story_call_line";
constexpr const char* kPageReplay = "page_replay";
constexpr const char* kScript = "script";
constexpr const char* kReturnLine = "return_line";
constexpr const char* kLine = "line";
constexpr const char* kScene = "scene";
constexpr const char* kRootProps = "root_props";
constexpr const char* kLayers = "layers";
constexpr const char* kPageAnchor = "page_anchor";
constexpr const char* kPageMeswindow = "page_meswindow";
constexpr const char* kId = "id";
constexpr const char* kProps = "props";
constexpr const char* kHandlers = "handlers";
constexpr const char* kType = "type";
constexpr const char* kEnabled = "enabled";
constexpr const char* kPenetration = "penetration";
constexpr const char* kHandler = "handler";
constexpr const char* kFile = "file";
constexpr const char* kLabel = "label";
constexpr const char* kCall = "call";
constexpr const char* kParams = "params";
constexpr const char* kFilterParams = "filter_params";
constexpr const char* kAudio = "audio";
constexpr const char* kBgm = "bgm";
constexpr const char* kSe = "se";
constexpr const char* kVoice = "voice";
constexpr const char* kLoopPlay = "loop_play";
constexpr const char* kGain = "gain";
constexpr const char* kPan = "pan";
constexpr const char* kSkippable = "skippable";
constexpr const char* kText = "text";
constexpr const char* kTextLayers = "text_layers";
constexpr const char* kBacklog = "backlog";
constexpr const char* kBacklogWrite = "backlog_write";
constexpr const char* kBacklogAllow = "backlog_allow";
constexpr const char* kBacklogPages = "pages";
constexpr const char* kPageFont = "page_font";
constexpr const char* kTags = "tags";
constexpr const char* kKind = "kind";
constexpr const char* kActive = "active";
constexpr const char* kPositioned = "positioned";
constexpr const char* kLayered = "layered";
constexpr const char* kFont = "font";
constexpr const char* kBox = "box";
constexpr const char* kVisual = "visual";
constexpr const char* kRevealed = "revealed";

/// Version gate: newer → FormatError; older → defaults (no migration).
void check_version(const Reader& r) {
    if (r.version() > uint32_t(kSaveFormatVersion)) {
        throw FormatError("save format version " + std::to_string(r.version()) +
                          " newer than supported " +
                          std::to_string(kSaveFormatVersion));
    }
}

// ---------------------------------------------------------------------------
// runtime::Value <-> binary tags (nil/bool/int/float64/string only; the raw
// tags exist at the stream level for future binary carriers).
// ---------------------------------------------------------------------------
void write_value(Writer& w, const oa::runtime::Value& v) {
    switch (v.kind) {
        case oa::runtime::ValueKind::Int:
            w.i64(v.int_val);
            break;
        case oa::runtime::ValueKind::Float:
            w.f64(v.float_val);
            break;
        case oa::runtime::ValueKind::String:
            w.str(v.str_val);
            break;
        case oa::runtime::ValueKind::Bool:
            w.boolean(v.bool_val);
            break;
        case oa::runtime::ValueKind::Null:
        default:
            w.nil();
            break;
    }
}

oa::runtime::Value read_value(Reader& r) {
    const uint8_t t = r.peek_tag();
    if (t == oa::util::kTagNil) {
        (void)r.u8();
        return oa::runtime::Value::make_null();
    }
    if (t == oa::util::kTagTrue) {
        (void)r.u8();
        return oa::runtime::Value::make_bool(true);
    }
    if (t == oa::util::kTagFalse) {
        (void)r.u8();
        return oa::runtime::Value::make_bool(false);
    }
    if (Reader::is_int_tag(t)) return oa::runtime::Value::make_int(r.i64());
    if ((t >= 0xa0 && t <= 0xbf) || t == oa::util::kTagStr8 ||
        t == oa::util::kTagStr16 || t == oa::util::kTagStr32) {
        return oa::runtime::Value::make_string(r.str());
    }
    if (t == oa::util::kTagFloat32 || t == oa::util::kTagFloat64) {
        return oa::runtime::Value::make_float(r.f64());
    }
    throw FormatError("unsupported tag in variable value position");
}

// ---------------------------------------------------------------------------
// String maps (layer props / root props / handler params).
// ---------------------------------------------------------------------------
void write_str_map(Writer& w, const std::map<std::string, std::string>& m) {
    w.map_begin(m.size());
    for (const auto& [k, v] : m) {
        w.str(k);
        w.str(v);
    }
}

std::map<std::string, std::string> read_str_map(Reader& r) {
    std::map<std::string, std::string> out;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string k = r.str();
        out[k] = r.str();
    }
    return out;
}

// ---------------------------------------------------------------------------
// Audio snapshot.
// ---------------------------------------------------------------------------
void write_channel(Writer& w, const AudioChannelSnap& c) {
    w.map_begin(6);
    w.str(kId);
    w.str(c.id);
    w.str(kFile);
    w.str(c.file);
    w.str(kLoopPlay);
    w.boolean(c.loop_play);
    w.str(kGain);
    w.i64(c.gain);
    w.str(kPan);
    w.i64(c.pan);
    w.str(kSkippable);
    w.boolean(c.skippable);
}

AudioChannelSnap read_channel(Reader& r) {
    AudioChannelSnap c;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kId) {
            c.id = r.str();
        } else if (key == kFile) {
            c.file = r.str();
        } else if (key == kLoopPlay) {
            c.loop_play = r.boolean();
        } else if (key == kGain) {
            const int64_t v = r.i64();
            if (v < std::numeric_limits<int32_t>::min() ||
                v > std::numeric_limits<int32_t>::max())
                throw FormatError("channel gain out of int32 range");
            c.gain = int32_t(v);
        } else if (key == kPan) {
            const int64_t v = r.i64();
            if (v < std::numeric_limits<int32_t>::min() ||
                v > std::numeric_limits<int32_t>::max())
                throw FormatError("channel pan out of int32 range");
            c.pan = int32_t(v);
        } else if (key == kSkippable) {
            c.skippable = r.boolean();
        } else {
            r.skip_value();
        }
    }
    return c;
}

void write_channels(Writer& w, const char* key,
                    const std::vector<AudioChannelSnap>& v) {
    w.str(key);
    w.array_begin(v.size());
    for (const auto& c : v) write_channel(w, c);
}

std::vector<AudioChannelSnap> read_channels(Reader& r) {
    std::vector<AudioChannelSnap> out;
    const size_t n = r.array_items();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(read_channel(r));
    return out;
}

void write_audio(Writer& w, const AudioSnap& a) {
    w.map_begin((a.bgm ? 1 : 0) + 2);
    if (a.bgm) {
        w.str(kBgm);
        write_channel(w, *a.bgm);
    }
    write_channels(w, kSe, a.se);
    write_channels(w, kVoice, a.voice);
}

AudioSnap read_audio(Reader& r) {
    AudioSnap a;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kBgm) {
            a.bgm = read_channel(r);
        } else if (key == kSe) {
            a.se = read_channels(r);
        } else if (key == kVoice) {
            a.voice = read_channels(r);
        } else {
            r.skip_value();
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// Layer event handlers + layer snapshot.
// ---------------------------------------------------------------------------
void write_handler(Writer& w, const LayerEventHandlerSnap& h) {
    w.map_begin(9);
    w.str(kType);
    w.str(h.type);
    w.str(kEnabled);
    w.boolean(h.enabled);
    w.str(kPenetration);
    w.boolean(h.penetration);
    w.str(kHandler);
    w.str(h.handler);
    w.str(kFile);
    w.str(h.file);
    w.str(kLabel);
    w.str(h.label);
    w.str(kCall);
    w.boolean(h.call);
    w.str(kParams);
    write_str_map(w, h.params);
    w.str(kFilterParams);
    write_str_map(w, h.filter_params);
}

LayerEventHandlerSnap read_handler(Reader& r) {
    LayerEventHandlerSnap h;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kType) {
            h.type = r.str();
        } else if (key == kEnabled) {
            h.enabled = r.boolean();
        } else if (key == kPenetration) {
            h.penetration = r.boolean();
        } else if (key == kHandler) {
            h.handler = r.str();
        } else if (key == kFile) {
            h.file = r.str();
        } else if (key == kLabel) {
            h.label = r.str();
        } else if (key == kCall) {
            h.call = r.boolean();
        } else if (key == kParams) {
            h.params = read_str_map(r);
        } else if (key == kFilterParams) {
            h.filter_params = read_str_map(r);
        } else {
            r.skip_value();
        }
    }
    return h;
}

void write_text_layer(Writer& w, const TextLayerSnap& ts) {
    w.map_begin(ts.layered >= 0 ? 8 : 7);
    w.str(kId);
    w.str(ts.id);
    w.str(kActive);
    w.boolean(ts.active);
    w.str(kPositioned);
    w.boolean(ts.positioned);
    w.str(kRevealed);
    w.boolean(ts.revealed);
    if (ts.layered >= 0) {
        w.str(kLayered);
        w.i64(ts.layered);
    }
    w.str(kBox);
    w.map_begin(4);
    w.str("left");
    w.f64(ts.left);
    w.str("top");
    w.f64(ts.top);
    w.str("width");
    w.f64(ts.width);
    w.str("height");
    w.f64(ts.height);
    w.str(kFont);
    write_str_map(w, ts.font);
    w.str(kText);
    w.str(ts.text);
}

void write_layer(Writer& w, const LayerSnap& ls) {    w.map_begin(ls.handlers.empty() ? 2 : 3);
    w.str(kId);
    w.str(ls.id);
    w.str(kProps);
    write_str_map(w, ls.props);
    if (!ls.handlers.empty()) {
        w.str(kHandlers);
        w.array_begin(ls.handlers.size());
        for (const auto& h : ls.handlers) write_handler(w, h);
    }
}

TextLayerSnap read_text_layer(Reader& r) {
    TextLayerSnap ts;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kId) {
            ts.id = r.str();
        } else if (key == kActive) {
            ts.active = r.boolean();
        } else if (key == kPositioned) {
            ts.positioned = r.boolean();
        } else if (key == kLayered) {
            ts.layered = int(r.i64());
        } else if (key == kRevealed) {
            ts.revealed = r.boolean();
        } else if (key == kBox) {
            const size_t bn = r.map_entries();
            for (size_t j = 0; j < bn; ++j) {
                const std::string bkey = r.str();
                if (bkey == "left") ts.left = r.f64();
                else if (bkey == "top") ts.top = r.f64();
                else if (bkey == "width") ts.width = r.f64();
                else if (bkey == "height") ts.height = r.f64();
                else r.skip_value();
            }
        } else if (key == kFont) {
            ts.font = read_str_map(r);
        } else if (key == kText) {
            ts.text = r.str();
        } else {
            r.skip_value();
        }
    }
    return ts;
}

LayerSnap read_layer(Reader& r);

void write_backlog_page(Writer& w, const BacklogPageSnap& bp) {
    // 回顾页只存**重放命令**（print/font/rt…）。画面与脚本锚点属于框架
    // 自己的载体（backlog_N.dat / log.stack / blj），引擎不复制一份。
    w.map_begin(2);
    w.str(kPageFont);
    write_str_map(w, bp.page_font);
    w.str(kTags);
    w.array_begin(bp.tags.size());
    for (const auto& t : bp.tags) {
        w.map_begin(4);
        w.str(kKind);
        w.i64(t.kind);
        w.str(kText);
        w.str(t.text);
        w.str(kFont);
        write_str_map(w, t.font);
        w.str(kParams);
        write_str_map(w, t.params);
    }
}

BacklogPageSnap read_backlog_page(Reader& r) {
    BacklogPageSnap bp;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kPageFont) {
            bp.page_font = read_str_map(r);
        } else if (key == kTags) {
            const size_t tn = r.array_items();
            bp.tags.reserve(tn);
            for (size_t j = 0; j < tn; ++j) {
                BacklogTagSnap t;
                const size_t mn = r.map_entries();
                for (size_t k = 0; k < mn; ++k) {
                    const std::string tk = r.str();
                    if (tk == kKind) t.kind = int(r.i64());
                    else if (tk == kText) t.text = r.str();
                    else if (tk == kFont) t.font = read_str_map(r);
                    else if (tk == kParams) t.params = read_str_map(r);
                    else r.skip_value();
                }
                bp.tags.push_back(std::move(t));
            }
        } else {
            // 旧档的 script/line/call_stack/visual：跳过，不迁移。
            r.skip_value();
        }
    }
    return bp;
}

LayerSnap read_layer(Reader& r) {
    LayerSnap ls;
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kId) {
            ls.id = r.str();
        } else if (key == kProps) {
            ls.props = read_str_map(r);
        } else if (key == kHandlers) {
            const size_t hn = r.array_items();
            ls.handlers.reserve(hn);
            for (size_t j = 0; j < hn; ++j) ls.handlers.push_back(read_handler(r));
        } else {
            r.skip_value();
        }
    }
    return ls;
}

} // namespace

std::string SaveData::encode() const {
    if (current_line > size_t(std::numeric_limits<int64_t>::max()) ||
        call_stack.size() > size_t(std::numeric_limits<int64_t>::max())) {
        throw FormatError("save position out of int64 range");
    }
    Writer w;
    w.map_begin(4 + (has_scene ? 1 : 0) + (has_audio ? 1 : 0) + (has_text ? 1 : 0) +
                (has_backlog ? 1 : 0) + (story_call_line ? 1 : 0) + (page_replay ? 1 : 0) +
                (page_anchor ? 1 : 0) + (page_meswindow.empty() ? 0 : 1));
    w.str(kLocalVars);
    w.map_begin(local_variables.size());
    for (const auto& [k, v] : local_variables) {
        w.str(k);
        write_value(w, v);
    }
    w.str(kCurrentScript);
    w.str(current_script);
    w.str(kCurrentLine);
    w.i64(int64_t(current_line));
    w.str(kCallStack);
    w.array_begin(call_stack.size());
    for (const auto& [script, line] : call_stack) {
        if (line > size_t(std::numeric_limits<int64_t>::max())) {
            throw FormatError("return line out of int64 range");
        }
        w.map_begin(2);
        w.str(kScript);
        w.str(script);
        w.str(kReturnLine);
        w.i64(int64_t(line));
    }
    if (story_call_line) {
        w.str(kStoryCallLine);
        w.i64(int64_t(story_call_line));
    }
    if (page_replay) {
        w.str(kPageReplay);
        w.boolean(true);
    }
    if (page_anchor) {
        w.str(kPageAnchor);
        w.boolean(true);
    }
    if (!page_meswindow.empty()) {
        w.str(kPageMeswindow);
        w.str(page_meswindow);
    }
    if (has_scene) {
        w.str(kScene);        w.map_begin(2);
        w.str(kRootProps);
        write_str_map(w, root_props);
        w.str(kLayers);
        w.array_begin(layers.size());
        for (const auto& ls : layers) write_layer(w, ls);
    }
    if (has_audio) {
        w.str(kAudio);
        write_audio(w, audio);
    }
    if (has_text) {
        w.str(kText);
        w.map_begin(1);
        w.str(kTextLayers);
        w.array_begin(text_layers.size());
        for (const auto& ts : text_layers) write_text_layer(w, ts);
    }
    if (has_backlog) {
        w.str(kBacklog);
        w.map_begin(3);
        w.str(kBacklogWrite);
        w.boolean(backlog_write_mode);
        w.str(kBacklogAllow);
        w.boolean(backlog_allow);
        w.str(kBacklogPages);
        w.array_begin(backlog.size());
        for (const auto& bp : backlog) write_backlog_page(w, bp);
    }
    return w.data();
}

// ---------------------------------------------------------------------------
// 官方 BOWS 槽位 → SaveData（尽力而为的导入）
// ---------------------------------------------------------------------------
namespace {

/// BOWS slot → engine SaveData (best-effort: variables, commands, BGM,
/// position). In-flight tween/wait timing is not recoverable.
/// Gaps: docs/OFFICIAL_SAVES.md.
oa::runtime::SaveData official_slot_to_save_data(
    const oa::runtime::OfficialSlot& slot) {
    oa::runtime::SaveData d;
    d.version = kSaveFormatVersion;

    size_t pluto_converted = 0;
    for (const auto& [key, raw] : slot.header.variables) {
        if (key.empty()) continue;
        std::string value = raw;
        if (auto doc = oa::runtime::convert_official_pluto(raw)) {
            value = std::move(*doc);
            ++pluto_converted;
        }
        d.local_variables[key] = oa::runtime::Value::make_string(std::move(value));
    }

    // 位置：内层在前（frames[0] = 当前执行位置）。
    const oa::runtime::OfficialFrame* current = nullptr;
    if (!slot.frames.empty()) {
        current = &slot.frames.front();
    } else if (slot.has_system_frame) {
        current = &slot.system_frame;
    }
    if (current) {
        d.current_script = current->script;
        d.current_line = current->line;
    }
    // Call stack pairs (frames[i].return_line, frames[i+1].script); 0 = root.
    for (size_t i = 0; i + 1 < slot.frames.size(); ++i) {
        const auto& f = slot.frames[i];
        const auto& caller = slot.frames[i + 1];
        if (caller.script.empty() || f.return_line == 0) continue;
        d.call_stack.emplace_back(caller.script, f.return_line);
    }

    if (!slot.image_layers.empty()) {
        d.has_scene = true;
        d.layers.reserve(slot.image_layers.size());
        for (const auto& layer : slot.image_layers) {
            if (layer.id.empty()) continue;
            oa::runtime::LayerSnap snap;
            snap.id = layer.id;
            for (const auto& cmd : layer.commands) {
                if (cmd.name == "lyc" || cmd.name == "lyprop") {
                    // 属性就是命令的 attrs（名字→值），"id" 由图层键给出，跳过。
                    for (const auto& [name, value] : cmd.attrs) {
                        if (name.empty() || name == "id") continue;
                        snap.props[name] = value;
                    }
                } else if (cmd.name == "lyevent") {
                    // lyevent 的 attrs 与脚本里的 [lyevent] 同构，所以按
                    // Compositor::apply_lyevent 的规则还原：已知字段进 row，
                    // **其余全部进 params**（function/name/key/lua/… 是回调要
                    // 用的注册参数），filter_params 再补上完整的那一份。
                    const auto get = [&cmd](const char* k) {
                        const auto it = cmd.attrs.find(k);
                        return it == cmd.attrs.end() ? std::string() : it->second;
                    };
                    const std::string mode = get("mode");
                    if (mode == "reset") continue; // reset = 删掉这一行
                    static const char* kKnown[] = {"id",           "type",   "mode",
                                                   "file",         "label",  "call",
                                                   "handler",      "penetration"};
                    std::map<std::string, std::string> extra;
                    for (const auto& [name, value] : cmd.attrs) {
                        if (name.empty()) continue;
                        bool known = false;
                        for (const char* k : kKnown) known = known || name == k;
                        if (!known) extra[name] = value;
                    }
                    const auto make_row = [&](const std::string& type,
                                              const std::string& function_key,
                                              const std::string& function) {
                        oa::runtime::LayerEventHandlerSnap h;
                        h.type = type;
                        h.enabled = mode != "disable";
                        const std::string handler = get("handler");
                        h.handler = handler;
                        h.file = get("file");
                        h.label = get("label");
                        const std::string call = get("call");
                        h.call = save_i64(call, 0) != 0;
                        h.penetration = save_i64(get("penetration"), 0) != 0;
                        h.params = extra;
                        if (!function_key.empty()) h.params[function_key] = function;
                        h.filter_params = extra;
                        const std::pair<const char*, std::string> fields[] = {
                            {"id", layer.id}, {"type", type},       {"mode", mode},
                            {"file", h.file}, {"label", h.label},   {"handler", handler}};
                        for (const auto& [k, v] : fields) {
                            if (!v.empty()) h.filter_params[k] = v;
                        }
                        if (h.call) h.filter_params["call"] = "1";
                        if (h.penetration) h.filter_params["penetration"] = "1";
                        return h;
                    };
                    const std::string type = get("type");
                    if (!type.empty()) {
                        snap.handlers.push_back(make_row(type, "", ""));
                        continue;
                    }
                    // type 缺席 = FPM glue（button.lua 的写法）：一个 click/over/
                    // out… 键各展开一行，与 apply_lyevent 的展开同一张表。
                    static const char* kGlue[][2] = {{"click", "click"},   {"over", "rollover"},
                                                     {"out", "rollout"},   {"dragin", "dragin"},
                                                     {"drag", "drag"},     {"dragout", "dragout"}};
                    bool expanded = false;
                    for (const auto& pair : kGlue) {
                        const auto it = cmd.attrs.find(pair[0]);
                        if (it == cmd.attrs.end()) continue;
                        expanded = true;
                        auto row = make_row(pair[1], "function", it->second);
                        if (row.handler.empty()) row.handler = "calllua";
                        snap.handlers.push_back(std::move(row));
                    }
                    (void)expanded;
                }
            }
            d.layers.push_back(std::move(snap));
        }
    }

    if (!slot.text_layers.empty()) {
        d.has_text = true;
        for (const auto& layer : slot.text_layers) {
            if (layer.id.empty()) continue;
            oa::runtime::TextLayerSnap ts;
            ts.id = layer.id;
            ts.active = layer.id == slot.current_message_layer;
            ts.revealed = true;
            for (const auto& cmd : layer.commands) {
                if (cmd.name == "print") {
                    const auto it = cmd.attrs.find("data");
                    if (it != cmd.attrs.end()) ts.text += it->second;
                } else if (cmd.name == "font") {
                    for (const auto& [name, value] : cmd.attrs) {
                        if (name.empty() || name == "id") continue;
                        ts.font[name] = value;
                    }
                }
            }
            // 文字框几何：官方把 (left, top, width, height) 存在排版状态（f25）
            // 里，[font] 指令本身经常只带颜色/字号（test4 就是这样），只带
            // left/top 而不带宽高的也有（test13）——所以以 f25 的框为准，指令
            // 里剩下的属性照样用。
            const auto rect = slot.text_rects.find(layer.id);
            if (rect != slot.text_rects.end()) {
                ts.positioned = true;
                ts.left = rect->second.left;
                ts.top = rect->second.top;
                ts.width = rect->second.width;
                ts.height = rect->second.height;
            }
            // 只有带文本（或当前活动）的层才进快照：f24 里 40 多个层大多是
            // 名字框/ruby 框，没有正文，恢复它们只会平白多建消息槽。
            if (!ts.text.empty() || ts.active) {
                d.text_layers.push_back(std::move(ts));
            }
        }
    }

    if (!slot.bgm.empty()) {
        const auto& cmd = slot.bgm.front();
        if (cmd.name == "splay") {
            const auto attr = [&cmd](const char* k) {
                const auto it = cmd.attrs.find(k);
                return it == cmd.attrs.end() ? std::string() : it->second;
            };
            oa::runtime::AudioChannelSnap ch;
            ch.id = "bgm";
            ch.file = attr("file");
            const std::string loop = attr("loop");
            ch.loop_play = !loop.empty() && loop != "0";
            ch.gain = int(save_i64(attr("gain"), 1000));
            ch.pan = int(save_i64(attr("pan"), 0));
            if (!ch.file.empty()) {
                d.has_audio = true;
                d.audio.bgm = std::move(ch);
            }
        }
    }

    std::string tail;
    if (slot.frames.size() > 1) tail = " stack=" + std::to_string(slot.frames.size());
    if (!slot.frames.empty() && slot.frames.front().has_current) {
        tail += " at-tag=" + slot.frames.front().current.name;
        const size_t pending = slot.frames.front().pending.size();
        if (pending != 0) tail += "+" + std::to_string(pending) + " pending";
    }
    size_t boxes = 0;
    for (const auto& t : d.text_layers) {
        if (t.positioned) ++boxes;
    }
    size_t handlers = 0;
    for (const auto& l : d.layers) handlers += l.handlers.size();
    std::fprintf(stderr,
                 "[save] official BOWS v%u → SaveData: %zu variable(s) (%zu Pluto table(s) "
                 "converted), %zu layer(s)/%zu handler(s), %zu text layer(s) (%zu box(es)), "
                 "bgm=%s, at %s:%zu%s%s\n",
                 slot.header.version, d.local_variables.size(), pluto_converted, d.layers.size(),
                 handlers, d.text_layers.size(), boxes,
                 d.has_audio ? d.audio.bgm->file.c_str() : "(none)", d.current_script.c_str(),
                 d.current_line,
                 slot.frames_truncated ? " [call stack truncated: unknown frame shape]" : "",
                 tail.c_str());
    return d;
}

} // namespace

SaveData SaveData::decode(const std::string& bytes) {
    // 官方编号存档（BOWS）：容器 + 命令栈能解，但官方的模型是"每图层命令栈 +
    // 脚本栈"，与本文档的"脚本位置 + 快照"不是一对一。这里做**导入**：
    // 位置/变量/图层/文本/音频逐项搬过来（见 official_slot_to_save_data），
    // 解不出来的部分（例如字段 4 的后续帧）留在日志里，不假装成功。
    if (oa::runtime::is_official_save(bytes)) {
        const auto slot = oa::runtime::decode_official_slot(bytes);
        SaveData imported = official_slot_to_save_data(slot);
        if (imported.current_script.empty()) {
            throw FormatError(
                "official BOWS save: no script frame could be decoded, so there is no "
                "position to resume from (see docs/OFFICIAL_SAVES.md)");
        }
        return imported;
    }
    Reader r(bytes);
    check_version(r);
    SaveData d;
    d.version = int(r.version());
    const size_t n = r.map_entries();
    for (size_t i = 0; i < n; ++i) {
        const std::string key = r.str();
        if (key == kLocalVars) {
            const size_t vn = r.map_entries();
            for (size_t j = 0; j < vn; ++j) {
                const std::string name = r.str();
                Value v = read_value(r);
                // 旧版本把 t.* 一起存进了槽位变量表。t.* 是 scratch，
                // **绝不能**在读档时回灌：t10 的 t.saveslot 一旦非 0，
                // save.iet 会走"保存成功"分支，读档就变成存档收尾。
                if (name.rfind("t.", 0) == 0) continue;
                d.local_variables[name] = std::move(v);
            }
        } else if (key == kCurrentScript) {
            d.current_script = r.str();
        } else if (key == kCurrentLine) {
            const int64_t line = r.i64();
            if (line < 0) throw FormatError("negative current_line in save");
            d.current_line = size_t(line);
        } else if (key == kCallStack) {
            const size_t cn = r.array_items();
            d.call_stack.reserve(cn);
            for (size_t j = 0; j < cn; ++j) {
                std::string script;
                int64_t line = 0;
                const size_t fn = r.map_entries();
                for (size_t k = 0; k < fn; ++k) {
                    const std::string fkey = r.str();
                    if (fkey == kScript) {
                        script = r.str();
                    } else if (fkey == kReturnLine) {
                        line = r.i64();
                    } else {
                        r.skip_value();
                    }
                }
                if (line < 0) throw FormatError("negative return_line in save");
                d.call_stack.emplace_back(std::move(script), size_t(line));
            }
        } else if (key == kStoryCallLine) {
            d.story_call_line = size_t(r.i64());
        } else if (key == kPageReplay) {
            d.page_replay = r.boolean();
        } else if (key == kPageAnchor) {
            d.page_anchor = r.boolean();
        } else if (key == kPageMeswindow) {
            d.page_meswindow = r.str();
        } else if (key == kScene) {
            d.has_scene = true;
            const size_t sn = r.map_entries();
            for (size_t j = 0; j < sn; ++j) {
                const std::string skey = r.str();
                if (skey == kRootProps) {
                    d.root_props = read_str_map(r);
                } else if (skey == kLayers) {
                    const size_t ln = r.array_items();
                    d.layers.reserve(ln);
                    for (size_t k = 0; k < ln; ++k) d.layers.push_back(read_layer(r));
                } else {
                    r.skip_value();
                }
            }
        } else if (key == kAudio) {
            d.has_audio = true;
            d.audio = read_audio(r);
        } else if (key == kText) {
            d.has_text = true;
            const size_t tn = r.map_entries();
            for (size_t j = 0; j < tn; ++j) {
                const std::string tkey = r.str();
                if (tkey != kTextLayers) {
                    r.skip_value();
                    continue;
                }
                const size_t ln = r.array_items();
                d.text_layers.reserve(ln);
                for (size_t k = 0; k < ln; ++k) d.text_layers.push_back(read_text_layer(r));
            }
        } else if (key == kBacklog) {
            d.has_backlog = true;
            const size_t bn = r.map_entries();
            for (size_t j = 0; j < bn; ++j) {
                const std::string bkey = r.str();
                if (bkey == kBacklogWrite) {
                    d.backlog_write_mode = r.boolean();
                } else if (bkey == kBacklogAllow) {
                    d.backlog_allow = r.boolean();
                } else if (bkey == kBacklogPages) {
                    const size_t pn = r.array_items();
                    d.backlog.reserve(pn);
                    for (size_t k = 0; k < pn; ++k) d.backlog.push_back(read_backlog_page(r));
                } else {
                    r.skip_value();
                }
            }
        } else {
            r.skip_value();
        }
    }
    r.expect_eof("save document");
    return d;
}

std::string encode_domain_map(const std::map<std::string, oa::runtime::Value>& vars) {
    Writer w;
    w.map_begin(vars.size());
    for (const auto& [k, v] : vars) {
        w.str(k);
        write_value(w, v);
    }
    return w.data();
}

std::map<std::string, oa::runtime::Value> decode_domain_map(const std::string& bytes) {
    Reader r(bytes);
    check_version(r);
    std::map<std::string, oa::runtime::Value> out;
    const size_t n = r.map_entries(); // throws when the root is not a map
    for (size_t i = 0; i < n; ++i) {
        const std::string k = r.str();
        out[k] = read_value(r);
    }
    r.expect_eof("domain map");
    return out;
}

} // namespace oa::runtime

// ---------------------------------------------------------------------------
// SaveStore implementations (moved from core/fs/store.cpp).
// ---------------------------------------------------------------------------

namespace oa::runtime {

DirSaveStore::DirSaveStore(std::string root) : root_(std::move(root)) {
    if (!root_.empty()) mount_ = std::make_unique<oa::fs::WritableMount>(root_);
}

bool DirSaveStore::write(const std::string& rel_path, const std::vector<uint8_t>& data) {
    return mount_ && mount_->write(rel_path, data);
}

std::optional<std::vector<uint8_t>> DirSaveStore::read(const std::string& rel_path) const {
    return mount_ ? mount_->read(rel_path) : std::nullopt;
}

bool DirSaveStore::remove(const std::string& rel_path) {
    return mount_ && mount_->remove(rel_path);
}

bool DirSaveStore::exists(const std::string& rel_path) const {
    return mount_ && mount_->exists(rel_path);
}

std::optional<std::array<int64_t, 6>> DirSaveStore::modification_time(
    const std::string& rel_path) const {
    return mount_ ? mount_->modification_time(rel_path) : std::nullopt;
}

} // namespace oa::runtime
