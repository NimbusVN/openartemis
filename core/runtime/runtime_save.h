#pragma once
// Engine save formats — runtime save contract (part of the runtime_save family; implementation lives in runtime_save.cpp, binary since the...
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/fs/physfs_fs.h"
#include "core/render/scene_snapshot.h"
#include "core/runtime/runtime_iet.h"

namespace oa::runtime {

/// Logical save schema version (= the binary header version; must match
/// oa::util::kFormatVersion). Bump on any layout change below.
/// 维护契约：任何字段增删改都必须同步 bump 本常量，并检查
/// 读侧对"失效 id"的容忍（层 id 属数据，但读档后可能不存在——要么重建、
/// 要么显式判空跳过；旧档只补默认值，不写迁移器）。
constexpr int kSaveFormatVersion = 1;

// ---------------------------------------------------------------------------
// Audio replay snapshot ( AudioSnapshot / AudioChannelSnapshot).
// ---------------------------------------------------------------------------
struct AudioChannelSnap {
    std::string id;
    std::string file;
    bool loop_play = false;
    int32_t gain = 1000; // raw Artemis scale
    int32_t pan = 0;
    bool skippable = false;
};

struct AudioSnap {
    std::optional<AudioChannelSnap> bgm;
    std::vector<AudioChannelSnap> se;
    std::vector<AudioChannelSnap> voice;
    bool empty() const { return !bgm && se.empty() && voice.empty(); }
};

// ---------------------------------------------------------------------------
// 统一存档点（= 官方 BOWS 的语义，一份结构服务 save/qsave/autosave）：
//
//   SavePoint
//     ├── 脚本栈    current_script / current_line / call_stack
//     ├── 变量域    local_variables（**只有**无前缀 local；g./s./t. 不进来）
//     ├── 画面      has_scene / root_props / layers（BOWS 字段 26）
//     ├── 文本面    has_text / text_layers（BOWS 字段 24/25）
//     └── 音频      has_audio / audio（BOWS 字段 29）
//
// 回顾跳转不在这份结构里存"每页一份画面"：框架自己保存载体
// （t10 的 backlog_N.dat 就是一份本结构；t13 是 log.stack[].scr；
//  NekoMiko 是 log.stack[].blj），跳转 = [load] 载体 + 脚本重放/快进。
// ---------------------------------------------------------------------------
using LayerEventHandlerSnap = oa::render::LayerEventHandlerSnap;
using LayerSnap = oa::render::LayerSnap;
using TextLayerSnap = oa::render::TextLayerSnap;

struct PageState {
    std::map<std::string, oa::runtime::Value> local_variables; // 只有无前缀 local
    std::string current_script;
    size_t current_line = 0;
    std::vector<std::pair<std::string, size_t>> call_stack; // {script, return_line}
    /// true = `current_script/current_line` 是**这一页的起点**（读档会重放
    /// 这一页再停在页尾停驻）。iMel 的存档动作发生在 save.iet 里，精确位置
    /// 已经越过页尾的 [@]；页起点是框架自己的重放入口。
    bool page_anchor = true;
    /// 保存发生在框架脚本（save.iet 等）时，最内层**剧情帧**压帧时正在
    /// 执行的指令（官方 BOWS frame.current）。读档回到这一帧时先回到
    /// `call_line`，把这一页的宏调用重走一遍再停在 `[@]`；否则直接回到
    /// `return_line` 会立刻打印下一页。0 = 旧档/不适用。
    size_t story_call_line = 0;
    /// true = 位置是**这一页的脚本起点**，读档时脚本会把这一页重放一遍
    /// （画面/文本仍由 surface 恢复）。只在存档发生在框架脚本里时使用；
    /// 旧档没有 → false，按精确位置恢复。
    bool page_replay = false;
    /// 剧情页的消息窗 id（`[adv mw=...]` 的页面级配置）。框架读档后用它选
    /// sysbtn/窗口图片目录；存档页会临时改写 `meswindow.num`，所以单独存
    /// 页面值（不是画面快照，只是一条页面变量）。旧档没有 → 空。
    std::string page_meswindow;
    // 画面（场景层）
    bool has_scene = false;
    std::map<std::string, std::string> root_props;
    std::vector<LayerSnap> layers;
    // 文本面
    bool has_text = false;
    std::vector<TextLayerSnap> text_layers;
    // Audio (optional).
    bool has_audio = false;
    AudioSnap audio;
};

/// 引擎回顾表（`get_backlog_size` / `backlog_tags`）的一页。
///
/// 它不是"打开着的回顾页 UI"，而是**剧情已经读过的行**：读档后脚本的
/// `system/backlog.iet` 靠 `e:var(system="get_backlog_size")` 和
/// `backlog_tags(page)` 重建回顾页。不存它，读档后回顾页就是空的
/// （用户报的"回顾页没有字"）。
///
/// 页锚点/画面快照**不在这里**：跳转由框架自己的载体负责（见上）。
struct BacklogTagSnap {
    int kind = 0; // oa::render::BacklogTag::Kind
    std::string text;
    std::map<std::string, std::string> font;
    std::map<std::string, std::string> params;
};
struct BacklogPageSnap {
    std::map<std::string, std::string> page_font;
    std::vector<BacklogTagSnap> tags;
};

// --------------------------------------------------------------------------- One numbered save.
struct SaveData : PageState {
    int version = kSaveFormatVersion;
    // Engine backlog (read history) + its write config. Optional; a save
    // without it restores an empty backlog (旧档) instead of failing.
    bool has_backlog = false;
    bool backlog_write_mode = false;
    bool backlog_allow = true;
    std::vector<BacklogPageSnap> backlog;

