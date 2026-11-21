#pragma once
// openartemis script runtime (oa::runtime) — the single public header of the .iet/.ast/.asb scenario module.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/runtime/runtime_lua.h"  // Lua host contract

namespace oa::runtime {

/// 框架/引擎脚本（不是"剧情正文脚本"）。
///
/// 两类：
///   * `system/...`（iMel/FPM 的宏、按钮、存档页）；
///   * `scenario/` **直属** 的 `.asb`（Artemis 的 asbmacro/asbscript 引擎层；
///     t13 的正文在 `scenario/main/*.asb` 子目录里）。
/// 页锚点/页状态必须是剧情脚本；落到这两类上会把读档位置带进框架宏。
inline bool is_framework_script(std::string_view name) {
    if (name.rfind("system/", 0) == 0 || name.find("/system/") != std::string_view::npos)
        return true;
    constexpr std::string_view kScenario = "scenario/";
    if (name.rfind(kScenario, 0) == 0) {
        const std::string_view rest = name.substr(kScenario.size());
        if (rest.find('/') == std::string_view::npos) {
            const size_t dot = rest.rfind('.');
            const std::string_view ext = dot == std::string_view::npos ? rest : rest.substr(dot);
            if (ext == ".asb") return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// values — Value + VariableStore
// ---------------------------------------------------------------------------// Artemis variable Value + VariableStore (value conversions, four domains,
// prefix routing).
enum class ValueKind { Int, Float, String, Bool, Null };

struct Value {
    ValueKind kind = ValueKind::Null;
    int64_t int_val = 0;
    double float_val = 0.0;
    std::string str_val;
    bool bool_val = false;

    static Value make_int(int64_t v) {
        Value vv;
        vv.kind = ValueKind::Int;
        vv.int_val = v;
        return vv;
    }
    static Value make_float(double v) {
        Value vv;
        vv.kind = ValueKind::Float;
        vv.float_val = v;
        return vv;
    }
    static Value make_string(std::string v) {
        Value vv;
        vv.kind = ValueKind::String;
        vv.str_val = std::move(v);
        return vv;
    }
    static Value make_bool(bool v) {
        Value vv;
        vv.kind = ValueKind::Bool;
        vv.bool_val = v;
        return vv;
    }
    static Value make_null() { return Value{}; }

    std::optional<int64_t> as_int() const;
    std::optional<double> as_float() const;
    bool to_bool() const;
    std::string to_string() const;
    /// Display-style formatting (concat uses Rust fmt::Display semantics).
    std::string to_display() const;
    bool operator==(const Value& o) const = default;
};

/// VariableStore: four hash domains with prefix routing:
///   "g." -> global, "t." -> temp (never persisted), "s." -> system,
///   no prefix -> local. Missing variables are absent (evaluator turns
///   absence into Int(0) itself).
class VariableStore {
public:
    using Map = std::map<std::string, Value>;

    // Routing: returns the owning map for a full variable name.
    Map* domain_of(const std::string& name);
    const Map* domain_of(const std::string& name) const;

    std::optional<Value> get(const std::string& name) const;
    void set(const std::string& name, Value value);
    void remove(const std::string& name);

    /// [reset]: clear local + temp (g./s. survive).
    void reset_local_temp();
    /// Clear every domain (hosts/tests only; the [var system=delete] tag
    /// never triggers this — a nameless delete is a no-op).
    void clear_all();

    Map local;
    Map global;
    Map temp;
    Map system;
    std::string platform; // "windows" / "android" / ... (not persisted)
};

// --------------------------------------------------------------------------- expressions — ExpressionEvaluator...
struct ExpressionError : std::runtime_error {
    explicit ExpressionError(const std::string& msg) : std::runtime_error(msg) {}
};

/// Remove '$' prefixes in front of identifier-ish chars; '$' inside string
/// literals is kept. Used on whole expressions entered from tags.
std::string normalize_expr(std::string_view expr);

class ExpressionEvaluator {
public:
    explicit ExpressionEvaluator(const VariableStore& variables) : vars_(&variables) {}

    /// Evaluate a full expression (after normalize_expr by callers).
    Value evaluate(std::string_view expr) const;

    /// resolve_param: '$'-prefixed -> evaluate; '...' literals -> String;
    /// numeric text -> Int/Float; otherwise String.
    Value resolve_param(const std::string& value) const;
    /// resolve_param_str: same but never coerces numeric-looking strings to
    /// numbers (keeps "1.80" as "1.80").
    std::string resolve_param_str(const std::string& value) const;
    /// `$$<expr>` indirect reference: evaluate <expr> to a variable NAME, then
    /// read that variable. The framework builds names dynamically
    /// ($$'g.movie_read_' + file, $$('pos_' + pos)) and also embeds the form
    /// inside larger expressions ([if estimate="$$flag != 0"]).
    Value resolve_indirect(std::string_view name_expr) const;
    /// Rewrite every embedded `$$<operand>` in an expression into the value it
    /// references (string values are quoted so they stay one token).
    std::string expand_indirect(std::string_view expr) const;

private:
    const VariableStore* vars_;
};

// ---------------------------------------------------------------------------
// instructions — Instruction + parse_params
// ---------------------------------------------------------------------------
struct ParseError : std::runtime_error {
    size_t line = 0;
    ParseError(size_t line_no, const std::string& msg)
        : std::runtime_error("script line " + std::to_string(line_no) + ": " + msg),
          line(line_no) {}
};

struct Instruction {
    /// Row kind. Story text and [lua] blocks are NOT tag rows: they carry
    /// this discriminator instead of a reserved tag name, so a script that
    /// literally contains "[__text ...]" / "[__lua_block ...]" parses them
    /// as ordinary (custom) tags and can never be confused with engine-
    /// synthesized rows.
    enum class Kind {
        Tag,      // a "[tag ...]" row; `tag` holds its script tag name
        Text,     // parser-synthesized story text (params["text"])
        LuaBlock, // parser-synthesized "[lua] ... [/lua]" block (params["code"])
        /// A [&linetag] row: `params["text"]` holds "tag p0,p1,...". The
        /// parameter COLUMN order lives in the project's tag.ini, which the
        /// parser cannot see, so the row is expanded at execution time
        /// (Interpreter::line_tag_params).
        LineTag,
    };

    Kind kind = Kind::Tag;
    std::string tag; // valid when kind == Kind::Tag; empty otherwise
    std::map<std::string, std::string> params; // insertion-independent (last wins)
    size_t line = 0;
    /// Script that was executing when this row was ENQUEUED from Lua
    std::string origin_script;
    /// 一条排队 [return] 的"地板"：它最多只能弹到排下它的那一层。
    /// 只有**事件处理会话**排的 tag 才有地板（玩家点一下按钮、宏随即发一串
    /// [return]，那一串不许把玩家正在读的剧情帧弹掉）；脚本自己 e:tag{"return"}
    /// 关 UI 时没有地板，该弹就弹。`kNoFloor` = 不设限。
    static constexpr size_t kNoFloor = static_cast<size_t>(-1);
    size_t floor_depth = kNoFloor;
    /// 这条排队 tag 来自**tag filter 内部**的 e:tag/e:enqueueTag 时置位：
    /// 过滤器用它自己把"真正的命令"重新投递一次，不能再被过滤器接住
    /// （trans 过滤器就会 `param[1]="trans"` 后重新入队）。置位的 tag 还会
    /// 由 splice_filter_emitted 原位插回队首，保持"顶替被接住的那条"的语义。
    bool skip_filter = false;

    const std::string* get(std::string_view key) const {
        const auto it = params.find(std::string(key));
        return it == params.end() ? nullptr : &it->second;
    }
    bool has(std::string_view key) const { return get(key) != nullptr; }
    std::string get_or(std::string_view key, const std::string& fallback) const {
        const std::string* v = get(key);
        return v ? *v : fallback;
    }
    /// Positional-parameter access: key "0" wins, else first param by key order.
    const std::string* get_default() const {
        if (const std::string* v = get("0")) return v;
        if (!params.empty()) return &params.begin()->second;
        return nullptr;
    }
};

/// Parse the parameter string of one tag ("k=v ..." / positional words).
/// Quoted values may contain spaces; quotes do not support escapes and an
/// unterminated quote is a ParseError. Numeric keys are stored verbatim.
std::map<std::string, std::string> parse_params(std::string_view params_str, size_t line);

// --------------------------------------------------------------------------- script text — Script...
struct Script {
    std::string name;
    std::map<std::string, size_t> labels; // label -> instruction index
    std::vector<Instruction> instructions;

    static Script parse(const std::string& name, std::string_view content);

    /// Empty label means "file start" (index 0); missing label -> nullopt.
    std::optional<size_t> get_label_line(std::string_view label) const {
        if (label.empty()) return 0;
        const auto it = labels.find(std::string(label));
        if (it == labels.end()) return std::nullopt;
        return it->second;
    }
};

// --------------------------------------------------------------------------- ASB decoder — decode_asb...
struct AsbError : std::runtime_error {
    explicit AsbError(const std::string& msg) : std::runtime_error(msg) {}
};

/// Decode bytes into text lines joined by "\n" (labels as *name, tags as
/// [name k="v" ...]). Throws AsbError on malformed data.
std::string decode_asb(const std::vector<uint8_t>& data);

// ---------------------------------------------------------------------------
// interpreter — events, waits, hooks, Interpreter
// ---------------------------------------------------------------------------
// Artemis script interpreter core: script store, call stack,
// control tags, wait model. Lua hooks are injected through InterpreterHooks.

struct ScriptError : std::runtime_error {
    size_t line = 0;
    std::string kind; // "label" | "runtime" | "script-not-found" | "lua" | ...
    ScriptError(std::string kind_, const std::string& msg, size_t line_ = 0)
        : std::runtime_error(msg), line(line_), kind(std::move(kind_)) {}
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/// 一次脚本停驻。
///
/// 旧字段（release/stage/timer_ms/...）保留为 tag 构造与探针的"规格视图"；
/// 真正决定放行的是武装时由 normalize_wait() 生成的 `wakes` 位集。一个停驻
/// 可以同时注册多个 wake（例如 click + timeout、click + trans），任一 wake
/// 先成立即结束停驻；需要清理演出的 wake（video/se/tween/trans）在放行前
/// 执行自己的 stop 动作，然后才继续脚本。
struct Wait {
    /// 阻塞式演出：完成后才放行（点击/跳过可让它提前结束）。
    enum class Stage {
        None,
        Trans,      // [trans]
        Video,      // 全屏视频
        VideoLayer, // [wait video=]
        Se,         // [wait se=]
        AllDelete,  // [alldelete]
        Tween,      // [lytween sync=1]
    };

    /// 这个停驻能被什么放行。**只有这一份**：构造时直接写，武装时
    /// normalize_wait() 只会补演出/时钟类（trans/video/timer…）。
    /// 旧的 `release` 规格位已删除 —— 两套位集是两套真值，改一处永远漏一处。
    enum Wake : uint32_t {
        WakeClick = 1u << 0,
        WakeDecide = 1u << 1,
        WakeSkip = 1u << 2,
        WakeAuto = 1u << 3,
        WakeTimer = 1u << 4,
        WakeKeys = 1u << 5,
        WakeRevealDone = 1u << 6,
        WakeRevealClear = 1u << 7,
        WakeTrans = 1u << 8,
        WakeVideo = 1u << 9,
        WakeVideoLayer = 1u << 10,
        WakeSe = 1u << 11,
        WakeAllDelete = 1u << 12,
        WakeTween = 1u << 13,
        WakeCustom = 1u << 14,
        WakeExskipEnd = 1u << 15,
    };

    // ---- 规格视图（tag 构造 / 探针 / 存档身份）----
    uint32_t wakes = WakeSkip; // 默认：跳过可以放行一切停驻
    /// 请求的计时毫秒；-1 表示没有计时器。0 是合法时长（下一帧放行）。
    /// 注意：这是**请求值**，不再在查询时递减；实际到点看 deadline_ms。
    int64_t timer_ms = -1;
    std::vector<int> keys;       // [exkey]：这些键可放行
    bool on_reveal_done = false; // [wait scenario=1]：等逐字完成
    bool reveal_clear = false;   // [wait scenario=2]：瞬显，立即继续
    bool ends_exskip = false;    // [stop 0="exskip"]：放行并结束快进

    Stage stage = Stage::None;
    std::string stage_id;
    std::string custom_name;     // [wait name=]：命名 wake（[trigger name=] 放行）
    bool se_time_given = false; // [wait se= time=]：从播放起算
    uint64_t se_time_ms = 0;

    // ---- 运行态（武装时填充）----
    std::string wake_id;          // Se / VideoLayer / Tween / Custom 的目标
    std::vector<std::string> tween_ids; // [tweenset] 组内多个 sync 目标
    uint64_t deadline_ms = 0;     // WakeTimer 的绝对到点时刻
    uint64_t armed_tick = 0;      // 本停驻武装时的帧号（时钟 wake 至少让一帧）
    uint64_t seq = 0;             // 停驻身份（自动存档/探针）
    uint64_t auto_elapsed_ms = 0; // 每个停驻独立的 auto 计时（不再跨页残留）
    uint64_t se_deadline_ms = 0;  // 无活动 SE 时惰性建立的到点时刻
    bool script_owned = true;     // false = 队列 tag 停驻（release 不推进脚本）
    /// 由调用帧恢复出来的停驻：恢复这一帧的输入已经属于（刚关闭的）UI，
    /// 不能再被它放行，否则关闭 UI 的那一下会顺手翻掉剧情页。
    bool restored = false;
    /// 这个停驻是哪个脚本位置武装的。停驻只对那一行有效：处理器发起的
    /// [jump] 把脚本挪走之后，再把它恢复回来就成了挂在错行上的"幽灵停驻"
    /// （test13 backlog 追いジャンプ：弹窗回答后每按一下都在同一行打转）。
    std::string owner_script;
    size_t owner_line = 0;
    /// A video skip already queued its finish event (the finish handler runs
    /// first and may enqueue the transition the skip should play).
    bool finish_requested = false;

    bool has(uint32_t w) const { return (wakes & w) != 0; }
    bool timed() const { return timer_ms >= 0; }
    /// 可以把输入事件标记挂在哪个停驻上：脚本停驻、点击停驻、
    /// 只受跳过控制的计时（普通计时等待本来就在跑，不需要锚点）。
    bool is_input_anchor() const {
        return is_park() || waiting_click() || (timed() && !has(WakeClick));
    }
    /// 一个"等点击"的页（用于点击提示图标）：只受点击/跳过影响的空停驻。
    bool waiting_click() const {
        return stage == Stage::None && !timed() && keys.empty() && !on_reveal_done &&
               !reveal_clear && has(WakeClick);
    }
    /// 脚本停驻（物理点击不放行）：[stop] 及其演出 gate。
    bool is_park() const {
        return stage != Stage::None ||
               (!has(WakeClick) && !reveal_clear && !on_reveal_done && !timed());
    }
};

/// `[save]` 的执行现场（**只有** save 需要）。
///
/// `[save]` 常常是框架 tag filter 里 `e:enqueueTag` 出来的：真正落盘发生在
/// 队列排空时，那时 `current_line_` 已经是**下一条指令**（iMel 的
/// `save.iet` 明确把 `[save]` 下一行写成"ロード時はここから再開"）。
/// 直接把现场记在事件上，宿主就不必再猜"这是哪一页"。
///
/// - 剧本里内联的 `[save]`：脚本自己会前进一条 ⇒ line = 当前指令 + 1；
/// - Lua `e:tag{"save"}`（FPM 的 quicksave）：当场执行 ⇒ line = 当前指令
///   （读档重新停在这个停驻上）。
struct SaveOrigin {
    std::string script;
    size_t line = 0;
    std::vector<std::pair<std::string, size_t>> call_stack;
    size_t story_call_line = 0; ///< 最内层剧情帧的 call_line（见 CallFrame）
};

struct Event {
    enum class Kind {
        ScenarioText,
        Wait_,
        Custom,
        Reset,
        GoTitle,
        Exit,
        // ---- native tag events ----
        LayerCreate,     // lyc / lyc2
        LayerDelete,     // lydel
        LayerSetProps,   // lyprop
        LayerEventCmd,   // lyevent/lytween/tweenset/anime/video...
        Trans,           // [trans]: scene transition (type 0/1/2)
        Flip,            // [flip]: commit layer changes immediately
        MessageLayerSwitch, // chgmsg
        MessageLayerPop,    // /chgmsg
        ScenarioLine,    // print
        LineBreak,       // rt
        PageBreak,       // rp
        TextConfig,      // font/fontinit/glyph/scetween/indent/...
        ConfigEvent,     // everything else (config/audio/etc, kept verbatim)
    };
    Kind kind = Kind::Wait_;
    std::string content; // ScenarioText
    bool inline_text = false;
    Wait wait; // Wait_
    std::string tag;   // Custom / TextConfig / ConfigEvent / LayerEventCmd
    std::map<std::string, std::string> params;
    std::string id;    // layer / message-layer events
    /// 仅 save：落盘要用的执行现场（见 SaveOrigin）。
    std::shared_ptr<const SaveOrigin> save_origin;

    static Event park_on(Wait w) {
        Event e;
        e.kind = Kind::Wait_;
        e.wait = std::move(w);
        return e;
    }
    static Event custom(std::string t, std::map<std::string, std::string> p) {
        Event e;
        e.kind = Kind::Custom;
        e.tag = std::move(t);
        e.params = std::move(p);
        return e;
    }
    static Event scenario_text(std::string content) {
        Event e;
        e.kind = Kind::ScenarioText;
        e.content = std::move(content);
        return e;
    }
};

enum class CallbackResult { Continue, Pause, Abort };
enum class ExecutionResult {
    Completed,
    Wait,
    /// 单次 run() 用完了每帧 tag 预算（轮询型主循环）：既没停也没结束，
    /// 下一帧从当前位置继续。宿主**不要**因此清场或算作完成。
    BudgetExhausted,
    /// 一个 [return] 弹出了携带“挂起停驻上下文”的调用帧：宿主已经把
    /// 被挂起的等待/队列尾部恢复成当前执行状态，解释器必须立即停止推进
    /// （否则会越过恢复出的停驻、把剧情直接翻到下一页）。
    ParkRestored,
};

struct CallFrame {
    std::string script;
    size_t return_line = 0;
    /// Frame identity for the host's suspended-park bookkeeping. 0 = synthetic
    /// marker (inline-event anchor, no continuation attached).
    uint64_t id = 0;
    /// 事件处理帧（原版 Artemis 里 file=="" 的那一层）。
    ///
    /// 框架的 ReturnStack()/ReturnStackAll() 通过 e:getScriptStack() 找"最顶层
    /// 的 file==\"\" 帧"来决定发不发 [return]：处理帧就是它关闭 UI 会话的把手。
    /// 这里保存的是**宿主脚本位置**（用于 [return] 弹栈后回到原行），但对外
    /// （getScriptStack）报成空文件名。
    bool synthetic = false;
    /// 由**排队 [jump]** 压出来的回程帧（框架的 `fn.push()`/UI 入口）。
    ///
    /// 原版里这类帧在 `e:getScriptStack()` 上的文件名是空的——框架的
    /// `ReturnStack()` 正是靠 `ss[#ss].file == ""` 判断"现在在 UI 流程里、
    /// 该发 [return] 把它收掉"。我们压帧时知道返回地址，但对外必须同样报空，
    /// 否则 `ReturnStack()` 什么都不做，UI 永远停在 `[stop]`（test13 存档后
    /// 回不到正文就是这个）。
    bool anon = false;
    /// 压帧时**正在执行的那条指令**（官方 BOWS frame.current）。保存动作发生
    /// 在框架脚本（save.iet）里时，剧情帧的 return_line 是"这一页之后的
    /// 指令"，直接回到那里会立刻打印下一页；官方引擎回到这一帧的 current
    /// （宏调用行），把这一页重新走一遍再停在 [@]。
    size_t call_line = 0;
};

// Host-side capability hooks (filled by the engine runtime and its Lua layer).
struct InterpreterHooks {
    /// Execute a [lua] block (called at script load; fallback when stepped over).
    std::function<void(const std::string& code, const std::string& script_name, size_t line)>
        run_lua_block;
    /// Invoke a Lua function by dotted name with resolved params. Returns
    /// false when the function does not exist (optional-callback convention).
    std::function<bool(const std::string& function,
                       const std::map<std::string, std::string>& params,
                       const std::string& script_name, size_t line)>
        call_lua_function;
    /// File loader used by load_external_script / [macroadd] / Lua file reads.
    std::function<std::optional<std::vector<uint8_t>>(const std::string&)> file_loader;
    /// `e:var(system=...)` 的**宿主数据面**。
    ///
    /// 解释器只管协议：解析 `e:var`、把结果写进变量域（`name`、`name.size`、
    /// `name.N` 这些形状由返回的写列表原样表达）。数据本身归宿主——层属性、
    /// 回放记录、消息层标签都在 runtime 里，不该在脚本 VM 的头文件上各开一个洞。
    /// 返回 nullopt = 这个名字宿主不管，解释器继续走自己的通用分支。
    using VarWrites = std::vector<std::pair<std::string, oa::runtime::Value>>;
    std::function<std::optional<VarWrites>(const std::string& system,
                                          const std::string& name,
                                          const std::map<std::string, std::string>& params)>
        system_var;
    /// Sound playback snapshot for e:var system=get_sound_info (misc-B):
    /// BGM
    /// slot + SE list sorted by id; voices riding the SE bus appear there).
    /// Resource (asset) existence/read after magic-path resolution. The
    /// runtime wires these save-area aware (names under the savepath resolve
    /// through the save store first). Absent => file_loader fallback.
    std::function<bool(const std::string& resolved)> resource_exists;
    std::function<std::optional<std::vector<uint8_t>>(const std::string& resolved)>
        resource_read;
    /// 当前消息层文本度量 (整体宽度, 总高度, 最后一行宽度)。
    /// 供 var system=get_message_layer_width/height/line_width（Lua get_fontsize
    /// 字体测量流程）。
    /// 无钩子时查询落 0。
    /// 每压一层**执行上下文**帧就通知一次（脚本 [call]、队列 [call]、可回程的
    /// [jump]）。处理会话帧不通知：它是 ReturnStack 的把手，与所在层共享上下文。
    /// 宿主把外层停驻移进上下文栈，当前层从空白开始——帧即执行上下文。
    std::function<void()> frame_pushed;
    /// 处理函数把执行跳到别的脚本时问一次：这一层有值得回程的页面停驻吗？
    /// true → 解释器压一层返回帧，让流程自己的 [return] 落回那一页。
    std::function<bool()> jump_needs_return_frame;
    /// A queued [jump file=X] executed. The framework's backlog replay jumps
    /// to the recorded script right after e:debugSkip{index=N}; the runtime
    /// notes X so the recorded instruction index is only compared inside the
    /// script it belongs to.
    std::function<void(const std::string&)> exskip_jump_target;
    /// Called before each instruction while the interpreter is at `script`:
    /// `line` is the instruction about to run. Returns true when the runtime
    /// ended a debug fast-forward here; the loop then drains the tags that
    /// end-of-replay flow queued (exskip_end) before executing the line, so
    /// the page is redrawn inside the same run instead of waiting for input.
    std::function<bool(const std::string& script, size_t line)> exskip_boundary;
    /// 每弹一层调用帧就通知一次。返回 true = 交还了一个活着的停驻，步进必须
    /// 立刻停（ExecutionResult::ParkRestored），不能再跑过这个停驻。
    std::function<bool()> frame_popped;
};

class Interpreter {
public:
    struct Config {
        std::string charset = "Shift_JIS"; // script file decoding
        std::string platform;
        int stage_width = 1280;
        int stage_height = 720;
        int fps = 60;
        std::map<std::string, std::string> env;
    };

    Interpreter();
    explicit Interpreter(const Config& config);

    // -- loading -------------------------------------------------------------
    void load_script(const std::string& name, std::string_view text);
    const Script* get_script(const std::string& name) const;

    // -- execution -----------------------------------------------------------
    /// 事件处理会话：压一层合成帧（对外 file==""）。处理函数运行期间它就在
    /// 栈顶，框架的 ReturnStack() 因此看得到"处理帧"并发 [return]。
    bool begin_handler_frame();
    /// 结束会话：把合成帧以及它上面的一切弹掉，光标回到会话入口。
    /// frame_popped 沿途交还处理函数 [call] 寄存的页面停驻。
    bool handler_return_now();
    bool in_handler_frame() const {
        return !call_stack_.empty() && call_stack_.back().synthetic;
    }
    /// 处理函数没触发 ReturnStack：会话帧还在栈顶时把它收掉。
    void end_handler_frame();

    void start(const std::string& script, std::string_view label);
    /// Boot: *main -> *start -> *_start -> file head; tries macro.iet silently.
    void boot(const std::string& script);
    /// Iterate until Completed or a Wait event (callback returned Pause).
    ExecutionResult run();
    /// Host call after a Wait resolved: advance past the wait instruction.
    void next_line();
    /// Clear the wait bookkeeping and advance the cursor only when the park
    /// was script-owned. Queue-sourced parks never move the cursor.
    void release_park(bool advance_line);

    /// Restore the execution position from a save: script (loaded on
    /// demand), line and call stack. `script` empty keeps the interpreter
    /// unpositioned.
    void restore_position(const std::string& script, size_t line,
                          const std::vector<CallFrame> stack);
    /// 读档预检用的**非抛**版本：脚本已在缓存 → true；否则按
    /// restore_position 的同一条路径载入并解析（load_external_script），失败
    /// 只把原因写进 `error`（不抛）。语义上等价于 restore_position 的第一步，
    /// 使"读档失败"能在任何清场之前判定（原子失败）。
    bool try_load_script(const std::string& file, std::string* error);
    /// tag.ini column names for one [&linetag] command (loaded lazily from
    /// the project's tag.ini). Empty when the file or the section is absent,
    /// in which case the row's columns stay positional ("1", "2", ...).
    const std::vector<std::string>& line_tag_params(const std::string& tag);
    /// 逻辑调用深度：**处理会话帧不算一层**（它是 ReturnStack 的把手，与它
    /// 所在的那层共享执行上下文）。排队 tag 的归属按这个深度算。
    size_t logical_depth() const {
        size_t d = call_stack_.size();
        for (const auto& f : call_stack_) {
            if (f.synthetic) --d;
        }
        return d;
    }
    /// 压帧：当前层的剩余 tag 存进这一帧，被调方从空队列开始。
    void save_frame_queue() {
        queue_stack_.push_back(std::move(tag_queue_));
        tag_queue_.clear();
    }
    /// 弹帧：恢复调用者的队列，并把这一帧排下的 tag 接在后面。
    ///
    /// **不能丢**：框架的宏经常"先排一整段 UI 初始化再 [return]"，那些 tag
    /// 属于机器而不是那一帧（iMel 的启动就是一次排下 600+ 条）。丢弃它们
    /// 会让 UI 再也不出现。顺序保持 FIFO：调用者的 tag 排在前面（它们本来
    /// 就先入队）。
    void restore_frame_queue() {
        std::vector<Instruction> mine;
        mine.swap(tag_queue_);
        tag_queue_ = queue_stack_.empty() ? std::vector<Instruction>{}
                                          : std::move(queue_stack_.back());
        if (!queue_stack_.empty()) queue_stack_.pop_back();
        tag_queue_.insert(tag_queue_.end(), std::make_move_iterator(mine.begin()),
                          std::make_move_iterator(mine.end()));
    }
    /// Rewrite ONLY the call stack, leaving the current position and the
    /// queue-wait/arrival flags untouched.
    void set_call_stack(std::vector<CallFrame> stack) { call_stack_ = std::move(stack); }
    /// Put the cursor on a script row without touching the call stack or the
    /// queue-wait flags. Used when a suspended park is handed back: the park
    /// belongs to the page it was armed on, and closing the UI that took it
    /// over has to put the reader back on that page — the stack has already
    /// unwound to the frame that opened the UI.
    bool set_position(const std::string& script, size_t line) {
        if (script.empty()) return false;
        if (!scripts_.count(script) && hooks_.file_loader) {
            try {
                load_external_script(script);
            } catch (const std::exception&) {
                return false;
            }
        }
        const auto it = scripts_.find(script);
        if (it == scripts_.end()) return false;
        if (line > it->second.instructions.size()) return false;
        current_script_name_ = script;
        current_line_ = line;
        return true;
    }
    /// Temporarily detach / re-queue pending engine tags (used by the save
    /// domain to flush only the tags an onSave/onLoad handler generated).
    /// restore_tag_queue re-queues the taken items BEHIND whatever the flushed
    /// handler left queued (generated-leftovers-first order).
    std::vector<Instruction> take_tag_queue();
    void restore_tag_queue(std::vector<Instruction> items);
    size_t queued_tag_count() const { return tag_queue_.size(); }
    /// 未实现的指令（标签名 → 出现过几次）。宿主拿它提示玩家/写日志：
    /// 静默吞掉会变成"页面出来了但没字""按钮点了没反应"这种哑故障。
    const std::map<std::string, int>& unknown_tags() const { return unknown_tags_; }
    void note_unknown_tag(const std::string& tag) { ++unknown_tags_[tag]; }
    // ------------------------------------------------------------------
    // 已读行（既読）——skip 的既读判定（s.status.alreadyread）与已读文字色。
    //
    // 键 = 脚本名，值 = 打印过的行号集合。原版把这个表存在全局档（BOWG 的
    // read_lines：脚本 → 已读行），这里同构：宿主负责把它落到 g.readlines。
    // note_text_line 返回"这一行之前读过吗"，并在同一拍记入。
    // ------------------------------------------------------------------
    bool note_text_line(const std::string& script, size_t line);
    /// 当前脚本行是正文：判既读 → 写 s.status.alreadyread → 记入表。
    /// [print] 标签与 Text 行（.iet 的裸文本）两条路都走这里。
    void note_text_position();
    bool has_read_line(const std::string& script, size_t line) const;
    const std::map<std::string, std::set<uint32_t>>& read_lines() const {
        return read_lines_;
    }
    void set_read_lines(std::map<std::string, std::set<uint32_t>> lines);
    void merge_read_lines(const std::map<std::string, std::vector<uint32_t>>& lines);
    /// Put a parked script tail back in front of the queue (a released park's
    /// deferred tags run before anything enqueued while it was live).
    void prepend_tags(std::vector<Instruction> items) {
        if (items.empty()) return;
        tag_queue_.insert(tag_queue_.begin(), items.begin(), items.end());
    }

    /// Queue an engine tag the interpreter executes from its tag queue (the
    /// drain that Lua e:enqueueTag uses; the runtime enqueues lytween
    /// completion handlers through it).
    void enqueue_tag(std::string tag, std::map<std::string, std::string> params);
    /// 把 tag filter 在本次调用里重新投递的命令插回队首（保持投递顺序）。
    /// 过滤器是"原位替换"：它排下的真命令要顶替被它接住的那条，不能排到
    /// 调用者剩余队列之后（sysbtn 的子菜单：队列里还排着别的 tag，转场跑到
    /// 它们后面就会打乱标题/菜单流程）。
    void splice_filter_emitted();
    /// Whether the tag queue holds pending tags.
    bool has_queued_tags() const { return !tag_queue_.empty(); }
    /// Macro registry: [macroadd file=F] registers every label of F as a
    /// callable tag (`[btn ...]` -> `[call file=F label=btn]`), the iMel
    /// framework's whole UI layer (button.iet/msg.iet/chara_*.iet).
    const std::map<std::string, std::string>& macros() const { return macros_; }
    void add_macro(const std::string& label, const std::string& file) {
        macros_[label] = file;
    }
    void remove_macros_of_file(const std::string& file) {
        for (auto it = macros_.begin(); it != macros_.end();) {
            if (it->second == file) it = macros_.erase(it);
            else ++it;
        }
    }
    /// Macro parameter scope snapshot (see macro_param_saves_).
    struct MacroParamSave {
        size_t depth = 0;
        std::map<std::string, std::optional<Value>> saved;
        /// Parameter names THIS call supplied. Artemis gives every macro call
        /// its own local frame: an outer call's parameter must not look like a
        /// local variable inside a nested macro that did not pass it
        /// ([set_functionkeys] with no mode used to see an enclosing
        /// selectbtn's mode=1 and jump to registration_functionkeys_1).
        std::set<std::string> provided;
        /// Names written with writelocal=1 while this frame was active
        /// (frame-local temporaries the script itself declares).
        std::set<std::string> written;
    };
    void push_macro_param_save(MacroParamSave save) {
        macro_param_saves_.push_back(std::move(save));
    }
    /// Undo the parameter variables of the macro frame that just returned
    /// (depth = call stack depth BEFORE the pop).
    void restore_macro_params(size_t depth);
    /// `[var system=var_exist local=1]`: is `name` local to the CURRENT macro
    /// frame (parameter or writelocal declaration)? Outer frames' parameters
    /// and stale leftovers of returned frames do not count.
    bool local_var_declared(const std::string& name) const;
    /// Remember a writelocal=1 write in the innermost macro frame.
    void note_local_write(const std::string& name);
    /// First `n` queued tags in FIFO order (diagnostics only).
    std::vector<Instruction> peek_tag_queue(size_t n) const {
        std::vector<Instruction> out;
        for (size_t i = 0; i < n && i < tag_queue_.size(); ++i)
            out.push_back(tag_queue_[i]);
        return out;
    }
    /// Drain ONLY the queued tags (no inline script stepping). Returns Wait
    /// when a queued tag paused the interpreter (parked-wait drains
    /// run queued tags only, no inline stepping); Completed when the queue
    /// emptied without a
    /// pause. Queued jump/call/return tags change the interpreter position.
    ExecutionResult run_queued();
    /// Queued-drain shape flags:
    /// saw_call/saw_jump set while run_queued executes one queued tag of
    /// that kind; consumed by the runtime's inline-event-frame settle after
    /// each drain round.
    bool queued_saw_call() const { return queued_saw_call_; }
    bool queued_saw_jump() const { return queued_saw_jump_; }

    // -- state ---------------------------------------------------------------
    const std::string* current_script() const;
    size_t current_line() const { return current_line_; }
    const std::vector<CallFrame>& call_stack() const { return call_stack_; }
    /// 玩家当前看到的那一页的位置（存档恢复点）。
    const std::string& stable_script() const { return stable_script_; }
    size_t stable_line() const { return stable_line_; }
    /// Lua `tags.<tag>` 处理器作用域：这期间 `e:tag{"return"}` 立即弹帧。
    void enter_lua_tag_handler() { in_lua_tag_handler_ = true; }
    void leave_lua_tag_handler() { in_lua_tag_handler_ = false; }

    /// 这一页的**脚本起点**（第一条源码指令 + 调用栈）。
    ///
    /// 存档动作发生在框架脚本（save.iet）里时，精确位置是"存档页的收尾"；
    /// 直接回到它，剧情帧已经越过了这一页的 [@]。官方 BOWS 把帧的
    /// current/pending 一起存下来；本引擎只存**这一页的源码起点**：读档后
    /// 脚本把这一页重放一遍，自然停在页尾的 [@]（画面/文本由存档的
    /// surface 恢复，页前的 [bg]/[chgmsg] 不需要重跑）。
    ///
    /// 只记**新页第一段文字**之前的那条源码行；不存任何画面快照。
    void note_page_start(std::string_view layer = {}) {
        if (!current_script_name_) return;
        const auto is_system = [](const std::string& s) {
            return is_framework_script(s);
        };
        std::string script = *current_script_name_;
        size_t line = current_line_;
        std::vector<CallFrame> stack = call_stack_;
        if (is_system(script)) {
            for (size_t i = call_stack_.size(); i-- > 0;) {
                const CallFrame& f = call_stack_[i];
                if (f.script.empty() || is_system(f.script)) continue;
                script = f.script;
                line = f.call_line ? f.call_line : (f.return_line > 0 ? f.return_line - 1 : 0);
                stack.assign(call_stack_.begin(), call_stack_.begin() + i);
                break;
            }
        }
        if (is_system(script)) return;
        // 退到同一源码行的首指令（[lang] / [font] 这类前置 tag 一起重放）。
        if (const Script* s = get_script(script)) {
            if (line < s->instructions.size()) {
                const size_t src = s->instructions[line].line;
                while (line > 0 && s->instructions[line - 1].line == src) --line;
            }
        }
        page_start_script_ = std::move(script);
        page_start_line_ = line;
        page_start_stack_ = std::move(stack);
        page_start_valid_ = true;
        ++page_start_serial_;
    }
    bool page_start_valid() const { return page_start_valid_; }
    uint64_t page_start_serial() const { return page_start_serial_; }
    const std::string& page_start_script() const { return page_start_script_; }
    size_t page_start_line() const { return page_start_line_; }
    const std::vector<CallFrame>& page_start_stack() const { return page_start_stack_; }
    /// 这一页的**重放起点** = 产生页首文字的那条源码行的第一条指令。
    /// 只在新页第一段文字时调用（宿主在 push_text 之前判断）。
    void note_page_printed(std::string_view layer = {}) {
        if (!current_script_name_) return;
        const auto is_system = [](const std::string& s) {
            return is_framework_script(s);
        };
        std::string script = *current_script_name_;
        size_t line = current_line_;
        std::vector<CallFrame> stack = call_stack_;
        if (is_system(script)) {
            for (size_t i = call_stack_.size(); i-- > 0;) {
                const CallFrame& f = call_stack_[i];
                if (f.script.empty() || is_system(f.script)) continue;
                script = f.script;
                line = f.call_line ? f.call_line : (f.return_line > 0 ? f.return_line - 1 : 0);
                stack.assign(call_stack_.begin(), call_stack_.begin() + i);
                break;
            }
        }
        if (is_system(script)) return;
        if (const Script* s = get_script(script)) {
            if (line < s->instructions.size()) {
                const size_t src = s->instructions[line].line;
                while (line > 0 && s->instructions[line - 1].line == src) --line;
            }
        }
        page_anchor_script_ = std::move(script);
        page_anchor_line_ = line;
        page_anchor_stack_ = std::move(stack);
        page_anchor_layer_.assign(layer);
        page_anchor_valid_ = true;
        // 页首的变量（含 t.*）：存档发生在 save.iet 里时用它，避免把存档页
        // 的 UI scratch（t.sysbtn_submenu / backlog.num / meswindow.*）写进档。
        page_start_vars_ = variables_.local;
        for (const auto& [k, v] : variables_.temp) page_start_vars_[k] = v;
    }
    const std::map<std::string, Value>& page_start_vars() const { return page_start_vars_; }
    bool page_anchor_valid() const { return page_anchor_valid_; }
    const std::string& page_anchor_script() const { return page_anchor_script_; }
    size_t page_anchor_line() const { return page_anchor_line_; }
    const std::vector<CallFrame>& page_anchor_stack() const { return page_anchor_stack_; }
    const std::string& page_anchor_layer() const { return page_anchor_layer_; }
    void clear_page_anchor() { page_anchor_valid_ = false; }
    /// 停驻这一刻记位置（诊断/稳定停驻查询）。
    void note_stable_park() {
        if (!current_script_name_) return;
        stable_script_ = *current_script_name_;
        stable_line_ = current_line_;
    }
    /// 无 file 的 `[call label]` / `[jump label]` 的标签解析规则。
    ///
    /// 命名空间 = **当前脚本 + 框架脚本**（`system/*` 与 `scenario/*.asb`
    /// 这类框架层，见 is_framework_script）。iMel 的 `config_s.iet` 顶部
    /// 写着"config.iet 必須"，并直接 `[call label="mw_reprop"]` /
    /// `delete_config_sample_mes` —— 这些标签只存在于 config.iet，属于框架
    /// 自己的**跨页命名空间**，不是"随手到处找"。
    ///
    /// 不搜索剧情脚本：剧情文件缺标签仍是作者错误，不能静默跳到别的剧本。
    /// 顺序固定：当前脚本 → 框架脚本（按文件名稳定排序）。
    bool resolve_label(const std::string& label, std::string* file, size_t* line) const;
    /// 以 `context` 为发起脚本的同一套解析（排队 tag 用 origin_script）。
    bool resolve_label_from(const std::string& context, const std::string& label,
                            std::string* file, size_t* line) const;
    VariableStore& variables() { return variables_; }
    const VariableStore& variables() const { return variables_; }
    void set_variable(const std::string& name, Value v) { variables_.set(name, std::move(v)); }
    const Config& config() const { return config_; }

    // -- Lua (created lazily; one shared VM) ----------------------------------
    oa::runtime::LuaBridge& lua_bridge();
    /// Apply a [var ...] tag (simple + system= subset); used by the var tag
    /// and by Lua e:tag{"var",...}. Returns false for unknown system= keys.
    bool apply_var(const std::map<std::string, std::string>& params);
    /// Fire a registered event handler (e.g. onEnterFrame -> Lua vsync).
    bool fire_event(const std::string& event_name);
    /// 带参数触发框架注册的事件处理器（`e:setEventHandler{ onSave = "store" }`
    /// / `onLoad = "restore"` / iMel 的 `setOnSave`/`setOnLoad`）。处理器签名
    /// 与 `calllua` 相同：(e, param)。这是框架**声明自己要存什么/恢复什么**
    /// 的唯一入口 —— 引擎不再硬编码 `store`/`restore` 这两个名字。
    bool fire_event(const std::string& event_name,
                    const std::map<std::string, std::string>& params);
    /// `e:tag{...}` from Lua executes the tag NOW (Artemis semantics), unlike
    /// `e:enqueueTag` which defers. Only presentation tags are accepted here;
    /// control-flow outcomes return false so the caller can queue the tag and
    /// let the main loop own the redirect. Re-entrant (an [lyc] filter
    /// re-issuing [lyc]) up to a bounded depth, then falls back to the queue.
    bool run_tag_now(const std::string& tag, std::map<std::string, std::string> params);

    /// A scenario text row is `[print data="<row>"]` to the framework side:
    /// hand it to the registered tagFilters.print (the iMel multi-language
    /// layer lives there — it picks the text of the active [lang xx] block and
    /// re-issues e:tag{"print", data=…}). Returns true when the filter took
    /// ownership (the row must not be emitted as an engine text event).
    bool dispatch_text_row_to_print_filter(const std::string& text);

    /// e:getScriptBlock backing: tag row at script : instruction index
    /// (command/parameter/源码行号). nullopt when the script is unknown or the
    /// index is out of range.
    std::optional<oa::runtime::LuaHost::ScriptBlock>
    script_block(const std::string& file, size_t index) const;
    /// Resolve a logical file name through the magic-path table
    /// (e:setMagicPath, ":bg/" prefixes etc).
    std::string resolve_magic_path(const std::string& name) const;

    // -- wiring --------------------------------------------------------------
    const InterpreterHooks& hooks() const { return hooks_; }
    InterpreterHooks& hooks() { return hooks_; }
    void set_callback(std::function<CallbackResult(const Event&)> cb) {
        callback_ = std::move(cb);
    }
    /// 立即执行的 tag（`e:tag`）里宿主要求停驻的事件。
    ///
    /// 我们的立即执行路径（run_tag_now）只把事件派发给宿主，**丢掉了 Pause
    /// 返回值** —— 影片于是脱离脚本：没有视频停驻，后面的 `[wt]` 又只是普通
    /// 点击等待，点击放开 `[wt]` 后影片继续盖在画面上（"跳过依旧占据屏幕"）。
    /// 这里把宿主的裁决留下来，由 run()/队列排空在下一个边界转成 Wait。
    std::optional<Event> pending_immediate_park_;
    /// run() 的嵌套深度：只有在脚本流里才接住 Pause（UI 回调里的事件由
    /// 派发方自己处理停驻，不能在这里重复建）。
    int run_depth_ = 0;
    /// 有挂起的立即停驻就转成一次 Wait（并记账 last_wait_event_）。
    std::optional<ExecutionResult> take_immediate_park();

    /// The Wait event that made run return Wait (cleared on next_line).
    const Event* last_wait_event() const { return last_wait_event_.has_value() ? &*last_wait_event_ : nullptr; }
    /// Whether the current wait came from the tag queue (next_line no-op).
    bool last_wait_from_queue() const { return last_wait_from_queue_; }
    /// Diagnostics: called for every executed instruction (inline + queued).
    std::function<void(const std::string& script, size_t line, const Instruction& ins)>
        on_step;
    /// Engine-registered tags (override builtin semantics, e.g. [reset]).
    /// Returned event is emitted like a tag Emit; nullopt means Continue.
    using EngineTagFn =
        std::function<std::optional<Event>(Interpreter&, const Instruction&)>;
    void register_engine_tag(const std::string& name, EngineTagFn fn) {
        engine_tags_[name] = std::move(fn);
    }
    const std::map<std::string, EngineTagFn>& engine_tags() const { return engine_tags_; }

private:
    Config config_;
    std::map<std::string, Script> scripts_;
    /// tag.ini: command -> ordered parameter names ([&linetag] columns).
    std::map<std::string, std::vector<std::string>> line_tags_;
    bool line_tags_loaded_ = false;
    std::optional<std::string> current_script_name_;
    size_t current_line_ = 0;
    /// 打印过的行（脚本 → 行号）；见 note_text_line。
    std::map<std::string, std::set<uint32_t>> read_lines_;
    /// 最近一次停驻的位置（诊断/框架查询用）。
    ///
    /// **存档不再用它**：存档保存的是执行现场本身（`[save]` 事件带
    /// SaveOrigin；宿主自动存档用当前停驻）。早期把它当"恢复点"，结果每种
    /// 框架都要一套不同的修正，正是本轮删掉的复杂度。
    std::string stable_script_;
    size_t stable_line_ = 0;
    /// 这一页的脚本起点（见 note_page_start；不存画面）。
    std::string page_start_script_;
    size_t page_start_line_ = 0;
    std::vector<CallFrame> page_start_stack_;
    bool page_start_valid_ = false;
    uint64_t page_start_serial_ = 0;
    std::string page_anchor_script_;
    size_t page_anchor_line_ = 0;
    std::vector<CallFrame> page_anchor_stack_;
    std::string page_anchor_layer_;
    bool page_anchor_valid_ = false;
    std::map<std::string, Value> page_start_vars_;
    /// 正在执行 Lua 侧的 `tags.<tag>` 处理器（`[load_broken]` 这类）。
    /// 这期间 `e:tag{"return"}` 必须**立即**弹帧（原版 e:tag 的语义）：
    /// 排进队列的话，处理器随后排的 `[call ...]` 会把回程点记在当前（UI）
    /// 脚本上，收尾链就停死在 ui.asb 的 `[stop]`（test13 存档后点不动）。
    bool in_lua_tag_handler_ = false;
    std::vector<CallFrame> call_stack_;
    bool arrived_by_jump_ = false;
    bool last_wait_from_queue_ = false;
    bool queued_saw_call_ = false;
    bool queued_saw_jump_ = false;
    /// **队列归帧**：第 i 层（0 = 剧本顶层）还没跑完的 tag。当前层是
    /// `tag_queue_`；压帧时当前层的队列存进这里，弹帧时取回。
    ///
    /// 这一条替换了旧的 `queue_hold_depth_`：队列 [call] 之后的 tag 属于调用者，
    /// 它们留在调用者那一层，被调方看不到，也就不需要"顶住排空"的全局状态。
    /// tag 永远在它被排下的那一层排空，不会再拿被调方的脚本解析 label。
    std::vector<std::vector<Instruction>> queue_stack_;
    int immediate_tag_depth_ = 0;
    // Call-frame identity source (see CallFrame::id).
    uint64_t next_frame_id_ = 1;
    // Macro tag -> defining file ([macroadd]); see macros().
    std::map<std::string, std::string> macros_;
    // Macro parameter scope: tag params become plain variables for the
    // duration of the macro call; previous values are restored on [return].
    std::vector<MacroParamSave> macro_param_saves_;
    VariableStore variables_;
    InterpreterHooks hooks_;
    std::function<CallbackResult(const Event&)> callback_ =
        [](const Event&) { return CallbackResult::Continue; };
    std::map<std::string, EngineTagFn> engine_tags_;
    std::map<std::pair<std::string, size_t>, bool> executed_lua_blocks_;
    // queue of engine tags enqueued from Lua (drained before inline steps)
    std::vector<Instruction> tag_queue_;
    /// 引擎不认识的指令（名字 → 次数）。宿主负责提示，别静默吞。
    std::map<std::string, int> unknown_tags_;
    /// 当前 tag filter 调用里通过 e:tag/e:enqueueTag 重投递的命令（原位插回）。
    std::vector<Instruction> filter_emitted_;
    // Lua-side event handlers: event name -> global function name
    std::map<std::string, std::string> event_handlers_;
    // e:getScriptWaitReason backing store
    std::map<std::string, std::string> wait_reason_info_;
    std::optional<Event> last_wait_event_;
    std::unique_ptr<oa::runtime::LuaBridge> lua_bridge_;
    std::map<std::string, std::string> magic_paths_;

    friend struct LuaBootstrap;
    // internals ------------------------------------------------------------
    /// Load one scenario from bytes: text goes straight to Script::parse,
    /// ".asb" bytes through decode_asb. Internal (used only by
    /// load_external_script below).
    void load_file(const std::string& name, const std::vector<uint8_t>& bytes);
    /// Load a scenario file through InterpreterHooks::file_loader (decoding
    /// .asb bytes through load_file/decode_asb). Internal: hosts reach
    /// scenarios via boot()/start()/the file_loader hook only ("small
    /// interface").
    void load_external_script(const std::string& file);
    std::optional<ExecutionResult> flush_tag_queue();
    /// Sync metric vars read the active layer, but FPM writes them
    /// right after queueing a chgmsg/rp/print segment (uihelp centering,
    /// get_fontsize, line/backlog measuring). Run the queued message-text
    /// tags ahead of the query so it measures the intended layer; stops at
    /// the first non-text tag (waits/control never pre-execute).
public:
    /// 把排队中的文本标签立即落地（`e:var system=get_layer_info` 之类的
    /// 宿主查询要在"这一帧的写入"之后读，否则拿到上一帧的值）。
    void flush_pending_text_tags();

private:
    void note_wait_reason(const Event& event);
    const oa::runtime::Value* lookup_var_ptr(const std::string& name) const;
    oa::runtime::LuaHost build_lua_host();
    uint64_t now_ms() const;
    void seed_engine_variables_impl();
};

} // namespace oa::runtime
