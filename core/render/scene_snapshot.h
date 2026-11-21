#pragma once
// oa::render — 存档里的场景/文本面（纯数据，不含运行时 Value）。
//
// 只服务**存档本身**（`SaveData` 的 scene/text 段，等价于官方 BOWS 的
// 字段 26/24/25）。回顾跳转**不**再持有每页一份的画面：框架有自己的载体
// （t10 的 `backlog_N.dat`、t13 的 `log.stack[].scr`、NekoMiko 的 `blj`），
// 跳转 = 载入那份载体 + 脚本重放/快进。
//
// 放在 render 层是为了让 TextEngine 能直接持有存档里的文本排版，而不必把
// 整个 runtime::SaveData（Value/音频/文件格式）拖进渲染层。
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace oa::render {

/// 层事件行（[lyevent]）。恢复时逐字段写回，不能只留 handler 名。
struct LayerEventHandlerSnap {
    std::string type; // "click" / "rollover" / "rollout" / "dragin" / ...
    bool enabled = true;
    bool penetration = false;
    std::string handler;
    std::string file;
    std::string label;
    bool call = false;
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> filter_params;
};

/// 一个可见层的完整属性（id + props + 事件行）。
struct LayerSnap {
    std::string id;
    std::map<std::string, std::string> props;
    std::vector<LayerEventHandlerSnap> handlers;
};

/// 一个消息层的可见页。文本引擎的**排版流**是运行时结构，不属于场景；
/// 快照只保存重建这一页所必需的：layered / 绘制盒 / 字体 / 展平文本。
struct TextLayerSnap {
    std::string id;
    bool active = false;
    bool positioned = false;
    /// `[chgmsg layered=1]` 建的消息场景节点必须按原值恢复。
    /// -1 = 旧档没有这个键，恢复时不动当前值。
    int layered = -1;
    double left = 0, top = 0, width = 0, height = 0;
    std::map<std::string, std::string> font; // FontDesc::raw (box + face + size…)
    std::string text;                        // flattened page, '\n' = line break
    bool revealed = true;                    // fully revealed when saved
};

} // namespace oa::render