    /// Encode to one binary document.
    std::string encode() const;
    /// Decode one binary document; throws oa::util::FormatError
    /// (a std::runtime_error) on malformed data or a newer format version.
    static SaveData decode(const std::string& bytes);
};

/// Flat domain-map binary document for saveg.dat / system.dat (keys stripped
/// of the "g." / "s." prefix, syssave/sysload).
std::string encode_domain_map(const std::map<std::string, oa::runtime::Value>& vars);
/// Decode a domain-map binary document; throws oa::util::FormatError.
std::map<std::string, oa::runtime::Value> decode_domain_map(const std::string& bytes);
/// Sanitize the project ini SAVEPATH into a clean relative prefix (empty -> "save").
std::string sanitize_savepath(const std::string& raw);

} // namespace oa::runtime

// ---------------------------------------------------------------------------
// Save-domain storage seam (moved from core/runtime/runtime_save.h): hosts/tests inject a
// SaveStore; DirSaveStore rides the shared PhysicsFS READ-WRITE mount.
// ---------------------------------------------------------------------------
namespace oa::runtime {

/// Key-value byte persistence over relative paths under the save root.
class SaveStore {
public:
    virtual ~SaveStore() = default;
    /// Write bytes (creating parent directories as needed). False on failure.
    virtual bool write(const std::string& rel_path, const std::vector<uint8_t>& data) = 0;
    /// Read bytes; nullopt when missing or unreadable.
    virtual std::optional<std::vector<uint8_t>> read(const std::string& rel_path) const = 0;
    /// Delete; false when missing/failed.
    virtual bool remove(const std::string& rel_path) = 0;
    /// Whether the path exists (save-slot checks).
    virtual bool exists(const std::string& rel_path) const { return read(rel_path).has_value(); }
    /// Whether writes actually persist somewhere (autosave gating).
    virtual bool persistent() const { return true; }
    /// Local calendar components [y,mo,d,h,mi,s] of the file's modification
    /// time (var system=file_update_time hook). nullopt when missing or the
    /// store cannot report times.
    virtual std::optional<std::array<int64_t, 6>> modification_time(
        const std::string& rel_path) const {
        (void)rel_path;
        return std::nullopt;
    }
};

/// No-op store (default): save operations run their logic but persist nothing.
/// Keeps headless runs side-effect free; hosts/tests install a real store.
class NullSaveStore final : public SaveStore {
public:
    bool write(const std::string&, const std::vector<uint8_t>&) override { return false; }
    std::optional<std::vector<uint8_t>> read(const std::string&) const override {
        return std::nullopt;
    }
    bool remove(const std::string&) override { return false; }
    bool exists(const std::string&) const override { return false; }
    bool persistent() const override { return false; }
};

/// Read-write PhysicsFS-backed store rooted at a host-chosen directory
/// (app sandbox / tmp dir in tests). `root` may be empty (store inert like
/// Null). All bytes flow through the shared PhysicsFS session.
class DirSaveStore final : public SaveStore {
public:
    explicit DirSaveStore(std::string root);
    bool write(const std::string& rel_path, const std::vector<uint8_t>& data) override;
    std::optional<std::vector<uint8_t>> read(const std::string& rel_path) const override;
    bool remove(const std::string& rel_path) override;
    bool exists(const std::string& rel_path) const override;
    std::optional<std::array<int64_t, 6>> modification_time(
        const std::string& rel_path) const override;
    const std::string& root() const { return root_; }

private:
    std::string root_;
    std::unique_ptr<oa::fs::WritableMount> mount_;
};

} // namespace oa::runtime
