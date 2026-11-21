#include "core/runtime/runtime_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "core/emote/emote_player.h"
#include "core/fs/physfs_fs.h"
#include "core/fs/project.h"
#include "core/render/renderer.h"
#include "core/runtime/runtime.h" // App (host contract)
#include "core/runtime/runtime_save.h"
#include "core/version.h"

extern "C" {
#include "lua.h"
}

namespace oa::runtime {

namespace {
// virtual key codes used by Artemis scripts
constexpr int kKeyMouseLeft = 1;
constexpr int kKeyMouseRight = 2;
constexpr int kKeyEnter = 13;
// Short name for the renderer's frame-dirty tags (renderer.h Note).
using RNote = oa::render::RenderEngine::Note;
constexpr int kKeySpace = 32;

// Script param -> int, `dflt` when the text is not a full integer (the one
// file-local copy for every domain in this TU; the former
// parse_i32 / parse_i32_dflt pair had identical bodies).
int parse_i32(const std::string& v, int dflt) {
    try {
        return std::stoi(v);
    } catch (...) {
        return dflt;
    }
}

// Layer-prop number text for the [lyprop] readback and the click-wait icon
// homing: integral values print without a decimal point, others through %g
// (the two byte-identical lambdas that each defined it are merged).
std::string format_prop_num(double v) {
    char buf[64];
    const double iv = std::floor(v);
    if (v == iv) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%g", v);
    }
    return std::string(buf);
}

// Header-only still-image dimension sniffing (PNG IHDR, JPEG
// SOFn) for the get_layer_info natural-size fallback. Decoding whole frames
// just to answer a width query would be wasteful (and potentially huge for
// bg/fg textures); the reference engine's UI rows resolve their size from
// the bound image, and only these two formats exist in Artemis UI data.
bool sniff_image_dims(const std::vector<uint8_t>& b, double* w, double* h) {
    *w = 0;
    *h = 0;
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (b.size() >= 24 && std::memcmp(b.data(), kPngSig, 8) == 0 && b[12] == 'I' &&
        b[13] == 'H' && b[14] == 'D' && b[15] == 'R') {
        const uint32_t W = (uint32_t(b[16]) << 24) | (uint32_t(b[17]) << 16) |
                           (uint32_t(b[18]) << 8) | uint32_t(b[19]);
        const uint32_t H = (uint32_t(b[20]) << 24) | (uint32_t(b[21]) << 16) |
                           (uint32_t(b[22]) << 8) | uint32_t(b[23]);
        if (W != 0 && H != 0 && W <= 65536 && H <= 65536) {
            *w = double(W);
            *h = double(H);
            return true;
        }
        return false;
    }
    if (b.size() >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < b.size()) {
            if (b[i] != 0xFF) return false;
            const uint8_t m = b[i + 1];
            if (m == 0xD8 || m == 0x01) {  // SOI / TEM: no length
                i += 2;
                continue;
            }
            if (m >= 0xD0 && m <= 0xD7) {  // RSTn: no length
                i += 2;
                continue;
            }
            if (m == 0xD9 || m == 0xDA) return false;  // EOI / SOS: no SOF before
            if (i + 4 > b.size()) return false;
            const uint16_t len = (uint16_t(b[i + 2]) << 8) | uint16_t(b[i + 3]);
            if (len < 2) return false;
            // SOF0-SOF3 / SOF5-SOF7 / SOF9-SOF11 / SOF13-SOF15 (exclude the
            // non-frame tables DHT/DAC/DRI/APPn/COM)
            const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
            if (sof) {
                if (i + 9 > b.size()) return false;
                const uint16_t H = (uint16_t(b[i + 5]) << 8) | uint16_t(b[i + 6]);
                const uint16_t W = (uint16_t(b[i + 7]) << 8) | uint16_t(b[i + 8]);
                if (W != 0 && H != 0) {
                    *w = double(W);
                    *h = double(H);
                    return true;
                }
                return false;
            }
            i += 2 + size_t(len);
        }
    }
    return false;
}
} // namespace

// GameRuntime public forwarding.
//     实现面（状态 + 逻辑）全在 runtime_internal.h 的 RuntimeState；本类只持有
//     s_ 并把每个公开方法转给它的同名实现。公开签名与 runtime.h 逐字一致，
//     因此 56 个调用方一行不改。
// ===========================================================================
GameRuntime::RuntimeState::RuntimeState(GameRuntime* rt,
                                       std::shared_ptr<const oa::fs::IFileSystem> fs)
    // 初始化列表照搬原 GameRuntime 构造函数；顺序 = 声明顺序。
    : rt_(rt),
      fs_(std::move(fs)),
      renderer_(fs_.get(), static_cast<oa::render::RenderHost*>(rt), false)
{
    save_.save_store_ = std::make_shared<oa::runtime::NullSaveStore>();
    media_.media_players_ = std::make_unique<oa::media::MediaPlayers>();
    set_hit_providers(oa::render::RenderEngine::hit_size,
                      oa::render::RenderEngine::alpha_sampler, &renderer_);
    set_pointer_warp_callback([this](int sx, int sy) {
        renderer_.warp_mouse_to_stage(float(sx), float(sy));
    });
    set_frame_capture([this](uint32_t* w, uint32_t* h) {
        return renderer_.capture_frame(w, h);
    });
}

GameRuntime::RuntimeState::~RuntimeState()
{
    renderer_.release_all();
    // Stop the video driver and cancel decode pipes BEFORE the audio sink and
    // the decode pool are torn down: the driver thread calls the movie-audio
    // hook and the pool workers, so releasing either first is a use-after-free.
    media_.video_.stop_all_videos();
    media_.video_.stop_driver();
    media_.video_.set_audio_output(nullptr);
    media_.video_.set_audio_flush(nullptr);
    media_.media_players_->print_audio_diag("run");
    media_.media_players_->release();
}

bool GameRuntime::RuntimeState::attach_window(void* native_window,
                                              const std::string& backend_kind,
                                              bool with_audio)
{
    if (!renderer_.create_renderer(native_window, backend_kind)) return false;
    oa::emote::emote_set_default_external_pose(true);
    media_.media_players_->init(with_audio);
    media_.video_.set_audio_output([mp = media_.media_players_.get()](const float* d, size_t n) {
        mp->push_video_audio(d, n);
    });
    media_.video_.set_audio_flush([mp = media_.media_players_.get()]() {
        mp->flush_video_audio();
    });
    // 视频源自己解码并 update 自己的纹理；纹理挂到层上（driver 线程入队，
    // 主线程落场景，render 只负责画层）。
    media_.video_.set_texture_factory(
        [this](int w, int h) { return renderer_.create_host_texture(w, h); });
    media_.video_.set_frame_push(
        [this](const std::string& channel, const oa::media::TextureRef& tex,
               int, int) {
            const std::string id = channel.empty()
                                       ? std::string(oa::render::kOverlayNodeId)
                                       : channel;
            queue_host_texture(id, tex);
        });
    return true;
}

std::string GameRuntime::RuntimeState::resolve_magic_path(
    const std::string& name) const
{
    return interpreter_ ? interpreter_->resolve_magic_path(name) : name;
}

std::optional<std::string> GameRuntime::RuntimeState::host_variable(
    const std::string& key) const
{
    if (!interpreter_) return std::nullopt;
    const auto v = interpreter_->variables().get(key);
    if (!v) return std::nullopt;
    return v->to_string();
}

std::optional<std::vector<uint8_t>> GameRuntime::RuntimeState::read_save_file(
    const std::string& rel_path) const
{
    const std::string& sp = save_.savepath_;
    if (sp.empty() || rel_path.empty()) return std::nullopt;
    if (!(rel_path == sp ||
          (sp.size() < rel_path.size() && rel_path.rfind(sp + "/", 0) == 0)))
        return std::nullopt;
    if (!save_.save_store_) return std::nullopt;
    return save_.save_store_->read(rel_path);
}

bool GameRuntime::has_text_metrics() const {
    return s_ && static_cast<bool>(s_->text_metrics);
}

void GameRuntime::RuntimeState::bind_text_metrics(
    std::function<std::tuple<double, double, double>()> fn)
{
    if (!fn) return;
    text_metrics = std::move(fn);
}

namespace {
// e:var(system="file_update_time") 的日期渲染（yyyy/yy/MM/dd/hh/mm/ss，
// 单字母不补零）。数据面归 runtime，解释器不再持有这张格式表。
std::string format_update_time(const std::array<int64_t, 6>& c, const std::string& format) {
    std::string out;
    size_t i = 0;
    const auto pad2 = [](int64_t v) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02lld", (long long)v);
        return std::string(buf);
    };
    while (i < format.size()) {
        const char ch = format[i];
        size_t run = 1;
        while (i + run < format.size() && format[i + run] == ch) ++run;
        switch (ch) {
            case 'y':
                if (run >= 4) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%04lld", (long long)c[0]);
                    out += buf;
                    i += run;
                } else if (run >= 2) {
                    out += pad2(c[0] % 100);
                    i += run;
                } else {
                    out += ch;
                    i += 1;
                }
                break;
            case 'M':
                out += (run >= 2) ? pad2(c[1]) : std::to_string(c[1]);
                i += (run >= 2) ? run : 1;
                break;
            case 'd':
                out += (run >= 2) ? pad2(c[2]) : std::to_string(c[2]);
                i += (run >= 2) ? run : 1;
                break;
            case 'h':
                out += (run >= 2) ? pad2(c[3]) : std::to_string(c[3]);
                i += (run >= 2) ? run : 1;
                break;
            case 'm':
                out += (run >= 2) ? pad2(c[4]) : std::to_string(c[4]);
                i += (run >= 2) ? run : 1;
                break;
            case 's':
                out += (run >= 2) ? pad2(c[5]) : std::to_string(c[5]);
                i += (run >= 2) ? run : 1;
                break;
            default:
                out += ch;
                i += 1;
                break;
        }
    }
    return out;
}

} // namespace

GameRuntime::GameRuntime(std::shared_ptr<const oa::fs::IFileSystem> fs)
    : s_(std::make_unique<RuntimeState>(this, std::move(fs))) {}

GameRuntime::~GameRuntime() = default;

// ---- 公开方法：逐个转发 ----------------------------------------------------

void GameRuntime::open_project(std::string_view platform) { s_->open_project(platform); }
void GameRuntime::boot_project() { s_->boot_project(); }
void GameRuntime::tick(uint64_t delta_ms, const FrameInput& input) { s_->tick(delta_ms, input); }
void GameRuntime::tick(uint64_t delta_ms) { s_->tick_input(delta_ms); }
void GameRuntime::key_event(int vk, bool down) { s_->key_event(vk, down); }
void GameRuntime::mouse_move(float window_x, float window_y) { s_->mouse_move(window_x, window_y); }
void GameRuntime::mouse_button(int vk, bool down) { s_->mouse_button(vk, down); }
void GameRuntime::mouse_wheel(int direction) { s_->mouse_wheel(direction); }
bool GameRuntime::exit_requested() const { return s_->exit_requested(); }
std::vector<oa::runtime::Event> GameRuntime::drain_events() { return s_->drain_events(); }
oa::runtime::Interpreter& GameRuntime::interpreter() { return s_->interpreter(); }
const oa::runtime::Interpreter& GameRuntime::interpreter() const { return s_->interpreter(); }
const std::shared_ptr<const oa::fs::IFileSystem>& GameRuntime::fs() const { return s_->fs(); }
bool GameRuntime::attach_window(void* native_window, const std::string& renderer_kind,
                                bool with_audio) {
    return s_->attach_window(native_window, renderer_kind, with_audio);
}
std::string GameRuntime::resolve_magic_path(const std::string& name) const {
    return s_->resolve_magic_path(name);
}
std::optional<std::string> GameRuntime::host_variable(const std::string& key) const {
    return s_->host_variable(key);
}
std::optional<std::vector<uint8_t>> GameRuntime::read_save_file(
    const std::string& rel_path) const {
    return s_->read_save_file(rel_path);
}
void GameRuntime::bind_text_metrics(
    std::function<std::tuple<double, double, double>()> fn) {
    s_->bind_text_metrics(std::move(fn));
}
int GameRuntime::stage_width() const { return s_->stage_width(); }
int GameRuntime::stage_height() const { return s_->stage_height(); }
bool GameRuntime::vsync_active() const { return s_->vsync_active(); }
bool GameRuntime::frame_presented() const { return s_->frame_presented(); }
void GameRuntime::request_repaint() { s_->request_repaint(); }
std::optional<std::vector<uint8_t>> GameRuntime::capture_frame(uint32_t* w,
                                                              uint32_t* h) {
    return s_->capture_frame(w, h);
}
oa::render::Compositor& GameRuntime::scene() { return s_->scene(); }
const oa::render::Compositor& GameRuntime::scene() const { return s_->scene(); }
size_t GameRuntime::scene_layer_events() const { return s_->scene_layer_events(); }
size_t GameRuntime::scene_tween_events() const { return s_->scene_tween_events(); }
size_t GameRuntime::tween_completion_calls() const { return s_->tween_completion_calls(); }
bool GameRuntime::waiting_stop() const { return s_->waiting_stop(); }
const oa::runtime::Wait* GameRuntime::current_wait() const { return s_->current_wait(); }
void GameRuntime::apply_override(int key, int status) { s_->apply_override(key, status); }
TransitionStatus GameRuntime::transition() { return TransitionStatus(&s_->transition()); }
TransitionStatus GameRuntime::transition() const { return TransitionStatus(&s_->transition()); }

// ---- [trans] 只读投影 ------------------------------------------------------
// 转发到 render 内部语义机 oa::render::Transition 的同名只读查询（定义见
// runtime_internal.h -> core/render/render_internal.h）。state_ 恒非空：
// 只有 GameRuntime::transition() 能构造本类（构造私有 + friend GameRuntime）。
bool TransitionStatus::active() const { return state_ && state_->active(); }
bool TransitionStatus::is_in_progress(uint64_t clock_ms) const {
    return state_ && state_->is_in_progress(clock_ms);
}
double TransitionStatus::progress(uint64_t clock_ms) const {
    return state_ ? state_->progress(clock_ms) : 0.0;
}
int TransitionStatus::type() const { return state_ ? state_->type() : 0; }
const std::string& TransitionStatus::rule() const {
    static const std::string kNoRule;
    return state_ ? state_->rule() : kNoRule;
}
int TransitionStatus::vague() const { return state_ ? state_->vague() : 32; }
uint64_t GameRuntime::now_ms() const { return s_->now_ms(); }
void GameRuntime::transition_begin(const std::map<std::string, std::string>& params) { s_->transition_begin(params); }
oa::render::Transition& GameRuntime::transition_state() { return s_->transition_state(); }
void GameRuntime::set_transition_capture_callback(std::function<void()> cb) { s_->set_transition_capture_callback(std::move(cb)); }
void GameRuntime::mark_transition_captured() { s_->mark_transition_captured(); }
void GameRuntime::begin_all_delete(uint64_t time_ms) { s_->begin_all_delete(time_ms); }
double GameRuntime::all_delete_fade() const { return s_->all_delete_fade(); }
void GameRuntime::set_hit_providers(oa::render::Compositor::QuadSizeFn size, oa::render::Compositor::AlphaSamplerFn alpha, void* userdata) { s_->set_hit_providers(std::move(size), std::move(alpha), userdata); }
void GameRuntime::set_pointer_observer(std::function<void(const PointerDispatch&)> cb) { s_->set_pointer_observer(std::move(cb)); }
bool GameRuntime::is_hovered(const std::string& id) const { return s_->is_hovered(id); }
size_t GameRuntime::pointer_dispatch_count() const { return s_->pointer_dispatch_count(); }
void GameRuntime::set_pointer_warp_callback(std::function<void(int, int)> cb) { s_->set_pointer_warp_callback(std::move(cb)); }
size_t GameRuntime::pointer_warp_requests() const { return s_->pointer_warp_requests(); }
std::pair<int, int> GameRuntime::mouse_point() const { return s_->mouse_point(); }
bool GameRuntime::skip_active() const { return s_->skip_active(); }
bool GameRuntime::automode_active() const { return s_->automode_active(); }
bool GameRuntime::hide_active() const { return s_->hide_active(); }
bool GameRuntime::control_skip_effective() const { return s_->control_skip_effective(); }
bool GameRuntime::rclick_allowed() const { return s_->rclick_allowed(); }
const oa::render::TextEngine& GameRuntime::text() const { return s_->text(); }
oa::render::TextEngine& GameRuntime::text() { return s_->text(); }
size_t GameRuntime::text_events() const { return s_->text_events(); }
size_t GameRuntime::message_switch_events() const { return s_->message_switch_events(); }
oa::media::AudioEngine& GameRuntime::audio() { return s_->audio(); }
const oa::media::AudioEngine& GameRuntime::audio() const { return s_->audio(); }
oa::media::VideoEngine& GameRuntime::video() { return s_->video(); }
const oa::media::VideoEngine& GameRuntime::video() const { return s_->video(); }
const std::map<std::string, GameRuntime::EmoteLayerState>& GameRuntime::emote_layers() const { return s_->emote_layers(); }
size_t GameRuntime::emote_layer_events() const { return s_->emote_layer_events(); }
oa::media::MediaPlayers& GameRuntime::media_players() { return s_->media_players(); }
void GameRuntime::set_media_loader(std::function<std::optional<std::vector<uint8_t>>(const std::string&)> loader) { s_->set_media_loader(std::move(loader)); }
size_t GameRuntime::media_events() const { return s_->media_events(); }
size_t GameRuntime::media_handler_dispatches() const { return s_->media_handler_dispatches(); }
void GameRuntime::enable_decode_pool(int threads) { s_->enable_decode_pool(threads); }
void GameRuntime::set_save_store(std::shared_ptr<oa::runtime::SaveStore> store) { s_->set_save_store(std::move(store)); }
void GameRuntime::set_frame_capture(CaptureFn fn) { s_->set_frame_capture(std::move(fn)); }
bool GameRuntime::syssave() { return s_->syssave(); }
void GameRuntime::capture_screenshot() { s_->capture_screenshot(); }
const oa::render::RenderPlan* GameRuntime::current_plan() const {
    return s_->current_plan();
}
bool GameRuntime::save_game_to(const std::string& file) { return s_->save_game_to(file); }
bool GameRuntime::save_file_exists(const std::string& file) const {
    return s_->save_file_exists(file);
}
LoadResult GameRuntime::load_game(const std::string& file, int64_t trans_type) {
    return s_->load_game(file, trans_type);
}
bool GameRuntime::load_game_from(const std::string& file, int64_t trans_type) { return s_->load_game_from(file, trans_type); }
const std::string& GameRuntime::savepath() const { return s_->savepath(); }
void GameRuntime::apply_save_event(const oa::runtime::Event& e) { s_->apply_save_event(e); }
bool GameRuntime::apply_media_event(const oa::runtime::Event& e) { return s_->apply_media_event(e); }
oa::runtime::CallbackResult GameRuntime::apply_event(const oa::runtime::Event& e) {
    return s_->dispatch_interpreter_event(e);
}
void GameRuntime::sound_finished(oa::media::SoundCategory cat, const std::string& id) { s_->sound_finished(cat, id); }
std::vector<GameRuntime::SoundChannelState> GameRuntime::sound_state() const {
    return s_->sound_state();
}

std::pair<double, double> GameRuntime::RuntimeState::bound_image_size(const oa::render::Layer& l) {
    // Cached header-sniffed natural size; Asset textures only.
    if (l.file.empty()) return {0, 0};
    const oa::render::TextureKey key = oa::render::content_texture_key(l);
    if (key.empty() || !key.is_asset()) return {0, 0};
    const auto hit = image_size_cache_.find(key);
    if (hit != image_size_cache_.end()) return hit->second;
    std::pair<double, double> dims = {0, 0};
    auto probe = [&](const std::string& logical) {
        if (!interpreter_) return false;
        const std::string resolved = interpreter_->resolve_magic_path(logical);
        for (const std::string& cand : {resolved, resolved + ".png", resolved + ".jpg"}) {
            const auto data = fs_->read(cand);
            if (!data) continue;
            double w = 0, h = 0;
            if (sniff_image_dims(*data, &w, &h)) {
                dims = {w, h};
                return true;
            }
        }
        return false;
    };
    probe(key.name);
    image_size_cache_[key] = dims;  // negative results cached too (stable)
    return dims;
}

void GameRuntime::RuntimeState::enable_decode_pool(int threads) {
    if (threads == 0) {
        if (media_.decode_pool_) {
            if (media_.media_players_) media_.media_players_->set_decode_pool(nullptr);
            media_.video_.set_decode_pool(nullptr);
            media_.decode_pool_.reset();
        }
        return;
    }
    media_.decode_pool_ = std::make_unique<oa::media::DecodePool>(threads);
    if (media_.media_players_) media_.media_players_->set_decode_pool(media_.decode_pool_.get());
    media_.video_.set_decode_pool(media_.decode_pool_.get());
}

void GameRuntime::RuntimeState::open_project(std::string_view platform) {
    platform_ = std::string(platform);
    rt_->project_ = oa::fs::Project::open(*fs_, platform);
    oa::runtime::Interpreter::Config cfg;
    cfg.charset = rt_->project_.config.charset;
    cfg.platform = rt_->project_.config.platform;
    cfg.stage_width = rt_->project_.config.stage_width;
    cfg.stage_height = rt_->project_.config.stage_height;
    cfg.fps = rt_->project_.config.fps;
    cfg.env = rt_->project_.config.env;

    interpreter_ = std::make_unique<oa::runtime::Interpreter>(cfg);
    renderer_.bind_runtime_hooks();
    // Native [alldelete] handler. Registered as an engine tag so the
    // Lua tag filter cannot swallow it (FPM registers tags.alldelete but its
    // Lua-side lydel bookkeeping races [exit]/[reset]; the engine contract is
    // the whole-scene fade-out + clear). The runtime callback
    // (set_callback) receives the emitted custom event and runs the fade.
    interpreter_->register_engine_tag(
        "alldelete",
        [](oa::runtime::Interpreter&, const oa::runtime::Instruction& ins)
            -> std::optional<oa::runtime::Event> {
            return oa::runtime::Event::custom("alldelete", ins.params);
        });
    // Native "emotestatic" handler — the real e-table
    // createEmoteLayer enqueues lyc2 + emotestatic; the runtime decodes the
    // PSB and renders the static pose (GameRuntime::apply_emote_static).
    interpreter_->register_engine_tag(
        "emotestatic",
        [](oa::runtime::Interpreter&, const oa::runtime::Instruction& ins)
            -> std::optional<oa::runtime::Event> {
            return oa::runtime::Event::custom("emotestatic", ins.params);
        });
    // e-table EmoteLayer methods route into the runtime
    // (players may not exist yet — calls queue and replay after creation).
    interpreter_->lua_bridge().host().emote_method =
        [this](const std::string& id, const std::string& method,
               const std::string& sv, const std::string& sv2, double n1,
               double n2, double n3, double* out, std::string* sout) {
            return emote_method_dispatch(id, method, sv, sv2, n1, n2, n3, out,
                                         sout);
        };
    interpreter_->variables().platform = cfg.platform;
    // project-level seeding: savepath 与系统存档读取
    save_.savepath_ = sanitize_savepath(rt_->project_.config.savepath);
    interpreter_->set_variable("s.savepath", oa::runtime::Value::make_string(save_.savepath_));
    // 帧号 / 存档根只在 LuaHost 上用（e:getFrameNumber、io.open 重定向），
    // 直接写进去——不必先在 InterpreterHooks 上开一个洞再原样搬一次。
    interpreter_->lua_bridge().host().frame_number = [this] { return tick_count_; };
    // 框架 ReturnStack → e:tag{"return"}：会话立即收掉（原版 e:tag 语义）。
    interpreter_->lua_bridge().host().handler_return = [this] {
        return handler_session_return();
    };
    // 帧即执行上下文：解释器每压一层调用帧，外层停驻就入栈；弹帧时原样
    // 交还。UI 会话（存档/读档/配置/backlog）因此总是回到**同一页**，而不是
    // 顺着剧情往下跑。
    interpreter_->hooks().frame_pushed = [this] { push_park_context(); };
    // A handler-issued [jump] into another script is returnable when it
    // interrupts a page park (the framework's `[jump <flow>] ... [return]`
    // idiom). The hook answers whether the jump needs a return frame at all.
    interpreter_->hooks().jump_needs_return_frame = [this] {
        return park_wants_return_frame();
    };
    interpreter_->hooks().frame_popped = [this] { return pop_park_context(); };
    interpreter_->hooks().exskip_jump_target = [this](const std::string& file) {
        if (!control_.exskip_active_ || control_.exskip_target_ < 0) return;
        if (control_.exskip_script_.empty()) control_.exskip_script_ = file;
    };
    interpreter_->hooks().exskip_boundary = [this](const std::string& script,
                                                   size_t line) {
        // e:debugSkip{index=N}: stop at the instruction the backlog page was
        // printed on. The index only means something inside the script the
        // replay jumped into, and only after that jump happened — before it we
        // are still in the caller, whose line can already be past N.
        if (!control_.exskip_active_) return false;
        if (control_.exskip_target_ < 0 || control_.exskip_script_.empty()) return false;
        if (script != control_.exskip_script_) return false;
        if (line < size_t(control_.exskip_target_)) return false;
        end_debug_skip();
        return true;
    };
    interpreter_->hooks().file_loader = [this](const std::string& name)
        -> std::optional<std::vector<uint8_t>> {
        return fs_->read(interpreter_->resolve_magic_path(name));
    };
    // The decode host reads media assets through the same resolution:
    // the interpreter's magic-path table first, then the project filesystem.
    // (MediaPlayers::set_loader also probes the ".ogg"/".wav" containers for
    // extension-less sound names — the Ethornell-derived projects hand the
    // sound tags a bare base name.)
    media_.media_players_->set_loader([this](const std::string& logical)
                                   -> std::optional<std::vector<uint8_t>> {
        if (!interpreter_) return std::nullopt;
        return fs_->read(interpreter_->resolve_magic_path(logical));
    });
    // The video decoder reads whole asset files through the same face.
    // A missing/undecodable file keeps the logical immediate-finish fallback
    // inside VideoEngine, so project flows never hang on unavailable video.
    media_.video_.set_loader([this](const std::string& logical)
                          -> std::optional<std::vector<uint8_t>> {
        if (!interpreter_) return std::nullopt;
        return fs_->read(interpreter_->resolve_magic_path(logical));
    });
    // Save-area aware asset hooks — the runtime serves both the
    // project filesystem (assets) and the save store (files under the logical
    // savepath) through one resolution face, matching the host file
    // reader. Hooks: e:file / e:isFileExists / var file_exist(save) /
    // file_update_time / get_sound_info.
    const auto read_resolved =
        [this](const std::string& resolved) -> std::optional<std::vector<uint8_t>> {
        const bool under_save = !save_.savepath_.empty() &&
                                (resolved == save_.savepath_ ||
                                 (resolved.size() > save_.savepath_.size() &&
                                  resolved.rfind(save_.savepath_ + "/", 0) == 0));
        if (under_save && save_.save_store_) {
            const std::string rel = qualify_save_file(resolved);
            if (!rel.empty()) {
                if (auto b = save_.save_store_->read(rel)) return b;
                return std::nullopt; // save-area names do not fall back to PFS
            }
        }
        return fs_->read(resolved);
    };
    interpreter_->hooks().resource_read = read_resolved;
    interpreter_->hooks().resource_exists = [read_resolved](const std::string& resolved) {
        return read_resolved(resolved).has_value();
    };
    interpreter_->lua_bridge().host().save_root = [this]() -> std::string {
        if (auto dir = std::dynamic_pointer_cast<oa::runtime::DirSaveStore>(save_.save_store_)) {
            return dir->root();
        }
        return std::string();
    };
    // e:var(system=...) 的宿主数据面：解释器只管把它们写进变量域，数据和
    // 形状（name / name.size / name.N.*）都由这里给出。
    interpreter_->hooks().system_var =
        [this](const std::string& system, const std::string& name,
               const std::map<std::string, std::string>& params)
        -> std::optional<oa::runtime::InterpreterHooks::VarWrites> {
        using Writes = oa::runtime::InterpreterHooks::VarWrites;
        const auto arg = [&params](const char* k) {
            const auto it = params.find(k);
            return it == params.end() ? std::string() : it->second;
        };
        if (system == "get_layer_info") {
            // 滑块回读（slider_dragX / xslider_pin）：把一层的有效属性摊成
            // name.<prop>。先冲刷排队的文本标签，否则读到的是上一帧。
            interpreter_->flush_pending_text_tags();
            Writes out;
            const std::string id = arg("id");
            const oa::render::Layer* l = id.empty() ? nullptr : scene().find(id);
            if (!l) return out;
            std::map<std::string, std::string> props;
            for (const auto& [k, v] : l->props) props[k] = v;
            props["left"] = format_prop_num(l->left);
            props["top"] = format_prop_num(l->top);
            props["x"] = format_prop_num(l->left);
            props["y"] = format_prop_num(l->top);
            std::pair<double, double> natural = {0, 0};
            if (l->width <= 0 && !l->has_clip) natural = bound_image_size(*l);
            props["width"] = format_prop_num(
                l->width > 0 ? l->width : (l->has_clip ? l->clip_w : natural.first));
            props["height"] = format_prop_num(
                l->height > 0 ? l->height : (l->has_clip ? l->clip_h : natural.second));
            props["alpha"] = format_prop_num(l->alpha * 255.0);
            props["visible"] = l->visible != 0.0 ? "1" : "0";
            if (l->has_clip) {
                props["clip"] = format_prop_num(l->clip_x) + "," + format_prop_num(l->clip_y) +
                                "," + format_prop_num(l->clip_w) + "," +
                                format_prop_num(l->clip_h);
                props["clip_x"] = format_prop_num(l->clip_x);
                props["clip_y"] = format_prop_num(l->clip_y);
                props["clip_w"] = format_prop_num(l->clip_w);
                props["clip_h"] = format_prop_num(l->clip_h);
            }
            for (const auto& [k, v] : props)
                out.emplace_back(name + "." + k, oa::runtime::Value::make_string(v));
            return out;
        }
        if (system == "get_backlog_size") {
            Writes out;
            out.emplace_back(name, oa::runtime::Value::make_int(
                                       (long long)text().backlog_size()));
            return out;
        }
        if (system == "get_backlog_tags") {
            long long page = 0;
            try {
                page = std::stoll(arg("page"));
            } catch (...) {
                page = 0;
            }
            if (page < 0) page = 0;
            const std::string allfont = arg("allfont");
            const bool prefix = allfont == "1" || allfont == "true" || allfont == "on";
            const auto tags = text().backlog_tags((size_t)page, prefix);
            Writes out;
            if (tags) {
                for (size_t i = 0; i < tags->size(); ++i)
                    out.emplace_back(name + "." + std::to_string(i),
                                     oa::runtime::Value::make_string((*tags)[i]));
                out.emplace_back(name + ".size",
                                 oa::runtime::Value::make_int((long long)tags->size()));
            } else {
                out.emplace_back(name + ".size", oa::runtime::Value::make_int(0));
            }
            return out;
        }
        if (system == "get_message_tags") {
            const std::string allfont = arg("allfont");
            const bool prefix = allfont == "1" || allfont == "true" || allfont == "on";
            const auto tags = text().message_tags(arg("id"), prefix);
            Writes out;
            if (tags) {
                for (size_t i = 0; i < tags->size(); ++i)
                    out.emplace_back(name + "." + std::to_string(i),
                                     oa::runtime::Value::make_string((*tags)[i]));
                out.emplace_back(name + ".size",
                                 oa::runtime::Value::make_int((long long)tags->size()));
            } else {
                out.emplace_back(name + ".size", oa::runtime::Value::make_int(0));
            }
            return out;
        }
        if (system == "file_exist") {
            // 只有 save=1 归宿主：保存区不是工程资源。工程内的存在性由
            // 解释器自己按 PFS 探（它拿到这个返回值后继续走通用分支）。
            if (arg("save") != "1") return std::nullopt;
            const std::string rel = qualify_save_file(arg("file"));
            const bool exists = !rel.empty() && save_.save_store_ &&
                                save_.save_store_->exists(rel);
            Writes out;
            out.emplace_back(name, oa::runtime::Value::make_bool(exists));
            return out;
        }
        if (system == "file_update_time") {
            const std::string rel = qualify_save_file(arg("file"));
            const std::string fmt_s = arg("format");
            std::string value = arg("noexist");
            if (!rel.empty() && save_.save_store_) {
                if (auto c = save_.save_store_->modification_time(rel))
                    value = format_update_time(*c, fmt_s.empty() ? "yyyy/MM/dd hh:mm:ss"
                                                                 : fmt_s);
            }
            Writes out;
            out.emplace_back(name, oa::runtime::Value::make_string(value));
            return out;
        }
        if (system == "get_sound_info") {
            Writes out;
            const auto snap = sound_info_snapshot();
            const std::string id = arg("id");
            if (!id.empty()) {
                const auto it = std::find_if(snap.se.begin(), snap.se.end(),
                                             [&](const auto& c) { return c.id == id; });
                if (it != snap.se.end()) {
                    out.emplace_back(name + ".playing",
                                     oa::runtime::Value::make_int(it->playing ? 1 : 0));
                    out.emplace_back(name + ".gain",
                                     oa::runtime::Value::make_int(it->gain));
                    out.emplace_back(name + ".pan", oa::runtime::Value::make_int(it->pan));
                } else {
                    out.emplace_back(name + ".playing", oa::runtime::Value::make_int(0));
                }
                return out;
            }
            out.emplace_back(name + ".playing",
                             oa::runtime::Value::make_int(
                                 snap.bgm && snap.bgm->playing ? 1 : 0));
            out.emplace_back(name + ".gain",
                             oa::runtime::Value::make_int(snap.bgm ? snap.bgm->gain : 1000));
            out.emplace_back(name + ".pan",
                             oa::runtime::Value::make_int(snap.bgm ? snap.bgm->pan : 0));
            size_t index = 0;
            for (const auto& ch : snap.se) {
                const std::string pfx = name + "." + std::to_string(index);
                out.emplace_back(pfx + ".id", oa::runtime::Value::make_string(ch.id));
                out.emplace_back(pfx + ".playing",
                                 oa::runtime::Value::make_int(ch.playing ? 1 : 0));
                out.emplace_back(pfx + ".gain", oa::runtime::Value::make_int(ch.gain));
                out.emplace_back(pfx + ".pan", oa::runtime::Value::make_int(ch.pan));
                ++index;
            }
            out.emplace_back(name + ".size",
                             oa::runtime::Value::make_int((long long)snap.se.size()));
            return out;
        }
        if (system == "get_message_layer_width" || system == "get_message_layer_height" ||
            system == "get_message_layer_line_width") {
            // Lua get_fontsize：先造一层、排测量文本，再读排版尺寸。读之前
            // 必须把排队的文本标签落地，否则量到的是空层。
            interpreter_->flush_pending_text_tags();
            double width = 0, height = 0, line_width = 0;
            if (text_metrics) {
                const auto [w, h, lw] = text_metrics();
                width = w;
                height = h;
                line_width = lw;
            }
            const double value = (system == "get_message_layer_width")
                                     ? width
                                     : (system == "get_message_layer_height") ? height
                                                                              : line_width;
            Writes out;
            out.emplace_back(name,
                             oa::runtime::Value::make_string(std::to_string(value)));
            return out;
        }
        return std::nullopt; // 不是宿主数据 → 解释器走自己的通用分支
    };
    // Every interpreter event is routed by domain in one place; waits only
    // reach the host stream (and pause) through that dispatcher.
    interpreter_->set_callback([this](const oa::runtime::Event& e) {
        return dispatch_interpreter_event(e);
    });
}

// --------------------------------------------------------------------------- Interpreter event dispatch — the fixed domain order is: engin...
oa::runtime::CallbackResult GameRuntime::RuntimeState::dispatch_interpreter_event(
    const oa::runtime::Event& e)
{
    oa::runtime::CallbackResult result = oa::runtime::CallbackResult::Continue;
    if (dispatch_engine_event(e, &result)) return result;
    if (dispatch_media_event(e, &result)) return result;
    if (apply_text_event(e)) return oa::runtime::CallbackResult::Continue;
    if (apply_scene_event(e)) return host_event_result(e);
    if (apply_control_event(e)) return oa::runtime::CallbackResult::Continue;

    // `[save]` 必须**当场**落盘：晚一拍，脚本已经走完存档页的收尾
    // （t.saveslot=0、status、页变量都变了），存下来的变量/画面就不再是
    // `[save]` 那一刻的东西。save 不重入解释器（onSave 只写变量），安全。
    // `[load]` 仍走事件泵：它要清场 + 恢复位置，放在 tick 的派发段执行。
    if (e.tag == "save") {
        handle_save_tag(e);
        return oa::runtime::CallbackResult::Continue;
    }
    // `[load]` **不返回**：原版读档是"整台机器的位置换掉"，`load.iet`/
    // `backlog.iet` 的 `[load]` 后面紧跟的是"读档失败"的收尾代码。以前这里
    // 只是把事件排进队列、脚本继续往下跑 —— `[load]` 之后的失败分支（会删掉
    // `g.backlogjump_keep`、关掉 UI）先在当晚跑完，真正读档时才到，导致
    // "跳转 = 没有跳过 start 收尾"（回顾跳转不重建消息窗）。这里返回 Pause：
    // 脚本停在 `[load]`，事件泵在同一拍完成读档。
    if (e.tag == "load") {
        pending_events_.push_back(e);
        return oa::runtime::CallbackResult::Pause;
    }

    pending_events_.push_back(e);
    return host_event_result(e);
}

bool GameRuntime::RuntimeState::dispatch_engine_event(
    const oa::runtime::Event& e, oa::runtime::CallbackResult* out)
{
    if (e.tag == "trigger") {
        const auto it = e.params.find("name");
        if (it != e.params.end() && !it->second.empty()) park_.pending_custom = it->second;
        *out = oa::runtime::CallbackResult::Continue;
        return true;
    }
    if (e.tag != "alldelete") return false;
    // Whole-scene fade-out + clear; time==0 clears instantly and continues.
    uint64_t t = 0;
    if (const auto it = e.params.find("time"); it != e.params.end()) {
        try {
            t = (uint64_t)std::stoull(it->second);
        } catch (...) {
            t = 0;
        }
    }
    begin_all_delete(t);
    *out = t > 0 ? oa::runtime::CallbackResult::Pause
                 : oa::runtime::CallbackResult::Continue;
    return true;
}

bool GameRuntime::RuntimeState::dispatch_media_event(
    const oa::runtime::Event& e, oa::runtime::CallbackResult* out)
{
    // e:createEmoteLayer static layer (lyc2 already materialized the carrier
    // chain); decode + render + bind, never pauses.
    if (e.tag == "emotestatic") {
        apply_emote_static(e);
        *out = oa::runtime::CallbackResult::Continue;
        return true;
    }
    if (!apply_media_event(e)) return false;
    // A fullscreen [video] (no id) parks the script (Stop{reason:"video"}).
    *out = (e.tag == "video" && e.id.empty())
               ? oa::runtime::CallbackResult::Pause
               : oa::runtime::CallbackResult::Continue;
    return true;
}

oa::runtime::CallbackResult GameRuntime::RuntimeState::host_event_result(
    const oa::runtime::Event& e) const
{
    using K = oa::runtime::Event::Kind;
    if (e.kind == K::Wait_) return oa::runtime::CallbackResult::Pause;
    // [reset] hands the engine back to its boot script: continuing would run
    // the rows after [reset] against the cleared variable store.
    if (e.kind == K::Reset) return oa::runtime::CallbackResult::Pause;
    if (e.kind == K::Trans) {
        int type = 1;
        if (const auto it = e.params.find("type"); it != e.params.end()) {
            try {
                type = std::stoi(it->second);
            } catch (...) {
                type = 1;
            }
        }
        if (type != 0) return oa::runtime::CallbackResult::Pause;
    }
    if (e.kind == K::LayerEventCmd && e.tag == "lytween" && !e.id.empty()) {
        int sync = 0;
        if (const auto it = e.params.find("sync"); it != e.params.end()) {
            try {
                sync = std::stoi(it->second);
            } catch (...) {
                sync = 0;
            }
        }
        // A [tweenset] member is collected, not started: its park is armed at
        // [/tweenset] after the group starts.
        if (sync != 0 && !scene().tweenset_collecting())
            return oa::runtime::CallbackResult::Pause;
    }
    // A closed [tweenset] carrying sync members is one Tween park for all of
    // them.
    if (e.kind == K::LayerEventCmd && e.tag == "/tweenset" &&
        !park_.pending_sync_tweens.empty())
        return oa::runtime::CallbackResult::Pause;
    return oa::runtime::CallbackResult::Continue;
}

void GameRuntime::RuntimeState::boot_project()
{
    // Load system saves before boot (Lua dataloading reads g./s.).
    sysload();
    load_read_state(); // g.readlines -> 既読表（skip 的既读判定）
    wire_lua_host();
    interpreter_->boot(rt_->project_.config.boot_script);
}

void GameRuntime::RuntimeState::reset_domains() {
    // Clear media/text/scene/transition/pointer/wait; boot re-applies config.
    media_.audio_.stop_all_sounds();
    if (media_.media_players_) media_.media_players_->stop_all();
    media_.video_.stop_all_videos();
    // Aligned with reset_ephemeral_on_load: stale video EOF must not leak.
    text().clear_scene();
    scene().clear_scene();
    drop_pending_tween_cancels(); // deferred timer rounds die with nodes
    media_.emote_layers_.clear(); // static emote frames die with the scene
    media_.emote_view_cache_.clear(); // ... and so does a recorded view
    transition().clear();
    pointer_.hovered_.clear();
    pointer_.drag_layer_.clear();
    reset_park_contexts();
    save_.screenshot_.valid = false;
    renderer_.stop_all_delete();
    park_.wait.reset();
    park_.tail.clear();
    park_.pending.clear();
    park_.pending_custom.clear();
    park_.pending_sync_tweens.clear();
    save_.autosaved_current_wait_ = false;
    save_.autosave_last_seq_ = 0;
    exit_requested_ = false;
    control_.skip_active_ = false;
    control_.exskip_active_ = false; // e:debugSkip state dies with the domains
    control_.skip_allowed_ = true;
    control_.skip_enabled_ = false;
    control_.automode_allowed_ = true;
    control_.automode_ = false;
    control_.control_skip_pressed_ = false;
    control_.control_skip_blocked_ = false;
    control_.last_control_skip_effective_ = false;
    control_.rclick_allowed_ = false;
    control_.rclick_file_.clear();
    control_.hide_allowed_ = false;
    control_.hide_active_ = false;
    control_.hide_window_.clear();
    control_.hide_snapshot_.clear();
    control_.keymap_.clear();
    control_.script_status_ = 0;
    control_.active_wait_icon_.clear();
    input_state_.overrides_.clear();
    input_state_.prev_overrides_.clear();
    input_state_.keys_down_.clear();
    input_state_.down_edges_.clear();
    input_state_.up_edges_.clear();
    input_state_.decide_edge_ = false;
    pointer_.mouse_prev_x_ = 0;
    pointer_.mouse_prev_y_ = 0;
    pointer_.mouse_inited_ = false;
    pointer_.left_down_prev_ = false;
}

// ---------------------------------------------------------------------------
// [alldelete] (engine-native whole-scene fade-out + clear)
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::begin_all_delete(uint64_t time_ms) {
    // FPM go_title/go_exit/suspend rows: "[alldelete time=1500]" fades every
    // scene layer (message-text overlay nodes included) to 0 on one global
    // opacity ramp, then clears the scene and the text engine — the black
    // transition page. time==0 clears instantly. The script pauses
    // (Stop{id:"alldelete"}) while the fade runs (see resolve_wait).
    renderer_.begin_all_delete(time_ms, now_ms_);
    if (time_ms == 0) finish_all_delete();
}

double GameRuntime::RuntimeState::all_delete_fade() const {
    return renderer_.all_delete_fade(now_ms_);
}

void GameRuntime::RuntimeState::finish_all_delete() {
    renderer_.stop_all_delete();
    // Everything the [alldelete] flow wipes: scene layers (image + text
    // overlay nodes + handler registries + message bindings), the text
    // engine content, and the pointer state pointing into them.
    scene().clear_scene();
    text().clear_scene();
    drop_pending_tween_cancels(); // deferred timer rounds die with nodes
    pointer_.hovered_.clear();
    pointer_.drag_layer_.clear();
    save_.screenshot_.valid = false;
    media_.emote_layers_.clear(); // static emote frames die with the scene
    media_.emote_view_cache_.clear(); // ... and so does a recorded view
}

void GameRuntime::RuntimeState::restart_project() {
    if (!interpreter_) return;
    reset_domains();
    // Reboot with a fresh interpreter (fresh Lua VM: the boot script's [lua]
    // init blocks re-run — reusing the VM would leave the framework globals
    // at their mid-game state). open_project re-wires every interpreter hook;
    // the host-side bridges (save store, hit providers, capture) survive.
    pending_events_.clear();
    open_project(platform_);
    boot_project();
}

void GameRuntime::RuntimeState::wire_lua_host() {
    auto& host = interpreter_->lua_bridge().host();
    auto* interp = interpreter_.get();
    host.now_ms = [this] { return now_ms_; };
    host.get_script_status = [this] { return control_.script_status_; };
    host.set_script_status = [this](int s) { control_.script_status_ = s; };
    host.debug_skip_start = [this](int64_t index) { start_debug_skip(index); };
    host.is_down = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            if (it->second & 4) return true;
        }
        return input_state_.keys_down_.count(key) > 0;
    };
    host.is_down_edge = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            const int prev = input_state_.prev_overrides_.count(key) ? input_state_.prev_overrides_.at(key) : 0;
            if ((it->second & 8) && !(prev & 8)) return true;
        }
        // physical edges are delivered through down-edge set computed in tick
        return input_state_.down_edges_.count(key) > 0;
    };
    host.is_up_edge = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            const int prev = input_state_.prev_overrides_.count(key) ? input_state_.prev_overrides_.at(key) : 0;
            if ((it->second & 16) && !(prev & 16)) return true;
        }
        return input_state_.up_edges_.count(key) > 0;
    };
    host.is_push = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            if (it->second & 2) return true;
        }
        return input_state_.keys_down_.count(key) > 0;
    };
    host.is_decide = [this](int key) {
        if (const auto it = input_state_.overrides_.find(key); it != input_state_.overrides_.end()) {
            const int prev = input_state_.prev_overrides_.count(key) ? input_state_.prev_overrides_.at(key) : 0;
            if ((it->second & 32) && !(prev & 32)) return true;
        }
        return false;
    };
    host.get_mouse_point = [this] {
        return std::pair(input_state_.mouse_x_, input_state_.mouse_y_);
    };
    host.override_key = [this](int key, int status) {
        apply_override(key, status);
    };
    (void)interp;
}

void GameRuntime::RuntimeState::apply_override(int key, int status) {
    if (key < 0) {
        // `e:overrideKey{status=0}`（不带 key）= **清空本帧输入**。
        //
        // 这是 vsync 系列唯一的"全局吞帧"手段：`scr.btstop`（按钮全停）、
        // 按键检查、alt+enter 屏蔽、对话框/存读档页打开时都靠它把这一帧的
        // 输入拦在引擎之外。以前这里是 "not supported yet, ignore" ——
        // 于是脚本以为吞掉了，引擎照样把同一次按下派发给层 handler 和剧情
        // 翻页，表现就是"点了一下既触发对话又推进了剧情 / 有时又什么都不
        // 发生"（test2 的概率性）。
        if (status == 0) {
            input_state_.keys_down_.clear();
            input_state_.down_edges_.clear();
            input_state_.up_edges_.clear();
            input_state_.overrides_.clear();
            input_state_.swallowed_frame_ = true;
        }
        return;
    }
    if (status == 0) {
        input_state_.overrides_.erase(key);
    } else {
        input_state_.overrides_[key] = status;
    }
}

void GameRuntime::RuntimeState::tick(uint64_t delta_ms, const FrameInput& in) {
    if (!interpreter_) return;
    ++tick_count_;
    now_ms_ += delta_ms;
    // A captured transition may have reached its duration: auto-clear before
    // any wait resolution consults it (transition::clear_finished every
    // frame).
    transition().clear_finished(now_ms_);
    // Drop the inline-event marker once its frame left the stack.
    // A closed UI screen may have left its own frames on the stack: hand the
    // page it was opened from back before input is resolved.
    // [alldelete]: the render fade clock expired -> clear the scene + text.
    if (renderer_.all_delete_expired(now_ms_)) finish_all_delete();
    // Cursor-warp emulation when the host cannot warp (hidden/headless).
    FrameInput eff = in;
    const bool raw_static =
        eff.mouse_x == pointer_.raw_mouse_x_ && eff.mouse_y == pointer_.raw_mouse_y_;
    pointer_.raw_mouse_x_ = eff.mouse_x;
    pointer_.raw_mouse_y_ = eff.mouse_y;
    if (pointer_.pointer_warp_pending_) {
        if (raw_static && !eff.left_click_edge && !eff.right_click_edge &&
            pointer_.drag_layer_.empty()) {
            eff.mouse_x = pointer_.pointer_warp_x_;
            eff.mouse_y = pointer_.pointer_warp_y_;
        } else {
            pointer_.pointer_warp_pending_ = false;
        }
    }
    input_state_.mouse_x_ = eff.mouse_x;
    input_state_.mouse_y_ = eff.mouse_y;

    // update physical state
    input_state_.down_edges_.clear();
    input_state_.up_edges_.clear();
    input_state_.keys_down_ = in.keys_down;
    for (const int k : in.key_down_edges) input_state_.down_edges_.insert(k);
    for (const int k : in.key_up_edges) input_state_.up_edges_.insert(k);
    if (in.left_click_edge) input_state_.down_edges_.insert(kKeyMouseLeft);
    // 鼠标左键的**抬起边沿**：只有"上一帧按着、这一帧松开"才算。
    //
    // 宿主（FrameInput）没有 left_up_edge 字段，最初这里写成"没按着就算抬起"，
    // 于是 `e:isUpEdge(1)` 在每一帧都返回真。Lua 侧拿它当事件用的框架会被
    // 骗：test13 的 vsync 用 `isUpTap()`（= `e:isUpEdge(1)`）判定"原地松手"，
    // 恒真 ⇒ 光标刚悬停到存档槽、`save_over` 立起 `adv.longtap` 的下一帧就
    // 被当成本次滑动的松手消费掉，槽位点击永远走不到 `save_check` ——
    // 读档界面点哪个槽都没反应（"没法读档"）。
    //
    // 上一帧的按下态取自 pointer_.left_down_prev_：它在本函数之后的
    // process_pointer_input 里才更新，此处读到的正是上一帧的值。
    if (pointer_.left_down_prev_ && !in.left_down)
        input_state_.up_edges_.insert(kKeyMouseLeft);
    // decide edge: override bit 32 freshly set this frame (scripted decide).
    // The snapshot is taken BEFORE the vsync hook so overrides injected by
    // Lua's onEnterFrame (the FPM dummy-click pattern: setexclick ->
    // e:overrideKey status=32) produce their decide edge in the SAME advance
    // (Lua runs inside the host frame before wait resolution).
    input_state_.prev_overrides_ = input_state_.overrides_;
    // 每帧复位"输入被吞"：由本帧 vsync 的 overrideKey{status=0} 决定。
    input_state_.swallowed_frame_ = false;

    // Lua per-frame hook first (it rewrites keys/decide edges, clears flags)
    // 框架每帧读 s.status.*：在 Lua 的 onEnterFrame 之前把控制域状态摊平。
    sync_control_status();
    interpreter_->fire_event("onEnterFrame");
    // decide 边沿 = 相对上一帧快照新出现的位 32（帧内/测试直接注入均
    // 生效）。配合每帧末清空覆盖（见 tick 尾部注释），FPM exclick 的
    // status=0+32 同帧注入在下一帧构成新边沿；滞留位不会再吞掉后续 decide。
    input_state_.decide_edge_ = false;
    for (const auto& [key, bits] : input_state_.overrides_) {
        const int prev = input_state_.prev_overrides_.count(key) ? input_state_.prev_overrides_.at(key) : 0;
        if ((bits & 32) && !(prev & 32)) {
            input_state_.decide_edge_ = true;
            // 脚本合成的这一下按键也要进 push 注册表。
            //
            // 原版 `overrideKey{key=.., status=32}` = "此刻按下了这个键"：
            // 存档/读档页的 `setonpush{key="146", exec="save_click"}` 就挂在
            // 这条路上（vsync 在"原地松手"时合成 146，把槽位点击送到
            // `save_click` → `save_check`）。以前只置 decide 位，push 行收不到
            // ⇒ 点槽位后没人调 save_check，读档界面点哪个槽都没反应。
            input_state_.down_edges_.insert(key);
            input_state_.keys_down_.insert(key);
        }
    }

    // Control-skip hold (role-14 keys) + effective-skip recompute —
    // the wait machine consults control_.skip_active_.
    update_control_skip_hold(input_state_.keys_down_);
    control_.skip_active_ = skip_active();

    // Pointer chain runs before wait resolution; a claimed press never
    // reaches the script. 一次按下的去向在 process_pointer_input 里定死
    // （引擎门 → 层 → 脚本），这里只读结果。
    const PressDecision press = process_pointer_input(eff);
    const bool clicked = press.owner == PressDecision::Owner::Script;

    // 一次物理按下 = 一个命令。同一次按下会给出多个信号（原始按下、图层
    // 命中、游戏 setexclick 派生的 decide 边缘），它们同属一条命令，只能被
    // 消费一次。press_used_ 只延续一帧：把这次按下的 decide 余波吞掉。
    const bool press_edge = press.press;
    if (press_edge) press_used_ = false;
    const bool used_before = press_used_;
    press_live_ = press_edge || (clicked && !press_used_) ||
                  (input_state_.decide_edge_ && !press_used_);
    // 文字还在逐字显示：这一按只用来补完整页，命令到此用尽。
    if (press_live_ && text().is_reveal_paced()) {
        text().reveal_all();
        press_used_ = true;
        press_live_ = false;
    }
    // 归一后的两条命令（补字用掉的那一按不再算）。
    const bool click_cmd = clicked && !press_used_;
    const bool decide_cmd = input_state_.decide_edge_ && !press_used_;
    // オート中のクリックはオートを止める（[automode stopbyclick]、既定 on）。
    // 判定は「生の押下」で行う：文字送り中の 1 発は click_cmd に昇格しない
    // （press_used_ が補字に食う）ので、click_cmd だけ見ると止まらない。
    // ボタン自身を押した 1 発（layer_handled）は除く —— オート開始の
    // クリックで即停止してしまう。
    if (press_edge && press.owner != PressDecision::Owner::Layer && automode_active() &&
        control_.automode_stopbyclick_)
        set_automode_mode(false);

    // script status 4 = forced stop (no user input): park the story line but
    // keep onEnterFrame ticking (already fired) and scene tweens advancing.
    // 緊急回避中も脚本を進めない：原版の vsync_game は avoid 中に本体ループへ
    // 入らない（回避画像の下で会話が進んでしまうのを防ぐ）。
    if (control_.script_status_ != 4 && !control_.avoid_active_) {
        // Exskip: burn crossing parks in one bounded burst.
        const int kMaxExskipBurst = 4096;
        int burst = 0;
        for (;;) {
            const bool was_ff = control_.exskip_active_;
            bool attempted_resolve = false;
            if (park_.wait) {
                if (interpreter_->has_queued_tags()) drain_parked_queue();
                if (park_.wait) {
                    attempted_resolve = true;
                    if (resolve_wait(delta_ms, click_cmd, decide_cmd)) {
                        if (press_live_) press_used_ = true;
                        advance_wait();
                    }
                } else {
                    // a queued jump/call/return resumed the script (it
                    // clears the wait and runs the interpreter the same frame).
                    run_until_wait();
                }
            } else {
                // execute until the next wait or completion
                run_until_wait();
            }
            // The exskip burst keeps consuming parks while the
            if (!was_ff || !control_.exskip_active_) break;
            if (++burst >= kMaxExskipBurst) break;
            if (attempted_resolve && park_.wait) break;
        }
    }

    // 被脚本输入掩码吃掉的这一下：如果同一帧里脚本自己的等待正好被它放行
    // （框架的 `[wait]` 之后紧跟 `var t.eventfilter=0` 是标准写法），原版在
    // 这一下之后才派发层事件，所以按钮是“一下点得动”的。引擎把层派发放在
    // 等待解析之前，必须在这里补一次，否则每个按钮/对话框都要点两次。
    park_.pending_custom.clear();

    // Overrides are frame-local: a sticky decide bit 32 would swallow every
    // later click edge.
    input_state_.overrides_.clear();
    // Host-stream drain (save then control) runs before the content advance.
    dispatch_pending_events();
    maybe_autosave_for_wait();

    // Content-plane order is phase-critical (R6): text reveal → scene tweens
    // → emote clock → click-wait icon → media frame end. Do not reorder.
    advance_content_planes(delta_ms);
    // press_used_ 只保护紧接着的一帧（同一次按下的 decide 余波）。
    if (used_before) press_used_ = false;
}

void GameRuntime::RuntimeState::arm_park(oa::runtime::Wait w, bool script_owned) {
    w.script_owned = script_owned;
    w.seq = ++wait_seq_;
    w.armed_tick = tick_count_;
    w.auto_elapsed_ms = 0;
    w.se_deadline_ms = 0;
    // The line this park belongs to: a park is only meaningful while the script
    // still sits on it (see pop_park_context).
    if (const std::string* script = interpreter_ ? interpreter_->current_script() : nullptr) {
        w.owner_script = *script;
        w.owner_line = interpreter_->current_line();
    } else {
        w.owner_script.clear();
        w.owner_line = 0;
    }
    normalize_wait(w);
    if (std::getenv("OA_PARKDBG")) {
        const std::string* cs = interpreter_ ? interpreter_->current_script() : nullptr;
        std::fprintf(stderr, "[park] arm %s:%zu (now %s:%zu) stage=%d click=%d timed=%lld own=%d w=0x%x\n",
                     w.owner_script.c_str(), w.owner_line,
                     cs ? cs->c_str() : "-", interpreter_ ? interpreter_->current_line() : 0,
                     int(w.stage), w.has(oa::runtime::Wait::WakeClick) ? 1 : 0,
                     (long long)w.timer_ms, w.script_owned ? 1 : 0, w.wakes);
    }
    park_.wait = std::move(w);
}

void GameRuntime::RuntimeState::normalize_wait(oa::runtime::Wait& w) {
    using W = oa::runtime::Wait;
    // 不清 wakes：构造时写的就是"什么能放行这个停驻"（唯一的真值），
    // 这里只补演出/时钟类的位。
    w.wake_id.clear();
    if (w.timed()) {
        w.wakes |= W::WakeTimer;
        w.deadline_ms = now_ms_ + uint64_t(w.timer_ms);
    }
    if (!w.keys.empty()) w.wakes |= W::WakeKeys;
    if (w.on_reveal_done) w.wakes |= W::WakeRevealDone;
    if (w.reveal_clear) w.wakes |= W::WakeRevealClear;
    if (w.ends_exskip) w.wakes |= W::WakeExskipEnd;
    switch (w.stage) {
        case W::Stage::Trans:
            w.wakes |= W::WakeTrans;
            w.wake_id = "trans";
            break;
        case W::Stage::Video:
            w.wakes |= W::WakeVideo;
            break;
        case W::Stage::VideoLayer:
            w.wakes |= W::WakeVideoLayer;
            w.wake_id = w.stage_id;
            break;
        case W::Stage::Se:
            w.wakes |= W::WakeSe;
            w.wake_id = w.stage_id;
            break;
        case W::Stage::AllDelete:
            w.wakes |= W::WakeAllDelete;
            break;
        case W::Stage::Tween:
            w.wakes |= W::WakeTween;
            w.wake_id = w.stage_id;
            break;
        case W::Stage::None:
            break;
    }
    if (!w.custom_name.empty()) {
        w.wakes |= W::WakeCustom;
        w.wake_id = w.custom_name;
    }
    // a wait registered on a presentation also accepts the "advance"
    // input; fire_click/fire_skip decide whether the presentation is actually
    // skippable (video skippable flag / transition input policy). [wait se=]
    // joins only when the tag asked for it (input/canskip).
    if (w.stage == W::Stage::Trans || w.stage == W::Stage::Video ||
        w.stage == W::Stage::VideoLayer || w.stage == W::Stage::Tween) {
        // Keep the spec view as the tag declared it (a trans/video park stays a
        // stop-class wait for probes and autosave); only the effective wakes
        // gain the advance input.
        w.wakes |= W::WakeClick | W::WakeDecide;
    }
}

std::optional<oa::runtime::Wait> GameRuntime::RuntimeState::park_from_event(
    const oa::runtime::Event* ev) {
    using W = oa::runtime::Wait;
    using K = oa::runtime::Event::Kind;
    if (!ev) return std::nullopt;
    if (ev->kind == K::Wait_) return ev->wait;
    if (ev->kind == K::Trans) {
        int type = 1;
        if (const auto it = ev->params.find("type"); it != ev->params.end())
            type = parse_i32(it->second, 1);
        if (type == 0) return std::nullopt;
        W w;
        w.wakes = W::WakeSkip | W::WakeDecide;
        w.stage = W::Stage::Trans;
        w.stage_id = "trans";
        return w;
    }
    if (ev->kind == K::LayerEventCmd) {
        if (ev->tag == "video" && ev->id.empty()) {
            W w;
            w.wakes = W::WakeSkip | W::WakeDecide;
            w.stage = W::Stage::Video;
            return w;
        }
        if (ev->tag == "lytween" && !ev->id.empty()) {
            int sync = 0;
            if (const auto it = ev->params.find("sync"); it != ev->params.end())
                sync = parse_i32(it->second, 0);
            if (sync != 0 && !scene().tweenset_collecting()) {
                W w;
                w.wakes = W::WakeSkip | W::WakeDecide;
                w.stage = W::Stage::Tween;
                w.stage_id = ev->id;
                return w;
            }
        }
        if (ev->tag == "/tweenset" && !park_.pending_sync_tweens.empty()) {
            W w;
            w.wakes = W::WakeSkip | W::WakeDecide;
            w.stage = W::Stage::Tween;
            w.tween_ids = std::move(park_.pending_sync_tweens);
            park_.pending_sync_tweens.clear();
            return w;
        }
        return std::nullopt;
    }
    if (ev->kind == K::Custom && ev->tag == "alldelete") {
        int64_t time = 0;
        if (const auto it = ev->params.find("time"); it != ev->params.end())
            time = parse_i32(it->second, 0);
        if (time > 0) {
            W w;
            w.wakes = W::WakeSkip;
            w.stage = W::Stage::AllDelete;
            w.stage_id = "alldelete";
            return w;
        }
    }
    return std::nullopt;
}

namespace {
// A script [stop] with no click/timer/reveal/keys: the shape a UI flow may
// legitimately take over (title/logo/menu boot parks).
bool is_bare_stop(const oa::runtime::Wait& w) {
    return w.stage == oa::runtime::Wait::Stage::None && !w.timed() && w.keys.empty() &&
           !w.on_reveal_done && !w.reveal_clear && !w.waiting_click() &&
           !w.has(oa::runtime::Wait::WakeClick);
}
} // namespace

void GameRuntime::RuntimeState::clear_pending_parks() {
    // Their tails hold already-enqueued script tags: put them back so nothing
    // is silently dropped when the queue parks are superseded.
    for (auto& p : park_.pending) {
        interpreter_->prepend_tags(std::move(p.tail));
    }
    park_.pending.clear();
}

void GameRuntime::RuntimeState::prepend_live_tail() {
    if (!park_.tail.empty()) {
        interpreter_->prepend_tags(std::move(park_.tail));
        park_.tail.clear();
    }
}

void GameRuntime::RuntimeState::run_until_wait() {
    if (!interpreter_) return;
    const oa::runtime::ExecutionResult r = interpreter_->run();
    if (r == oa::runtime::ExecutionResult::Wait) {
        const bool from_queue = interpreter_->last_wait_from_queue();
        if (auto park = park_from_event(interpreter_->last_wait_event())) {
            arm_park(std::move(*park), !from_queue);
            // The tags still queued behind this wait are the script's own
            // continuation (the page's click_exit cleanup): hold them until the
            // wait releases instead of running them under the live page.
            if (from_queue) park_.tail = interpreter_->take_tag_queue();
            // A queued UI wait that arrived before this bare stop must take
            // over now, or its click wake can never be reached (the opening
            // logo's eqwait was exactly this shape).
            if (park_.wait && is_bare_stop(*park_.wait) && !park_.pending.empty()) {
                ParkedWait next = std::move(park_.pending.front());
                park_.pending.pop_front();
                const bool next_owned = next.wait.script_owned;
                arm_park(std::move(next.wait), next_owned);
                prepend_live_tail();
                park_.tail = std::move(next.tail);
            }
        }
    }
    // BudgetExhausted: resume next frame. Completed: the script is done; no
    // presentation gate is latched behind its back (sync tweens park at the
    // [lytween sync=1] tag itself).
}

void GameRuntime::RuntimeState::drain_parked_queue() {
    if (!interpreter_) return;
    // A park never blocks user input: tags queued by pointer handlers (hover
    // feedback, the message-window buttons' UI calls) keep draining. Only the
    // script's own tail queued behind the wait is held (park_.tail) and runs
    // in order once the wait releases.
    int guard = 0;
    while (interpreter_->has_queued_tags() && guard++ < 64) {
        const std::string* script_before = interpreter_->current_script();
        const size_t line_before = interpreter_->current_line();
        const size_t stack_before = interpreter_->call_stack().size();
        const oa::runtime::ExecutionResult r = interpreter_->run_queued();
        if (r == oa::runtime::ExecutionResult::ParkRestored) {
            // A [return] restored the caller's suspended park (wait + pending
            // parks + queue tail). Nothing in this drain may run behind it.
            return;
        }
        // 队列里的 [jump]/[call] 是**显式改位置**：即便落点和停驻记录的
        // 行号相同（停驻记的是"停住后"的下一行，同脚本内的 label 可能正好
        // 是同一行），旧停驻也已经失效。只比较行号会把这种情况当成"没动"，
        // 于是脚本位置已经跳到 label、旧停驻却还挡在前面 —— 玩家必须再点
        // 一次才继续（test10 的按钮/对话框"点两次"）。
        const bool explicit_move =
            interpreter_->queued_saw_jump() || interpreter_->queued_saw_call();
        const bool moved = explicit_move ||
                           script_before != interpreter_->current_script() ||
                           line_before != interpreter_->current_line() ||
                           stack_before != interpreter_->call_stack().size();
        if (r == oa::runtime::ExecutionResult::Wait) {
            // 队列里的 [stop] 是框架的"防穿透"余量（stop 过滤器在停住之后又
            // 排了一条）。它跟着同一个队列里的 [jump]/[call] 一起被排空时，
            // 属于**旧位置**：不能再把已经跳过去的新位置停住。否则脚本位置
            // 已经是 label、停驻却还在原处 —— 玩家得再点一次才动
            // （test10 的按钮/对话框"点两次"就是这么来的）。
            const auto* ev = interpreter_->last_wait_event();
            const bool stop_like =
                ev && ev->kind == oa::runtime::Event::Kind::Wait_ &&
                ev->wait.stage == oa::runtime::Wait::Stage::None &&
                ev->wait.timer_ms < 0 && ev->wait.keys.empty() &&
                !ev->wait.on_reveal_done && !ev->wait.reveal_clear;
            if (explicit_move && interpreter_->last_wait_from_queue() && stop_like) {
                park_.wait.reset();
                // 旧位置的尾巴属于被放弃的流程（框架的重复 [stop] 也在里
                // 面）；补到新位置前面只会把它再停住。
                park_.tail.clear();
                clear_pending_parks();
                return;
            }
            if (auto park = park_from_event(interpreter_->last_wait_event())) {
                // 只有"队里那条 tag 自己"的停驻才是队列停驻。排空过程里被
                // [call] 带进去的脚本行（UI 的 [stop]）仍属于脚本：释放时要
                // 推进它，否则点一下只会在同一行重新停住（test13 关掉存档
                // 界面后停在 ui.asb:1，怎么点都回不到正文）。
                park->script_owned = !interpreter_->last_wait_from_queue();
                ParkedWait next;
                next.wait = std::move(*park);
                if (interpreter_->last_wait_from_queue())
                    next.tail = interpreter_->take_tag_queue();
                // A UI flow may take over a bare script stop (title/logo/menu
                // parks). Story pacing parks (click/timed/reveal/keys) are
                // protected: their waits go to park_.pending instead.
                const bool stop_park = park_.wait && is_bare_stop(*park_.wait);
                const bool next_owned = next.wait.script_owned;
                if (park_.wait && (!park_.wait->script_owned || stop_park)) {
                    // Same UI flow, or a bare stop handing over: latest-wins.
                    // The dropped parks' deferred tails still have to run; run
                    // them in enqueue order in front of the new park's tail.
                    std::vector<oa::runtime::Instruction> merged =
                        std::move(park_.tail);
                    for (auto& p : park_.pending) {
                        merged.insert(merged.end(), p.tail.begin(), p.tail.end());
                    }
                    merged.insert(merged.end(), next.tail.begin(), next.tail.end());
                    park_.pending.clear();
                    arm_park(std::move(next.wait), next_owned);
                    park_.tail = std::move(merged);
                } else {
                    next.wait.seq = 0; // assigned by arm_park when it becomes current
                    park_.pending.push_back(std::move(next));
                }
            }
            return; // the running park still governs
        }
        if (moved) {
            // A queued jump/return changed position: the UI flow takes over.
            // 显式 [jump]/[call] 是"换流程"：旧停驻的尾巴属于被放弃的位置
            // （框架 stop 过滤器排下的重复 [stop] 就在这里），不能补到新
            // 位置前面。只有 return 这类"回到原流程"的移动才需要把尾巴
            // 还回去。
            park_.wait.reset();
            if (!explicit_move) interpreter_->prepend_tags(std::move(park_.tail));
            park_.tail.clear();
            clear_pending_parks();
            park_.pending_custom.clear();
            park_.pending_sync_tweens.clear();
            return;
        }
    }
}

bool GameRuntime::RuntimeState::handler_session_return() {
    if (!interpreter_) return false;
    return interpreter_->handler_return_now();
}

void GameRuntime::RuntimeState::dispatch_pending_events() {
    // Fixed frame-level order: a flow's own [syssave] must land before the
    // [reset] that reboots it.
    dispatch_save_events();
    dispatch_control_events();
}

void GameRuntime::RuntimeState::reboot_boot_keep_vm(const char* reason) {
    // [reset]/[gotitle]: clear every domain and re-run the boot head with the
    // Lua VM kept. No label is hardcoded — the data's own boot chain decides
    // where to go (FPM keeps `systemreset` in the VM for that decision).
    std::fprintf(stderr, "[runtime] %s (VM kept; boot re-run)\n", reason);
    reset_domains();
    pending_events_.clear();
    try {
        interpreter_->boot(rt_->project_.config.boot_script);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[runtime] %s boot re-run failed (%s); "
                             "full restart fallback\n", reason, ex.what());
        restart_project();
    }
}

void GameRuntime::RuntimeState::dispatch_control_events() {
    if (pending_events_.empty()) return;
    std::deque<oa::runtime::Event> keep;
    for (const auto& e : pending_events_) {
        if (e.kind == oa::runtime::Event::Kind::Exit) {
            // 原版引擎在 [exit] 时**自动写全局档**（BOWG/BOWX），这是 g./s. 域
            // 唯一的常规持久化点：游戏脚本自己把 `[save]`（无 file）散落在
            // 菜单里，但"选完语言 → 标题 → 退出"这条路径一次都不经过它们。
            // test10 的 macro.iet 把这件事写成了注释：
            //   「[exit]命令でグローバルセーブが自動で走るので対策する」
            // （*共通終了処理，紧挨着 [exit]）。少了这一步，g.language /
            // g.clear_flag / g.readlines 都不会落盘，第二次启动就回到语言页、
            // 标题也永远停在未通关分支。
            (void)syssave();
            // Host quit request; hosts only poll exit_requested.
            std::fprintf(stderr, "[runtime] exit requested\n");
            exit_requested_ = true;
            control_.exskip_active_ = false; // quit paths never leave it set
            continue;
        }
        // [reset] is the FPM go-title flow; raw [gotitle] only fires for data
        // without the Lua tag handler. Both use the same VM-kept reboot.
        if (e.kind == oa::runtime::Event::Kind::Reset) {
            reboot_boot_keep_vm("engine reset");
            return;
        }
        if (e.kind == oa::runtime::Event::Kind::GoTitle) {
            reboot_boot_keep_vm("gotitle");
            return;
        }
        keep.push_back(e);
    }
    pending_events_.swap(keep);
}

// ---------------------------------------------------------------------------
// Message-layer text domain:文本标签在派发点被归约进
// oa::render::TextEngine（print/chgmsg/rt/rp/font/ruby/scein-sceout/排版配置）。
// 已消费的事件不再进宿主事件流。chgmsg/pop 同步合成器消息层绑定（~ 路由）。
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::ensure_active_message_node() {
    if (!interpreter_) return;
    const std::string active = text().active_layer_id();
    if (active.empty()) return;
    const oa::render::MessageLayer* al = text().layer(active);
    if (!al) return;
    // 文本宿主就是这一层自己（消息 id == 节点 id）。layered 只决定
    // "文本是否跟着宿主树的显隐走"，不再有槽位/绑定表。
    scene().ensure_message_scene_node(active, al->layered);
    // 字体表没给 width 时，用节点自身的逻辑框宽兜底折行（消息窗节点的
    // width/clip 来自窗口纹理，内边距已由宿主扣除）。节点只绑了纹理、没有
    // 逻辑宽时再退到纹理宽（仍未扣边距，但比 1e18 永不折行正确）。
    if (const oa::render::Layer* node = scene().find(active)) {
        double bw = node->has_clip && node->clip_w > 0.0 ? node->clip_w : node->width;
        if (!(bw > 0.0) && oa::render::textured_content(*node)) {
            const auto& sizes = static_cast<const oa::render::ContentSizeSource&>(renderer_);
            if (const auto sz = sizes.decoded_size(oa::render::content_texture_key(*node)))
                bw = sz->first;
        }
        if (bw > 0.0) text().set_message_box_width(active, bw);
    }
}

bool GameRuntime::RuntimeState::apply_text_event(const oa::runtime::Event& e) {
    using K = oa::runtime::Event::Kind;
    auto par = [&e](const char* k) -> std::string {
        const auto it = e.params.find(k);
        return it == e.params.end() ? std::string() : it->second;
    };
    switch (e.kind) {
        case K::ScenarioLine: {
            renderer_.note(RNote::Text);
            const auto* prev = text().layer(text().active_layer_id());
            const bool new_page = !prev || prev->char_count == 0;
            text().push_text(e.content);
            // every drawable message layer owns a scene node — content
            // pushed before any chgmsg (default/active layer) materializes it
            // lazily, so glyphs have exactly one paint point (their node's
            // slot) and no scene-wide text fallback pass is needed.
            ensure_active_message_node();
            if (new_page && !e.content.empty() && interpreter_) {
                const std::string active = text().active_layer_id();
                const bool ui_text = active.rfind("101.sblock", 0) == 0 ||
                                     active.rfind("100.", 0) == 0 ||
                                     active.rfind("1000.", 0) == 0;
                const std::string* cs = interpreter_->current_script();
                bool story_frame = cs && !oa::runtime::is_framework_script(*cs);
                for (const auto& f : interpreter_->call_stack())
                    if (!f.script.empty() && !oa::runtime::is_framework_script(f.script))
                        story_frame = true;
                if (story_frame && !ui_text) interpreter_->note_page_printed(active);
            }
            return true;
        }
        case K::LineBreak: {
            // [rt]：omitblankline=1（缺省 true）时末行为空行则不另起
            const std::string ob = par("omitblankline");
            const bool omit = ob.empty() || parse_i32(ob, 1) != 0;
            text().line_break(omit, e.params);
            return true;
        }
        case K::PageBreak: {
            std::optional<int> bl;
            const std::string bp = par("backlog");
            if (!bp.empty()) bl = parse_i32(bp, 0);
            // 回顾表的一页 = 这一页的**重放命令**（print/font/rt…）。
            // 跳转时框架用它自己的载体（backlog_N.dat / log.stack / blj）
            // 重新打印，引擎不在这里存画面。
            text().page_break(bl);
            return true;
        }
        case K::MessageLayerSwitch: {
            renderer_.note(RNote::MessageSwitch);
            // 参数 id/stack/layered（id 缺省 → 引擎生成匿名 id）
            const std::string id = par("id");
            std::optional<std::string> mid;
            if (!id.empty()) mid = id;
            const std::string st = par("stack");
            const bool stack = st.empty() || parse_i32(st, 1) != 0;
            std::optional<int> layered;
            const std::string ly = par("layered");
            if (!ly.empty()) layered = parse_i32(ly, -1);
            text().switch_layer(mid, stack, layered);
            // 绑定：活动消息层 id → 场景目标 + 默认消息层
            const std::string active = text().active_layer_id();
            const oa::render::MessageLayer* al = text().layer(active);
            const bool is_layered = al && al->layered;
            // 槽位显式化 —— layered=0 独立消息分配引擎消息槽(创建序),
            // layered=1 走宿主树节点(ensure_path 物化,父链位姿语义不变:
            // glyph 落点=ml.left/top+排版偏移,经节点世界变换合成)。
            scene().ensure_message_scene_node(active, is_layered);
                    scene().set_default_message_layer(active);
            // Publish s.current_message_layer for framework guards.
            if (interpreter_)
                interpreter_->set_variable("s.current_message_layer",
                                           oa::runtime::Value::make_string(active));
            return true;
        }
        case K::MessageLayerPop: {
            text().pop_layer();
            const std::string active = text().active_layer_id();
            const oa::render::MessageLayer* al = text().layer(active);
            if (al) {
                const bool is_layered = al->layered;
                const std::string scene_id =
                    scene().ensure_message_scene_node(active, is_layered);
                        }
            scene().set_default_message_layer(active);
            if (interpreter_)
                interpreter_->set_variable("s.current_message_layer",
                                           oa::runtime::Value::make_string(active));
            return true;
        }
        case K::TextConfig: {
            // font 族 / ruby / scein-sceout / 排版配置标签（ +
            // tags/ 文本面）
            if (e.tag == "font") {
                text().apply_font(e.params);
                return true;
            }
            if (e.tag == "font_close" || e.tag == "/font") {
                text().font_close();
                return true;
            }
            if (e.tag == "fontinit") {
                text().font_init();
                return true;
            }
            if (e.tag == "fontdefault") {
                text().font_default(e.params);
                return true;
            }
            if (e.tag == "ruby") {
                text().ruby_start(par("text"));
                return true;
            }
            if (e.tag == "/ruby") {
                text().ruby_end();
                return true;
            }
            if (e.tag == "scein") {
                text().set_text_hidden(false);
                return true;
            }
            if (e.tag == "sceout") {
                text().set_text_hidden(true);
                return true;
            }
            if (e.tag == "prohibit") {
                text().set_prohibit(par("head"), par("foot"));
                return true;
            }
            if (e.tag == "wordparts") {
                text().set_wordparts(par("parts"));
                return true;
            }
            if (e.tag == "indent") {
                text().set_indent(par("pair"));
                return true;
            }
            if (e.tag == "scetween") {
                text().apply_scetween(e.params);
                return true;
            }
            if (e.tag == "glyph") {
                text().set_glyph_config(e.params);
                return true;
            }
            if (e.tag == "backlog") {
                text().apply_backlog_config(e.params, par("clear") == "1");
                return true;
            }
            if (e.tag == "writebacklog") {
                const std::string m = par("mode");
                text().set_backlog_write_mode(!m.empty() && parse_i32(m, 0) != 0);
                return true;
            }
            // rt2/tximg/txkey/txnc/link… 仍 no-op（后续域/队列）
            return false; // 交给宿主（诊断/后续域）
        }
        default:
            return false;
    }
}

void GameRuntime::RuntimeState::apply_lyc2_csv(std::map<std::string, std::string>* props) {
    const auto fit = props->find("file");
    if (fit == props->end() || fit->second.empty()) return;
    // An explicit geometry on the tag wins (the framework sometimes overrides
    // a csv-placed layer inline).
    if (props->count("left") || props->count("top") || props->count("width") ||
        props->count("height"))
        return;
    if (!interpreter_ || !interpreter_->hooks().resource_read) return;
    const std::string csv = fit->second + ".csv";
    std::optional<std::vector<uint8_t>> bytes;
    if (auto b = interpreter_->hooks().resource_read(interpreter_->resolve_magic_path(csv)))
        bytes = std::move(b);
    else if (csv.rfind("_data/", 0) != 0 && csv[0] != ':')
        bytes = interpreter_->hooks().resource_read("_data/" + csv);
    if (!bytes) return;
    const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    // Two shapes in the wild: positional "left,top" (the mw plates) and
    // key=value rows (`left = 290`). Both are merged as layer props; comment
    // rows and blanks are skipped.
    std::vector<std::string> nums;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find_first_of("\r\n", pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty() || (line.size() > 1 && line[0] == '/' && line[1] == '/')) continue;
        if (const size_t eq = line.find('='); eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
            if (const size_t sp = val.find_first_of(" \t"); sp != std::string::npos)
                val.resize(sp); // drop a trailing // comment
            if (!key.empty() && !val.empty()) (*props)[key] = val;
            continue;
        }
        size_t p = 0;
        while (p <= line.size()) {
            size_t c = line.find(',', p);
            if (c == std::string::npos) c = line.size();
            std::string tok = line.substr(p, c - p);
            while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(0, 1);
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
            if (!tok.empty()) nums.push_back(tok);
            if (c >= line.size()) break;
            p = c + 1;
        }
    }
    static const char* kOrder[] = {"left", "top", "width", "height", "xscale",
                                   "yscale", "alpha", "rotate"};
    for (size_t i = 0; i < nums.size() && i < sizeof(kOrder) / sizeof(kOrder[0]); ++i)
        if (!props->count(kOrder[i])) (*props)[kOrder[i]] = nums[i];
}

bool GameRuntime::RuntimeState::apply_scene_event(const oa::runtime::Event& e) {
    using K = oa::runtime::Event::Kind;
    if (e.kind == K::LayerCreate) {
        std::map<std::string, std::string> props = e.params;
        if (e.tag == "lyc2") apply_lyc2_csv(&props);
        scene().create(e.id, props);
        renderer_.note(RNote::Layer);
        return true;
    }
    if (e.kind == K::LayerDelete) {
        // 文本内容随节点一起结束（Compositor::remove 的删除通知接线，
        // 见 RenderEngine::configure）；这里不再单独"隐藏"文本层。
        scene().remove(e.id);
        renderer_.note(RNote::Layer);
        // a deleted layer (or subtree) releases its emote
        // player (fgdel/lydel2 flows) — players whose id equals the deleted
        // id or sits below it are dropped (textures die with the scene too).
        for (auto it = media_.emote_layers_.begin(); it != media_.emote_layers_.end();) {
            const std::string& id = it->first;
            if (id == e.id || (id.size() > e.id.size() + 1 &&
                               id.compare(0, e.id.size(), e.id) == 0 &&
                               id[e.id.size()] == '.')) {
                if (it->second.player) {
                    media_.emote_view_cache_[id] = it->second.player->view_state();
                    // Park the player for the immediate same-beat rebuild.
                    if (media_.parked_emote_.size() >= 8) media_.parked_emote_.erase(media_.parked_emote_.begin());
                    // Key by PSB file: scene-id rebuilds keep the expression.
                    media_.parked_emote_[it->second.file] = std::move(it->second);
                }
                it = media_.emote_layers_.erase(it);
            } else {
                ++it;
            }
        }
        // A deleted subtree releases its layer-video channels.
        media_.video_.stop_layer_subtree(e.id);
        return true;
    }
    if (e.kind == K::LayerSetProps) {
        // routed: id == "!" hits the root props, never creates a "!" node.
        if (e.id == "!") {
            scene().set_root_props(e.params);
        } else {
            scene().set_props(e.id, e.params);
        }
        renderer_.note(RNote::Layer);
        return true;
    }
    if (e.kind == K::LayerEventCmd) {
        // Animation tags live on the Compositor and are
        // part of the compositor boundary (compositor/TweenSet*).
        if (e.tag == "lyevent") {
            // layer-event handler registrations are applied to the scene
            // registry at dispatch time (FPM type-less handler
            // expansion). Host stream never sees them.
            scene().apply_lyevent(e.id, e.params);
            renderer_.note(RNote::Layer);
            return true;
        }
        if (e.tag == "lytween") {
            scene().apply_lytween(e.params, now_ms_);
            renderer_.note(RNote::Tween);
            // Collect the group's sync targets; [/tweenset] turns them into
            // one Tween park (wait every action).
            if (scene().tweenset_collecting() && !e.id.empty()) {
                int sync = 0;
                if (const auto it = e.params.find("sync"); it != e.params.end())
                    sync = parse_i32(it->second, 0);
                if (sync != 0 &&
                    std::find(park_.pending_sync_tweens.begin(), park_.pending_sync_tweens.end(),
                              e.id) == park_.pending_sync_tweens.end()) {
                    park_.pending_sync_tweens.push_back(e.id);
                }
            }
            return true;
        }
        if (e.tag == "lytweendel") {
            // Defer cancelled completions to the frame-end flush.
            std::vector<oa::render::TweenDone> done = scene().apply_lytweendel(e.id);
            if (!done.empty()) {
                PendingTweenCancel pc;
                pc.id = e.id;
                pc.node = scene().find(e.id);
                pc.done = std::move(done);
                pending_tween_cancels_.push_back(std::move(pc));
            }
            renderer_.note(RNote::Tween);
            return true;
        }
        if (e.tag == "anime") {
            // [anime] frame animation: init/add/end reduced to the
            // AnimeState on the Compositor.
            scene().apply_anime(e.params, now_ms_);
            renderer_.note(RNote::Tween);
            return true;
        }
        if (e.tag == "tweenset") {
            park_.pending_sync_tweens.clear();
            scene().tweenset_start();
            renderer_.note(RNote::Tween);
            return true;
        }
        if (e.tag == "/tweenset") {
            scene().tweenset_end(now_ms_);
            renderer_.note(RNote::Tween);
            return true;
        }
        return false;
    }
    return false;
}

// Control/config domain: legacy rows → input registry → config → [lydrag].
bool GameRuntime::RuntimeState::apply_control_event(const oa::runtime::Event& e) {
    using K = oa::runtime::Event::Kind;
    if (e.kind == K::ConfigEvent) {
        static const std::map<std::string, std::string> kLayerSet = {
            {"setonclick", "click"},       {"setondrag", "drag"},
            {"setondragin", "dragin"},     {"setondragout", "dragout"},
            {"setonrollover", "rollover"}, {"setonrollout", "rollout"}};
        static const std::map<std::string, std::string> kLayerDel = {
            {"delonclick", "click"},       {"delondrag", "drag"},
            {"delondragin", "dragin"},     {"delondragout", "dragout"},
            {"delonrollover", "rollover"}, {"delonrollout", "rollout"}};
        if (const auto it = kLayerSet.find(e.tag); it != kLayerSet.end()) {
            scene().apply_legacy_layer_event(e.id, it->second, false, e.params);
            renderer_.note(RNote::Layer);
            return true;
        }
        if (const auto it = kLayerDel.find(e.tag); it != kLayerDel.end()) {
            scene().apply_legacy_layer_event(e.id, it->second, true, e.params);
            renderer_.note(RNote::Layer);
            return true;
        }
        static const std::map<std::string, std::string> kInputSet = {
            {"setonpush", "push"},         {"setonautomodein", "automodein"},
            {"setonautomodeout", "automodeout"}, {"setonbacklogin", "backlogin"},
            {"setonbacklogout", "backlogout"},   {"setoncommandskipin", "commandskipin"},
            {"setoncommandskipout", "commandskipout"},
            {"setoncontrolskipin", "controlskipin"},
            {"setoncontrolskipout", "controlskipout"}, {"setondirchg", "dirchg"},
            {"setonhidein", "hidein"},     {"setonhideout", "hideout"},
            {"setonwindowbutton", "windowbutton"}};
        static const std::map<std::string, std::string> kInputDel = {
            {"delonpush", "push"},         {"delonautomodein", "automodein"},
            {"delonautomodeout", "automodeout"}, {"delonbacklogin", "backlogin"},
            {"delonbacklogout", "backlogout"},   {"deloncommandskipin", "commandskipin"},
            {"deloncommandskipout", "commandskipout"},
            {"deloncontrolskipin", "controlskipin"},
            {"deloncontrolskipout", "controlskipout"}, {"delondirchg", "dirchg"},
            {"delonhidein", "hidein"},     {"delonhideout", "hideout"},
            {"delonwindowbutton", "windowbutton"}};
        if (const auto it = kInputSet.find(e.tag); it != kInputSet.end()) {
            std::map<std::string, std::string> raw = e.params;
            // setonwindowbutton indexes by "button" unless key was given.
            if (e.tag == "setonwindowbutton" && !raw.count("key")) {
                if (const auto b = raw.find("button"); b != raw.end()) raw["key"] = b->second;
            }
            scene().set_input_handler(it->second, raw);
            return true;
        }
        if (const auto it = kInputDel.find(e.tag); it != kInputDel.end()) {
            std::string key;
            const auto k = e.params.find("key");
            if (k != e.params.end()) key = k->second;
            if (e.tag == "delonwindowbutton" && key.empty()) {
                const auto b = e.params.find("button");
                if (b != e.params.end()) key = b->second;
            }
            scene().del_input_handler(it->second, key);
            return true;
        }
        // control domain: [keyconfig]/[rclick]/[hide]/[skip]/[automode]/
        // [exec] consumed by the runtime control state machine
        // (apply_control_config, below).
        if (apply_control_config(e)) return true;
        return false;
    }
    if (e.kind == K::Custom && e.tag == "lydrag") {
        // [lydrag]: force drag regardless of draggable/hover.
        // The scene event consumed; host never sees it.
        const auto it = e.params.find("id");
        force_pointer_drag(it == e.params.end() ? "" : it->second);
        return true;
    }
    return false;
}

void GameRuntime::RuntimeState::dispatch_tween_done(
    const std::vector<oa::render::TweenDone>& done) {
    // Enqueue tween completion handlers as engine tags (handler + jump).
    for (const oa::render::TweenDone& d : done) {
        if (!d.handler.empty() || !d.handler_file.empty() || !d.handler_label.empty()) {
            renderer_.note(RNote::TweenComplete);
        }
        if (!d.handler.empty()) {
            interpreter_->enqueue_tag(d.handler, d.extra);
        }
        if (!d.handler_file.empty() || !d.handler_label.empty()) {
            std::map<std::string, std::string> jump;
            if (!d.handler_file.empty()) jump["file"] = d.handler_file;
            if (!d.handler_label.empty()) jump["label"] = d.handler_label;
            interpreter_->enqueue_tag("jump", std::move(jump));
        }
    }
}

void GameRuntime::RuntimeState::flush_pending_tween_cancels() {
    // Deferred [lytweendel] completions. 只要这个 id 的层还在，就把完成回调
    // 交出去——**包括这一帧里被重建过的层**（`[lyc2]` 重新声明同一个 id 时
    // 节点指针会变）。框架把 `[lytweendel]` 当"这一轮结束了"用：NekoMiko 的
    // 配置页样本文字靠 `config_samplestart()` 先 del 再起一个新 tween 循环，
    // 层被重建时把回调吞掉，样本文字就再也不出现（调完速度后"文字不再显示"）。
    // 层真的没了（`[lydel]`）才静默丢弃：那时候回调写进去也没有落点。
    if (pending_tween_cancels_.empty()) return;
    std::vector<oa::render::TweenDone> out;
    for (auto& pc : pending_tween_cancels_) {
        if (scene().find(pc.id)) {
            for (auto& d : pc.done) out.push_back(std::move(d));
        }
    }
    pending_tween_cancels_.clear();
    dispatch_tween_done(out);
}

void GameRuntime::RuntimeState::drop_pending_tween_cancels() {
    pending_tween_cancels_.clear();
}

// Runtime-side frame advance (host upload pumps / group bakes stay outside).
// R6 order: text reveal → scene tweens/anime → emote clock → click-wait icon
// → media frame end. Reordering changes animation phase.
void GameRuntime::RuntimeState::advance_content_planes(uint64_t delta_ms) {
    // 逐字用绝对时钟（不是 delta 累加）：一帧卡多久都不会让文字永久落后，
    // 改速度也只是换速率、从当前位置继续。
    text().advance(now_ms_);
    advance_scene_frame();
    advance_emote_players(delta_ms); // host_clock layers only sync revisions
    advance_click_wait();
    advance_media_frame(delta_ms);
}

void GameRuntime::RuntimeState::advance_scene_frame() {
    // Deliver staged completions only while the tween's layer survived the
    // frames in between; then stage this frame's natural finishes for the
    // next flush (a config page rebuild must not reprint the old preview).
    flush_pending_tween_cancels();
    std::vector<oa::render::TweenDone> done = scene().advance_tweens(now_ms_);
    if (!done.empty()) {
        for (oa::render::TweenDone& d : done) {
            PendingTweenCancel pc;
            pc.id = d.id;
            pc.node = scene().find(d.id);
            pc.done.push_back(std::move(d));
            pending_tween_cancels_.push_back(std::move(pc));
        }
    }
}

// ---------------------------------------------------------------------------
// transitions
// ---------------------------------------------------------------------------

void GameRuntime::RuntimeState::transition_begin(const std::map<std::string, std::string>& params) {
    auto get = [&params](const char* k, const char* dflt) -> std::string {
        const auto it = params.find(k);
        return it == params.end() ? std::string(dflt) : it->second;
    };
    int type = 1;
    std::optional<uint64_t> time;
    std::string rule = get("rule", "");
    std::optional<int> vague;
    int input = 1;
    try {
        type = std::stoi(get("type", "1"));
    } catch (...) {
        type = 1;
    }
    try {
        if (!get("time", "").empty()) time = uint64_t(std::stoll(get("time", "0")));
    } catch (...) {
        time.reset();
    }
    try {
        if (!get("vague", "").empty()) vague = std::stoi(get("vague", "32"));
    } catch (...) {
        vague.reset();
    }
    try {
        input = std::stoi(get("input", "1"));
    } catch (...) {
        input = 1;
    }
    transition().start(type, time, std::move(rule), vague, input, now_ms_);
    renderer_.request_repaint();
    if (transition().needs_capture()) {
        if (transition_capture_cb_) {
            transition_capture_cb_(); // test host snapshots the old frame
        } else {
            // Renderer owns the transition capture; without a backend the
            // capture is treated as complete (headless duration still runs).
            renderer_.capture_transition_source();
            transition().mark_captured(now_ms_);
        }
    }
}

bool GameRuntime::RuntimeState::waiting_stop() const {
    return park_.wait && !park_.wait->has(oa::runtime::Wait::WakeClick);
}

// Debug-skip fast-forward: non-Stop waits release instantly; the
// [stop 0="exskip"] row ends the fast-forward. Details: docs/RUNTIME_NOTES.md.
void GameRuntime::RuntimeState::start_debug_skip(int64_t index) {
    if (control_.exskip_active_) return;
    control_.exskip_active_ = true;
    control_.exskip_target_ = index > 0 ? index : -1;
    control_.exskip_script_.clear();
}

void GameRuntime::RuntimeState::end_debug_skip() {
    if (!control_.exskip_active_) return;
    control_.exskip_active_ = false;
    control_.exskip_target_ = -1;
    control_.exskip_script_.clear();
    reveal_all_text(); // no half-revealed page left behind at the boundary
    if (interpreter_) {
        try {
            // Mirror the real engine's Lua tags.stop hook side effect (sets
            // flg.exskip=2 so exskip_end's guard passes); then fire the
            // registered handler (onDebugSkipOut -> exskip_end).
            interpreter_->lua_bridge().run_code(
                "if type(flg)=='table' and flg.exskip then flg.exskip = 2 end",
                "exskip-boundary-mark");
            interpreter_->fire_event("onDebugSkipOut");
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[exskipdbg] boundary handler error: %s\n", ex.what());
        }
    }
}

bool GameRuntime::RuntimeState::fire_skip(oa::runtime::Wait& w, bool force) {
    using W = oa::runtime::Wait;
    // skipMode / debug exskip always shows the page instantly before the
    // presentation-specific stop action.
    if (!text().is_reveal_complete()) text().reveal_all();
    switch (w.stage) {
        case W::Stage::Trans:
            if (force) {
                transition().clear();
                return true;
            }
            return transition().skip_by_input(true);
        case W::Stage::Video: {
            const auto vsnap = media_.video_.state();
            const auto& fsv = vsnap.overlay_video;
            if (!fsv || !fsv->playing || !fsv->decoded) return true;
            if (force) {
                media_.video_.stop_overlay();
                return true;
            }
            if (fsv->skippable) {
                // Go through the finish handler: the game's onVideoFinish is
                // what runs the transition after the logo movie.
                if (!w.finish_requested) {
                    media_.video_.queue_finish(std::string());
                    w.finish_requested = true;
                }
            }
            return false;
        }
        case W::Stage::VideoLayer: {
            const auto vsnap = media_.video_.state();
            const auto it = vsnap.video_layers.find(w.wake_id);
            if (it == vsnap.video_layers.end() || !it->second.playing || !it->second.decoded)
                return true;
            if (force) {
                media_.video_.stop_layer(w.wake_id);
                return true;
            }
            if (it->second.skippable) {
                if (!w.finish_requested) {
                    media_.video_.queue_finish(w.wake_id);
                    w.finish_requested = true;
                }
            }
            return false;
        }
        case W::Stage::Se:
            media_.audio_.stop_se(w.wake_id, 0);
            return true;
        case W::Stage::Tween:
            if (!w.tween_ids.empty()) {
                for (const auto& id : w.tween_ids) (void)scene().apply_lytweendel(id);
            } else {
                (void)scene().apply_lytweendel(w.wake_id);
            }
            return true;
        case W::Stage::AllDelete:
            finish_all_delete();
            return true;
        case W::Stage::None:
            return true;
    }
    return true;
}

bool GameRuntime::RuntimeState::fire_click(oa::runtime::Wait& w, bool from_decide) {
    using W = oa::runtime::Wait;
    (void)from_decide; // a scripted decide is the framework's fake click
    switch (w.stage) {
        case W::Stage::Trans:
            // input policy 0/2 still decides whether the transition may be cut
            return transition().skip_by_input(false);
        case W::Stage::Video: {
            const auto vsnap = media_.video_.state();
            const auto& fsv = vsnap.overlay_video;
            if (!fsv || !fsv->playing || !fsv->decoded) return true;
            if (fsv->skippable) {
                if (!w.finish_requested) {
                    media_.video_.queue_finish(std::string());
                    w.finish_requested = true;
                }
            }
            return false;
        }
        case W::Stage::VideoLayer: {
            const auto vsnap = media_.video_.state();
            const auto it = vsnap.video_layers.find(w.wake_id);
            if (it == vsnap.video_layers.end() || !it->second.playing || !it->second.decoded)
                return true;
            if (it->second.skippable) {
                if (!w.finish_requested) {
                    media_.video_.queue_finish(w.wake_id);
                    w.finish_requested = true;
                }
            }
            return false;
        }
        case W::Stage::Se:
            media_.audio_.stop_se(w.wake_id, 0);
            return true;
        case W::Stage::Tween:
            if (!w.tween_ids.empty()) {
                for (const auto& id : w.tween_ids) (void)scene().apply_lytweendel(id);
            } else {
                (void)scene().apply_lytweendel(w.wake_id);
            }
            return true;
        case W::Stage::AllDelete:
            return false; // the whole-scene fade is not click-skippable
        case W::Stage::None:
            return true;
    }
    return true;
}

bool GameRuntime::RuntimeState::resolve_wait(uint64_t delta_ms, bool click_cmd,
                                             bool decide_cmd) {
    // 緊急回避中は物語を進めない（原版も vsync が本体ループに入らない）。
    if (control_.avoid_active_) return false;
    if (!park_.wait) return true;
    oa::runtime::Wait& w = *park_.wait;
    using W = oa::runtime::Wait;
    const bool armed_now = (w.armed_tick == tick_count_);
    // A park restored from a call frame re-enters on the tick that closed the
    // UI; that tick's press/decide already belonged to the UI.
    const bool restored_now = w.restored && armed_now;
    const bool skipping = control_.skip_active_ || control_.exskip_active_;
    const bool force_skip = control_.exskip_active_;

    // [stop 0="exskip"] ends the debug fast-forward at its boundary.
    if (w.has(W::WakeExskipEnd)) {
        if (control_.exskip_active_) end_debug_skip();
        return true;
    }
    // skip wake: stop the presentation, then resume. A non-skippable
    // video/transition stays parked (the completion/click wakes still apply).
    // Artemis page rule: a skip on a still-revealing click page completes the
    // text first and advances on the next tick, not both at once.
    if (skipping && w.has(W::WakeSkip)) {
        const bool page_click =
            w.stage == W::Stage::None && !w.timed() && w.keys.empty() &&
            !w.on_reveal_done && !w.reveal_clear && w.has(W::WakeClick);
        // The "complete the page, advance on the next tick" rule exists so one
        // human press is not spent on two commands. A forced fast-forward has
        // no press to conserve: completing the reveal and releasing the page in
        // the same tick is what keeps a backlog quick-jump inside one tick
        // instead of flashing one intermediate page per frame.
        if (page_click && !text().is_reveal_complete() && !force_skip) {
            text().reveal_all();
        } else if (fire_skip(w, force_skip)) {
            return true;
        }
    }
    // One physical command: decide (scripted click) wins over the raw click.
    // A raw mouse press must still skip a skippable video even when a UI
    // layer/push row consumed the normalized click (the opening logo movie
    // sets keyconfig role=1 ALL; the push row otherwise eats the click).
    const bool phys_click = input_state_.down_edges_.count(kKeyMouseLeft) > 0 ||
                            input_state_.down_edges_.count(kKeyMouseRight) > 0;
    const bool video_stage =
        w.stage == W::Stage::Video || w.stage == W::Stage::VideoLayer;
    if (!restored_now && decide_cmd && w.has(W::WakeDecide)) {
        if (fire_click(w, true)) return true;
    } else if (!restored_now && (click_cmd || (phys_click && video_stage)) &&
               w.has(W::WakeClick)) {
        if (fire_click(w, false)) return true;
    }
    // [exkey]: these key edges release the park.
    if (!restored_now && w.has(W::WakeKeys)) {
        for (const int k : w.keys) {
            if (input_state_.down_edges_.count(k)) return true;
        }
    }
    // Text wakes.
    if (w.has(W::WakeRevealClear)) {
        text().reveal_all();
        return true;
    }
    if (w.has(W::WakeRevealDone) && text().is_reveal_complete()) return true;
    // Clock wakes never resolve on their arm tick (Artemis yields ≥1 frame).
    if (w.has(W::WakeTimer) && !armed_now && now_ms_ >= w.deadline_ms) return true;
    // Auto mode: per-park dwell; a manual click cannot leave a partial timer
    // behind for the next page (KAG restarts the timeout on every wait).
    if (w.has(W::WakeAuto)) {
        if (!control_.automode_) {
            w.auto_elapsed_ms = 0;
        } else {
            w.auto_elapsed_ms += delta_ms;
            if (w.auto_elapsed_ms >= automode_wait_ms() && !automode_sync_blocked())
                return true;
        }
    }
    // [wait name=] / [trigger name=].
    if (!park_.pending_custom.empty()) {
        if (w.has(W::WakeCustom) && park_.pending_custom == w.wake_id) {
            park_.pending_custom.clear();
            return true;
        }
        park_.pending_custom.clear(); // no waiter for this trigger
    }
    // Presentation completion wakes (state-polled; the wait owns identity).
    switch (w.stage) {
        case W::Stage::Trans:
            return !transition().is_in_progress(now_ms_);
        case W::Stage::Video: {
            const auto vsnap = media_.video_.state();
            const auto& fsv = vsnap.overlay_video;
            return !fsv || !fsv->playing || !fsv->decoded;
        }
        case W::Stage::VideoLayer: {
            const auto vsnap = media_.video_.state();
            const auto it = vsnap.video_layers.find(w.wake_id);
            return it == vsnap.video_layers.end() || !it->second.playing || !it->second.decoded;
        }
        case W::Stage::Se:
            return se_wait_finished(w);
        case W::Stage::AllDelete:
            return !renderer_.all_delete_active();
        case W::Stage::Tween:
            if (!w.tween_ids.empty()) {
                for (const auto& id : w.tween_ids) {
                    if (!scene().layer_tweens_finished(id)) return false;
                }
                return true;
            }
            return scene().layer_tweens_finished(w.wake_id);
        case W::Stage::None:
            break;
    }
    return false;
}

void GameRuntime::RuntimeState::advance_wait() {
    // 读档/UI 会话残链可能把活动消息层留在 UI 层（500.pageno 等），而剧本
    // 翻页分派（script.asb 的 chgmsg/pop/rp 清页模式）假定点击停驻释放时
    // 活动层 == 当前剧情文本层——错层导致清页清错层、新文本叠旧页后且
    // reveal 冻结（NekoMiko 读档后"剧情推进但文字不动"）。
    // 在页面等待释放（一次翻页/一次 UI 交互的起点）时把活动层锚回内容层。
    const bool page_wait_release = park_.wait && park_.wait->waiting_click();
    const bool script_owned = park_.wait && park_.wait->script_owned;
    const bool has_pending = !park_.pending.empty();
    // 停驻只在它武装的那一行有效（同 pop_park_context / Wait::owner_*）。
    // 点击先派发给层，层里的 [jump]/[call] 可能已经把脚本挪到别的流程
    // （backlog 的「戻る」按钮：leave 的 [returns] 已经把位置交还给
    // menu.iet 的 `[jump leave]`）。这时再 +1 会把落点的第一条指令跳过，
    // UI 收尾于是接着往下跑进别的分支。同一帧新武装的停驻也不吃这一下：
    // 那一次按下的所有权已经属于刚关闭的 UI。
    const bool on_owner_line =
        park_.wait && interpreter_->current_script() &&
        park_.wait->owner_script == *interpreter_->current_script() &&
        park_.wait->owner_line == interpreter_->current_line();
    const bool armed_now = park_.wait && park_.wait->armed_tick == tick_count_;
    park_.wait.reset();
    if (page_wait_release) text().anchor_active_to_content();
    // 这一层是脚本自己的停驻 → 光标 +1；队列停驻（`script_owned=false`）的
    // 光标已经指向它的 tag 之后，不用动。
    //
    // **不能因为 `has_pending` 就不加**：待定的队列停驻永远是 `script_owned=
    // false`（arm_park(..., false)），它们自己永远不会补这一次 +1。以前这里
    // 写的是 `script_owned && !has_pending`，于是"关掉 UI 后第一次点击"被
    // 队尾的那个 uitrans 待定停驻花掉、剧情行不动，表现就是"文字没有刷新"
    // （NekoMiko 进设置再回来：点多少下都停在同一行）。
    // 先 +1 再让待定停驻接管是安全的：脚本仍被那个停驻挡住，下一行不会提前跑。
    interpreter_->release_park(script_owned && on_owner_line && !armed_now);
    // The held script tail belongs after this wait: run it before any tag that
    // arrived while the park was live.
    prepend_live_tail();
    if (has_pending) {
        ParkedWait next = std::move(park_.pending.front());
        park_.pending.pop_front();
        const bool next_owned = next.wait.script_owned;
        arm_park(std::move(next.wait), next_owned);
        park_.tail = std::move(next.tail);
    }
}

// [glyph] click-wait icon: show/move while a click wait is fully revealed.
void GameRuntime::RuntimeState::advance_click_wait() {
    const bool click_wait = park_.wait && park_.wait->waiting_click();
    const bool show = click_wait && text().is_reveal_complete();
    if (!show) {
        if (!control_.active_wait_icon_.empty()) {
            // find guard: set_props would materialize a missing icon layer.
            if (scene().find(control_.active_wait_icon_)) {
                scene().set_props(control_.active_wait_icon_, {{"visible", "0"}});
            }
            control_.active_wait_icon_.clear();
        }
        return;
    }
    const auto placement = text().click_wait_placement(false);
    if (!placement) return;
    const bool homing = text().glyph_icon().homing;
    const oa::render::Layer* l = scene().find(placement->layer_id);
    const bool already = control_.active_wait_icon_ == placement->layer_id && l && l->visible &&
                         (!homing ||
                          (l->left == placement->left && l->top == placement->top));
    if (already) return;
    if (!control_.active_wait_icon_.empty() && control_.active_wait_icon_ != placement->layer_id &&
        scene().find(control_.active_wait_icon_)) {
        scene().set_props(control_.active_wait_icon_, {{"visible", "0"}});
    }
    std::map<std::string, std::string> p{{"visible", "1"}};
    if (homing) {
        p["left"] = format_prop_num(placement->left);
        p["top"] = format_prop_num(placement->top);
    }
    scene().set_props(placement->layer_id, p);
    control_.active_wait_icon_ = placement->layer_id;
}

// Pointer chain: hit test → hover diff → click/drag/push (docs/RUNTIME_NOTES.md).

namespace {
bool is_mouse_button_key(int key) { return key >= 1 && key <= 3; }
bool key_has_drag_handler(const oa::render::Compositor& scene, const std::string& id) {
    for (const char* type : {"drag", "dragin", "dragout"}) {
        const auto* h = scene.find_event_handler(id, type);
        if (h && h->enabled) return true;
    }
    return false;
}
} // namespace

GameRuntime::RuntimeState::PressDecision GameRuntime::RuntimeState::process_pointer_input(
    const FrameInput& in) {
    PressDecision d;
    if (!interpreter_) return d;

    const bool left_down_edge = in.left_click_edge;
    const bool left_up_edge = pointer_.left_down_prev_ && !in.left_down;
    pointer_.left_down_prev_ = in.left_down;
    const bool enter_space = input_state_.down_edges_.count(kKeyEnter) > 0 ||
                             input_state_.down_edges_.count(kKeySpace) > 0;
    // 一次物理按下 = 一张只能被认领一次的票。
    d.press = left_down_edge || enter_space;

    // 脚本用 `e:overrideKey{status=0}` 吞掉本帧输入（vsync 的 btstop /
    // 按键检查 / UI 打开）。左键谁都不给；Enter/Space 仍算一次"决定"
    // （原版如此：overrideKey 关的是指针，不是键盘）。
    if (input_state_.swallowed_frame_) {
        if (left_down_edge) d.owner = PressDecision::Owner::Engine;
        else if (enter_space) d.owner = PressDecision::Owner::Script;
        return d;
    }

    const double mx = double(in.mouse_x);
    const double my = double(in.mouse_y);
    const bool pointer_moved =
        !pointer_.mouse_inited_ || in.mouse_x != pointer_.mouse_prev_x_ || in.mouse_y != pointer_.mouse_prev_y_;
    pointer_.mouse_inited_ = true;
    pointer_.mouse_prev_x_ = in.mouse_x;
    pointer_.mouse_prev_y_ = in.mouse_y;

    // 緊急回避（スクランブル）中はポインタを一切当てない。画像が全画面を
    // 覆っているので hover/click の行き先は無い。終了キー（role 16）だけは
    // 键位处理に渡す。
    if (control_.avoid_active_) {
        d.owner = PressDecision::Owner::Engine;
        (void)dispatch_key_edges();
        return d;
    }

    // hide mode: a left click first recovers the message window and is
    // swallowed for the rest of the frame (hide click recovery, no
    // click/advance chain).
    if (control_.hide_active_ && left_down_edge) {
        exit_hide_mode();
        d.owner = PressDecision::Owner::Engine;
        return d;
    }

    // 一次命中扫描，三个消费者共用：owner（拖拽锚点）、click 链、hover 链。
    const auto probe = scene().probe_pointer(mx, my, pointer_.hit_size_,
                                             pointer_.compositor_userdata_,
                                             pointer_.alpha_sampler_);

    // --- hover：按**层实例**记账，不按 id -------------------------------
    //
    // 脚本的 `[lydel]`+`[lyc]` 会把同一个 id 的层整棵换掉（csvbtn 每次开
    // 界面都这么干）。按 id 记 hover 的话，新层一出生就被当成"已经
    // hover 着"，`rollover` 永远不重发；它的 handler 却是新的。玩家看到的
    // 就是"有时有反应、有时没反应"。带上实例代号，重建 = 新对象 = 该重新
    // 进一次。
    if (pointer_moved) {
        std::map<std::string, uint32_t> next;
        // (a) 仍在指针下的层：代号没变才算留任，否则按"新层"重发 over
        for (const std::string& id : probe.rollover) { // bottom -> top
            const uint32_t gen = scene().generation_of(id);
            const auto it = pointer_.hovered_.find(id);
            if (it != pointer_.hovered_.end() && it->second == gen) {
                next[id] = gen;
                continue;
            }
            const auto* row = scene().find_event_handler(id, "rollover");
            if (row) {
                dispatch_layer_row(id, *row, "rollover", {},
                                   PointerDispatch::Kind::HoverIn);
            }
            next[id] = gen;
        }
        // (b) 离开指针（含被重建掉的旧实例）：发 rollout
        //
        // "离开"的判据是**指针离开这一层的矩形**，不是"这层不再是最顶层"。
        // 反例就是消息窗底部的 dock 触发条：`1.80.mw.-1` 是一整条透明热区，
        // dock 展开后按钮正好盖在它上面。指针从热区移到按钮上时，按钮成了
        // topmost，热区若按"不再最顶层"判就立刻收到 rollout → 关 dock →
        // 指针又落回热区 → 再开……每帧一次，玩家看到的就是菜单栏一闪一闪
        // 地躲开。它明明还在 hits 里（指针确实在矩形内、也没被 alpha 判成
        // 透明），就不该算离开；rollout 留到真正移出矩形的那一帧。
        std::vector<std::string> leave;
        for (const auto& [id, gen] : pointer_.hovered_) {
            if (next.count(id)) continue;
            const bool still_under_pointer =
                std::find(probe.hits.begin(), probe.hits.end(), id) != probe.hits.end();
            const auto* row = scene().find_event_handler(id, "rollover");
            if (still_under_pointer && row && row->enabled &&
                scene().generation_of(id) == gen) {
                next[id] = gen; // 被上层盖住但指针没离开：保持 hover
                continue;
            }
            leave.push_back(id);
        }
        std::sort(leave.begin(), leave.end());
        for (const std::string& id : leave) {
            const auto* row = scene().find_event_handler(id, "rollout");
            if (!row) continue;
            // 旧实例已经没了：仍然通知脚本"指针离开了"，但事件行按当前
            // 节点取（脚本用它复位按钮态）。
            dispatch_layer_row(id, *row, "rollout", {},
                               PointerDispatch::Kind::HoverOut);
        }
        pointer_.hovered_ = std::move(next);
    } else {
        // 指针没动：节点被删/重建也要掉出 hover。代号比较是唯一判据 ——
        // "id 还在"说明不了什么（同 id 可能是新层）。
        for (auto it = pointer_.hovered_.begin(); it != pointer_.hovered_.end();) {
            if (scene().generation_of(it->first) != it->second) {
                it = pointer_.hovered_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // --- 按下：拖拽 → 点击 -------------------------------------------
    //
    // 拖拽只认 owner（指针下最前面的可交互层）。不能改成"最前面的 drag
    // 层"：旧框架的列表页把 jump 按钮画在全屏拖拽层 500.0.drag 之上，
    // 那样选会永远先起拖拽、按钮一次都点不到（backlog 跳转没反应）。
    if (left_down_edge) {
        // 背景「推しエフェクト」这类**全屏拖拽范围层**（push_ef 的 `-1000`：
        // 画面最背面、透明、只有 drag 行）会挡在剧情点击停驻前面。它只负责
        // 手势演出，不是按钮：没有可点的行、当前又是剧情点击停驻时，这一下
        // 必须仍然归脚本（翻页），不能因为起了拖拽就把按下票吞掉。停在上层
        // 的按钮/槽位拖动不受影响（probe.click 非空）。
        //
        // 判据是**它是不是压在画面最底下**（hits 是前→后，back 最深），不是
        // "没有 click 行"。消息窗顶上的触摸条（`1.80.mw.zmask`）同样只有
        // drag 行、同样停在剧情点击停驻上，但它在一堆层的前面 —— 早先用
        // "没有 click 行"当判据，把触摸条也当成氛围层，拖拽整条失效。
        const bool owner_is_backmost =
            !probe.hits.empty() && probe.hits.back() == probe.owner;
        const bool ambient_drag = probe.click.empty() && owner_is_backmost && park_.wait &&
                                  park_.wait->waiting_click();
        if (std::getenv("OA_DRAGDBG")) {
            std::fprintf(stderr,
                         "[dragdbg] at (%.0f,%.0f) owner='%s' backmost=%d ambient=%d "
                         "draggable=%d dragrow=%d hits=%zu\n",
                         mx, my, probe.owner.c_str(), int(owner_is_backmost), int(ambient_drag),
                         probe.owner.empty() ? 0 : int(scene().is_layer_draggable(probe.owner)),
                         probe.owner.empty() ? 0 : int(key_has_drag_handler(scene(), probe.owner)),
                         probe.hits.size());
            std::fprintf(stderr, "[dragdbg]   hits:");
            for (size_t i = 0; i < probe.hits.size() && i < 8; ++i)
                std::fprintf(stderr, " %s", probe.hits[i].c_str());
            std::fprintf(stderr, "\n");
        }
        if (!ambient_drag && !probe.owner.empty() && scene().is_layer_draggable(probe.owner) &&
            key_has_drag_handler(scene(), probe.owner)) {
            if (drag_begin(probe.owner, false)) {
                d.owner = PressDecision::Owner::Layer;
                d.layer = probe.owner;
                // 按下这一拍就要派发一次 `drag`（位移 0）。原版在"开始拖动"
                // 的当拍就进 drag 回调：读档界面的槽位缩略图靠这一拍把
                // `adv.fakeclick` 立起来，松手时 vsync 的 `isUpTap()` 才能用
                // `e:overrideKey{key=146}` 合成"确定"，走到 `save_click` →
                // `save_check`。只在移动时派发的话，原地点击永远不触发
                // 槽位的 drag 回调，点哪个槽都没反应（test13 读档）。
                if (const auto* row = scene().find_event_handler(probe.owner, "drag"))
                    dispatch_layer_row(probe.owner, *row, "drag",
                                       {{"drag", "1"}, {"id", probe.owner}},
                                       PointerDispatch::Kind::DragMove);
            }
        }
        if (d.owner != PressDecision::Owner::Layer) {
            if (std::getenv("OA_INPUTDBG")) {
                std::fprintf(stderr, "[hit] click chain:");
                for (const std::string& cid : probe.click) {
                    const auto* r = scene().find_event_handler(cid, "click");
                    std::fprintf(stderr, " %s(row=%d,label=%s)", cid.c_str(), r ? 1 : 0,
                                 r ? r->label.c_str() : "");
                }
                std::fprintf(stderr, "\n");
            }
            // probe.click 是**从下往上**（probe_pointer 里 reverse 过，hover
            // 链要的就是这个顺序）。点击必须从最上层往下：盖在页面上的对话
            // 框按钮才是玩家点的那个 —— 先派发下层的话，下层按钮会把
            // `t.button_data` 这类共享槽写成自己的值，上层的 [call pushed]
            // 于是跳到下层按钮的标签（点"确定"却重开对话框）。
            for (auto cit = probe.click.rbegin(); cit != probe.click.rend(); ++cit) {
                const std::string& id = *cit;
                const auto* row = scene().find_event_handler(id, "click");
                if (!row) continue;
                // NotHandled 才继续往下传。Masked（脚本掩码）同样算认领：
                // 掩码是"别执行这个按钮"，不是"把这一下让给剧情翻页"。
                if (dispatch_layer_row(id, *row, "click", {{"click", "1"}},
                                       PointerDispatch::Kind::Click) !=
                    HandlerOutcome::NotHandled) {
                    d.owner = PressDecision::Owner::Layer;
                    d.layer = id;
                    // 带跳转的按钮 = 这一下点击的终点。
                    //
                    // 框架里每个按钮的 click 行都写 penetration=1，命中扫描
                    // 会把**压在它下面的**按钮也算进来（对话确认框盖在语言
                    // 按钮上时，点"确定"会连底下那个语言按钮一起派发）。
                    // 这些行用的是同一个全局槽（`handler="var" name="t.button_data"`），
                    // 两条 [call pushed] 排进队列后都读到**最后一个**值 ——
                    // 点确定反而跳到上一个按钮的标签（对话框反复重开、语言
                    // 永远选不上）。原版是逐个立即执行，天然不会串。这里让
                    // "会改变流程的按钮"独占这一次按下，纯视觉/悬停层不受影响。
                    if (row->call || !row->label.empty() || !row->file.empty()) break;
                }
            }
        }
    }
    if (in.left_down && pointer_moved && !pointer_.drag_layer_.empty()) {
        // 目标层被重建/删除 ⇒ 这条拖拽到此为止（不再把 drag 送进旧回调）。
        if (scene().generation_of(pointer_.drag_layer_) != pointer_.drag_generation_) {
            pointer_.drag_layer_.clear();
            d.owner = PressDecision::Owner::Layer;
        } else {
            drag_update(mx, my);
            d.owner = PressDecision::Owner::Layer;
        }
    }
    if (left_up_edge && !pointer_.drag_layer_.empty()) {
        if (scene().generation_of(pointer_.drag_layer_) == pointer_.drag_generation_)
            drag_end();
        else
            pointer_.drag_layer_.clear();
        d.owner = PressDecision::Owner::Layer;
    }

    // 键位侧只报告认领，不抢按下的所有权。
    const KeyEdgeClaims keys = dispatch_key_edges();
    if (d.owner == PressDecision::Owner::None) {
        if (keys.role_page) {
            d.owner = PressDecision::Owner::Script;
        } else if (d.press) {
            // 全局 push 行接了左键，且当前不是"点击停驻"：push 是键位转发，
            // 不是 UI 命中，不该顺带翻页（评论里那个开屏 logo 的形状）。
            const bool click_wait = park_.wait && park_.wait->has(oa::runtime::Wait::WakeClick);
            const bool push_absorbs = keys.left_push && !click_wait;
            // 游戏自己命名了 role-0（advance_keys_set）：脚本自有的点击
            // 停驻由游戏释放（菜单页盖在剧情上时，按到按钮不该翻页）。
            const bool game_owns = advance_keys_set() && park_.wait &&
                                   park_.wait->waiting_click() &&
                                   park_.wait->has(oa::runtime::Wait::WakeDecide) &&
                                   park_.wait->script_owned;
            if (!push_absorbs && !game_owns) d.owner = PressDecision::Owner::Script;
        }
    }
    if (std::getenv("OA_INPUTDBG") && (d.press || d.owner != PressDecision::Owner::None)) {
        const char* owner = d.owner == PressDecision::Owner::Engine   ? "Engine"
                            : d.owner == PressDecision::Owner::Layer  ? "Layer"
                            : d.owner == PressDecision::Owner::Script ? "Script"
                                                                      : "None";
        const auto mask = interpreter_->variables().get("t.eventfilter");
        std::fprintf(stderr, "[press] owner=%s layer=%s press=%d mask=%s left=%d\n", owner,
                     d.layer.c_str(), int(d.press),
                     mask ? mask->to_display().c_str() : "<unset>", int(left_down_edge));
    }
    return d;
}


GameRuntime::RuntimeState::KeyEdgeClaims GameRuntime::RuntimeState::dispatch_key_edges() {
    KeyEdgeClaims claims;
    std::vector<int> key_edges(input_state_.down_edges_.begin(), input_state_.down_edges_.end());
    std::sort(key_edges.begin(), key_edges.end());
    for (const int key : key_edges) {
        bool cancel_park = false;
        // right button / ESC first walk the engine rclick chain (hide
        // recovery or the configured rclick script); only when unconsumed does
        // the same key reach the push row.
        if (key == kKeyMouseRight || key == 27) {
            if (trigger_rclick()) continue;
            // Cancel key satisfies a queue-armed [exkey] click park.
            cancel_park = park_.wait && park_.wait->waiting_click() && interpreter_ &&
                          interpreter_->last_wait_from_queue();
        }
        // keyconfig role edges (advance/hide/backlog/automode/skip).
        if (handle_role_key_edge(key)) claims.role_page = true;
        const std::string key_string = std::to_string(key);
        const auto* row = scene().get_input_handler("push", key_string);
        if (!row) {
            if (cancel_park) claims.role_page = true;
            continue;
        }
        const std::string event_type = is_mouse_button_key(key) ? "click" : "key";
        const bool handled = dispatch_input_row(
            "setonpush", *row,
            {{"key", key_string}, {"type", event_type}}, PointerDispatch::Kind::Push);
        if (key == kKeyMouseLeft && handled) claims.left_push = true;
        // Cancel press also runs the push row so flg.keycode is recorded.
        if (cancel_park) claims.role_page = true;
    }
    return claims;
}

void GameRuntime::RuntimeState::notify_pointer(const PointerDispatch& d) {
    ++pointer_.pointer_dispatch_count_;
    renderer_.request_repaint();
    if (pointer_.pointer_observer_) pointer_.pointer_observer_(d);
}

GameRuntime::RuntimeState::HandlerOutcome GameRuntime::RuntimeState::dispatch_layer_row(
    const std::string& id, oa::render::LayerEventHandler row,
    const std::string& event_type,
    const std::vector<std::pair<std::string, std::string>>& runtime_params,
    PointerDispatch::Kind kind) {
    (void)event_type;
    if (!row.enabled) return HandlerOutcome::NotHandled;
    // e:setEventFilter consult: 1 = script claims it, 2 = pretend failure.
    const auto verdict = interpreter_->lua_bridge().run_event_filter("lyevent", row.filter_params);
    if (verdict == 2) return HandlerOutcome::NotHandled;
    if (verdict == 1) {
        // 脚本的 lyevent 过滤器只是想屏蔽**这一层**的动作（框架的输入掩码）。
        // 它不吞玩家这一下点击本身：调用方会把这次按下继续传给停驻解析，
        // 原版就是"先跑脚本、再派发层"。返回 Masked 而不是 Handled，就是
        // 为了让"这一下算按过了"这件事对上层可见。
        return HandlerOutcome::Masked;
    }
    // calllua-equivalent: fn = params["function"]; missing = silent no-op
    // (the [calllua] branch lives in runtime_iet.cpp).
    std::map<std::string, std::string> params = row.params;
    for (const auto& [k, v] : runtime_params) params[k] = v;
    const auto fn_it = params.find("function");
    bool handled = true;
    bool fn_queued = false; // this row's fn enqueued >=1 engine tag
    // 事件处理会话：处理函数运行期间栈顶就是这层合成帧（对外 file==""），
    // 框架的 ReturnStack() 才看得到它并立即收掉。ReturnStack 收过之后
    // in_handler_frame() 已是 false，下面的收尾自然空转。
    const bool session = fn_it != params.end() && !fn_it->second.empty() &&
                         interpreter_->begin_handler_frame();
    // handler="exec": the row carries an exec command. The framework's
    // [forcedexec] macro parks `command`/`mode` on a zero-duration tween
    // because an exec issued from a handler event cannot cancel automode
    // (Artemis runs it inside the handler's event level); the tween fires the
    // row at frame level instead. Same command path as [exec].
    if (row.handler == "exec") {
        apply_exec_command(params);
        PointerDispatch d;
        d.kind = kind;
        d.layer = id;
        d.function = "exec";
        notify_pointer(d);
    }
    // handler="var": apply the row's [var name data] assignment (iMel buttons).
    if (row.handler == "var") {
        const auto name_it = params.find("name");
        if (name_it != params.end() && !name_it->second.empty()) {
            std::map<std::string, std::string> var_params;
            var_params["name"] = name_it->second;
            const auto data_it = params.find("data");
            var_params["data"] = data_it == params.end() ? std::string() : data_it->second;
            interpreter_->apply_var(var_params);
            PointerDispatch d;
            d.kind = kind;
            d.layer = id;
            d.function = "var:" + name_it->second;
            notify_pointer(d);
        }
    }
    if (fn_it != params.end() && !fn_it->second.empty()) {
        PointerDispatch d;
        d.kind = kind;
        d.layer = id;
        d.function = fn_it->second;
        const auto key_it = params.find("key");
        d.key = key_it == params.end() ? std::string() : key_it->second;
        const size_t q0 = interpreter_->queued_tag_count();
        try {
            d.ran_lua = interpreter_->lua_bridge().call_function(fn_it->second, params);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[runtime] handler %s error: %s\n", fn_it->second.c_str(),
                         ex.what());
            d.ran_lua = false;
        }
        fn_queued = interpreter_->queued_tag_count() > q0;
        notify_pointer(d);
    }
    if (session) interpreter_->end_handler_frame();
    // Queue navigation after the handler; call pushes a return frame.
    if (!row.file.empty() || !row.label.empty()) {
        std::map<std::string, std::string> nav;
        if (!row.file.empty()) nav["file"] = row.file;
        if (!row.label.empty()) nav["label"] = row.label;
        interpreter_->enqueue_tag(row.call ? "call" : "jump", std::move(nav));
        handled = true;
    }
    // 处理帧的生命周期：处理函数没排队任何 tag = 这一帧已经结束，立即回收；
    // 排了 tag 就留着，等队列排空时由 settle 收尾（[return] 也可能自己弹掉它）。
    return handled ? HandlerOutcome::Handled : HandlerOutcome::NotHandled;
}

bool GameRuntime::RuntimeState::dispatch_input_row(
    const std::string& filter_name, oa::render::InputHandler row,
    const std::vector<std::pair<std::string, std::string>>& runtime_params,
    PointerDispatch::Kind kind) {
    const auto verdict = interpreter_->lua_bridge().run_event_filter(filter_name, row.filter_params);
    if (verdict == 2) return false;
    if (verdict == 1) return true;
    std::map<std::string, std::string> params = row.params;
    for (const auto& [k, v] : runtime_params) params[k] = v;
    const auto fn_it = params.find("function");
    bool handled = true;
    bool fn_queued = false; // this row's fn enqueued >=1 engine tag
    const bool session = fn_it != params.end() && !fn_it->second.empty() &&
                         interpreter_->begin_handler_frame();
    if (fn_it != params.end() && !fn_it->second.empty()) {
        PointerDispatch d;
        d.kind = kind;
        d.layer.clear();
        d.function = fn_it->second;
        const auto key_it = params.find("key");
        d.key = key_it == params.end() ? std::string() : key_it->second;
        const size_t q0 = interpreter_->queued_tag_count();
        try {
            d.ran_lua = interpreter_->lua_bridge().call_function(fn_it->second, params);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[runtime] handler %s error: %s\n", fn_it->second.c_str(),
                         ex.what());
            d.ran_lua = false;
        }
        fn_queued = interpreter_->queued_tag_count() > q0;
        notify_pointer(d);
    }
    if (session) interpreter_->end_handler_frame();
    // file/label rows: enqueue jump/call — same path as layer rows.
    if (!row.file.empty() || !row.label.empty()) {
        std::map<std::string, std::string> nav;
        if (!row.file.empty()) nav["file"] = row.file;
        if (!row.label.empty()) nav["label"] = row.label;
        interpreter_->enqueue_tag(row.call ? "call" : "jump", std::move(nav));
        handled = true;
    }
    return handled;
}

// ---------------------------------------------------------------------------
// 停驻上下文（帧即执行上下文）
//
// 停驻不是宿主的一个单值成员，而是**当前这一层**的状态：`[call]` 压帧时
// 外层连同它的尾巴、排队中的停驻、命名 wake 一起入栈；`[return]` 弹帧时
// 原样交还。宿主因此不需要"把停驻寄存到某个帧 id"这种旁路记账。
// ---------------------------------------------------------------------------

bool GameRuntime::RuntimeState::park_wants_return_frame() const {
    // 只有"玩家正在读的那一页"的点击停驻值得回程：处理函数把执行跳到别的
    // 脚本时，那段流程自己的 [return] 必须落回这一页（含它的停驻和排在后面的
    // 脚本尾巴），而不是把调用链弹掉（test13: 快速存档/读档曾在返回路上翻页）。
    //
    // [stop] 或计时停驻不属于这一页（UI 界面/菜单早已结束），那条路由流程自己
    // 拥有——它们的 ReturnStack()/[return] 编舞不能再多一层帧（backlog 的关闭
    // 路径正是这样）。
    return park_.wait && park_.wait->waiting_click();
}

void GameRuntime::RuntimeState::push_park_context() {
    park_stack_.push_back(std::move(park_));
    park_ = ParkContext{};
}

bool GameRuntime::RuntimeState::pop_park_context() {
    ParkContext outer = park_stack_.empty() ? ParkContext{} : std::move(park_stack_.back());
    if (!park_stack_.empty()) park_stack_.pop_back();
    // 被弹掉那一层（callee）的停驻随帧一起死：它的 owner 行属于一个已经不存在
    // 的位置，留着只会让宿主在错误的行上等下一次输入。
    if (!outer.wait) {
        park_ = std::move(outer);
        return false;
    }
    const auto at_park_line = [this](const oa::runtime::Wait& w) {
        const std::string* script = interpreter_ ? interpreter_->current_script() : nullptr;
        return script && w.owner_script == *script && w.owner_line == interpreter_->current_line();
    };
    // 停驻属于武装它的那一行。处理函数发的 [jump] 可以让脚本在持帧期间就换
    // 位置（框架会把 [call <file> last] 预载行排在 jump 旁边）；把停驻恢复到
    // 新行上会让剧情停在一个它已经离开的位置，之后每次按键都重跑那一行
    // （test13 backlog 行跳转：确认框只响应一次）。
    if (!at_park_line(*outer.wait)) {
        // 过期：把它的尾巴交回队列（清理 tag 不能丢），执行从流程实际所在的
        // 位置继续。
        if (!outer.tail.empty()) interpreter_->prepend_tags(std::move(outer.tail));
        outer.tail.clear();
        outer.wait.reset();
        park_ = std::move(outer);
        return false;
    }
    park_ = std::move(outer);
    // 重新武装：关闭 UI 的那一次按键是 UI 的命令，不是剧情下一页的翻页键。
    park_.wait->armed_tick = tick_count_;
    park_.wait->seq = ++wait_seq_;
    park_.wait->auto_elapsed_ms = 0;
    park_.wait->restored = true;
    return true;
}

void GameRuntime::RuntimeState::reset_park_contexts() {
    park_ = ParkContext{};
    park_stack_.clear();
}






void GameRuntime::RuntimeState::force_pointer_drag(const std::string& layer_id) {
    if (layer_id.empty() || !interpreter_) return;
    drag_begin(layer_id, true);
}

bool GameRuntime::RuntimeState::drag_begin(const std::string& layer_id, bool forced) {
    (void)forced; // forced = [lydrag]: already checked by the caller
    const auto offset = scene().layer_offset(layer_id);
    if (!offset) return false;
    pointer_.drag_layer_ = layer_id;
    // 记下目标实例：这一层中途被 [lydel]+[lyc] 换掉时，序列立即作废
    // （后面那些 drag/dragout 回调属于已经不存在的那个层）。
    pointer_.drag_generation_ = scene().generation_of(layer_id);
    pointer_.drag_start_mouse_x_ = double(input_state_.mouse_x_);
    pointer_.drag_start_mouse_y_ = double(input_state_.mouse_y_);
    pointer_.drag_origin_left_ = offset->first;
    pointer_.drag_origin_top_ = offset->second;
    const auto* row = scene().find_event_handler(layer_id, "dragin");
    if (row)
        dispatch_layer_row(layer_id, *row, "dragin",
                           {{"drag", "1"}, {"id", layer_id}}, PointerDispatch::Kind::DragIn);
    return true;
}

void GameRuntime::RuntimeState::drag_update(double mx, double my) {
    if (pointer_.drag_layer_.empty()) return;
    const double dx = mx - pointer_.drag_start_mouse_x_;
    const double dy = my - pointer_.drag_start_mouse_y_;
    double left = 0, top = 0;
    if (!scene().drag_layer_to(pointer_.drag_layer_, pointer_.drag_origin_left_, pointer_.drag_origin_top_, dx, dy, &left,
                              &top)) {
        pointer_.drag_layer_.clear();
        return;
    }
    const auto* row = scene().find_event_handler(pointer_.drag_layer_, "drag");
    if (row)
        dispatch_layer_row(pointer_.drag_layer_, *row, "drag",
                           {{"drag", "1"}, {"id", pointer_.drag_layer_}},
                           PointerDispatch::Kind::DragMove);
}

void GameRuntime::RuntimeState::drag_end() {
    if (pointer_.drag_layer_.empty()) return;
    const auto* row = scene().find_event_handler(pointer_.drag_layer_, "dragout");
    if (row)
        dispatch_layer_row(pointer_.drag_layer_, *row, "dragout",
                           {{"drag", "0"}, {"id", pointer_.drag_layer_}},
                           PointerDispatch::Kind::DragOut);
    pointer_.drag_layer_.clear();
}

// Control domain: keyconfig roles, skip/automode, control-skip, rclick, hide.
// Details: docs/RUNTIME_NOTES.md.
namespace {

// keyconfig role numbers (docs/tag/system/keyconfig.md / ROLE_*).
constexpr int kRoleAdvance = 0;
constexpr int kRoleHideIn = 3;
constexpr int kRoleHideOut = 4;
constexpr int kRoleBacklogIn = 5;
constexpr int kRoleBacklogOut = 6;
constexpr int kRoleAutomodeIn = 9;
constexpr int kRoleAutomodeOut = 10;
constexpr int kRoleSkipIn = 12;
constexpr int kRoleSkipOut = 13;
constexpr int kRoleControlSkip = 14;
/// 緊急回避（スクランブル）：role 15 = 開始、role 16 = 終了。
/// 原版は 15/16 を鍵の役割として登録し、[avoid file=...] の画像を最前面に
/// 出して音を止める。test13 の set_scramble() がこの2つを [keyconfig] で書く。
constexpr int kRoleAvoidIn = 15;
constexpr int kRoleAvoidOut = 16;

} // namespace

// ---------------------------------------------------------------------------
// [keyconfig] role application
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_keyconfig(const std::map<std::string, std::string>& params) {
    const auto role_it = params.find("role");
    if (role_it == params.end()) return;
    const int role = parse_i32(role_it->second, -1);
    if (role < 0) return;
    std::set<int> keys;
    const auto k_it = params.find("keys");
    if (k_it != params.end()) {
        std::string raw = k_it->second;
        // keys are a comma / whitespace separated list of virtual key codes;
        // "" clears the role.
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t comma = raw.find_first_of(", \t\r\n", pos);
            const std::string tok = raw.substr(pos, comma == std::string::npos
                                                     ? std::string::npos
                                                     : comma - pos);
            if (!tok.empty()) keys.insert(parse_i32(tok, -1));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        keys.erase(-1);
    }
    control_.keymap_[role] = std::move(keys);
}

bool GameRuntime::RuntimeState::has_role(int role, int key) const {
    const auto it = control_.keymap_.find(role);
    return it != control_.keymap_.end() && it->second.count(key) > 0;
}

bool GameRuntime::RuntimeState::advance_keys_set() const {
    return control_.keymap_.count(kRoleAdvance) > 0;
}

bool GameRuntime::RuntimeState::key_is_toggle(int key, int other_role) const {
    return control_.keymap_.count(other_role) > 0 && control_.keymap_.at(other_role).count(key) > 0;
}

/// Role edge; true when the key acted as role-0 advance.
bool GameRuntime::RuntimeState::handle_role_key_edge(int key) {
    // 緊急回避中は「終了キー」だけを受け付ける。物語のクリック待ちは
    // 進めない（原版の vsync_game も avoid 中は本体ループに入らない）。
    if (control_.avoid_active_) {
        if (has_role(kRoleAvoidOut, key)) end_avoid();
        return false;
    }
    if (has_role(kRoleAvoidIn, key) && !control_.avoid_file_.empty()) {
        start_avoid();
        return false;
    }
    if (has_role(kRoleAdvance, key)) return true;
    if (has_role(kRoleHideIn, key)) {
        toggle_hide_mode();
    } else if (has_role(kRoleHideOut, key)) {
        if (!key_is_toggle(key, kRoleHideIn)) exit_hide_mode();
    } else if (has_role(kRoleBacklogIn, key)) {
        dispatch_mode_input("backlogin");
    } else if (has_role(kRoleBacklogOut, key)) {
        dispatch_mode_input("backlogout");
    } else if (has_role(kRoleAutomodeIn, key)) {
        // allow=0 は「プレイヤーのキーで入れない」。スクリプトの
        // [exec command="automode"] は set_automode_mode を直接呼ぶので
        // ここを通らない（フレームワークは allow=0 を書いてから exec する）。
        if (!control_.automode_allowed_) return false;
        if (key_is_toggle(key, kRoleAutomodeOut)) {
            set_automode_mode(!automode_active());
        } else {
            set_automode_mode(true);
        }
    } else if (has_role(kRoleAutomodeOut, key)) {
        if (!key_is_toggle(key, kRoleAutomodeIn)) {
            set_automode_mode(false);
        }
    } else if (has_role(kRoleSkipIn, key)) {
        if (key_is_toggle(key, kRoleSkipOut)) {
            set_skip_mode(!skip_active());
        } else {
            set_skip_mode(true);
        }
    } else if (has_role(kRoleSkipOut, key)) {
        if (!key_is_toggle(key, kRoleSkipIn)) {
            set_skip_mode(false);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// [rclick allow/file] + engine right-click chain
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_rclick_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) control_.rclick_allowed_ = parse_i32(allow_it->second, 1) != 0;
    const auto file_it = params.find("file");
    if (file_it != params.end() && !file_it->second.empty()) control_.rclick_file_ = file_it->second;
}
// [mouse left top] — request an OS cursor warp (host hook).
void GameRuntime::RuntimeState::apply_mouse_config(const std::map<std::string, std::string>& params) {
    // A live drag owns the pointer: never warp mid-drag (defensive; the FPM
    // dialog path disables drags first via sliderdrag_stat(0)).
    if (!pointer_.drag_layer_.empty()) return;
    const auto left_it = params.find("left");
    const auto top_it = params.find("top");
    if (left_it == params.end() && top_it == params.end()) return;
    int x = input_state_.mouse_x_;
    int y = input_state_.mouse_y_;
    if (left_it != params.end()) x = parse_i32(left_it->second, x);
    if (top_it != params.end()) y = parse_i32(top_it->second, y);
    // Clamp into the stage (defensive: official callers only send button
    // centers / +/-1px refresh nudges).
    const int sw = rt_->project_.config.stage_width;
    const int sh = rt_->project_.config.stage_height;
    if (sw > 0) x = std::clamp(x, 0, sw - 1);
    if (sh > 0) y = std::clamp(y, 0, sh - 1);
    pointer_.pointer_warp_x_ = x;
    pointer_.pointer_warp_y_ = y;
    pointer_.pointer_warp_pending_ = true;
    ++pointer_.pointer_warp_requests_;
    if (pointer_.pointer_warp_cb_) pointer_.pointer_warp_cb_(x, y);
}

bool GameRuntime::RuntimeState::trigger_rclick() {
    if (!interpreter_) return false;
    // hide mode: a right click first recovers the message window (no menu).
    if (control_.hide_active_) {
        exit_hide_mode();
        return true;
    }
    if (!control_.rclick_allowed_) return false;
    const std::string file =
        control_.rclick_file_.empty() ? std::string("rclick.iet") : control_.rclick_file_;
    // Already inside the rclick script: jump to its "leave" label (its return
    // comes back to the pre-rclick position —  approximation).
    const std::string* cur = interpreter_->current_script();
    if (cur && *cur == file) {
        std::map<std::string, std::string> nav{{"file", file}, {"label", "leave"}};
        interpreter_->enqueue_tag("jump", std::move(nav));
    } else {
        // Enter the rclick script as a subroutine (call frame; a [return] in
        // the script resumes the story at the original position).
        std::map<std::string, std::string> nav{{"file", file}};
        interpreter_->enqueue_tag("call", std::move(nav));
    }
    return true;
}

// ---------------------------------------------------------------------------
// [hide allow/window] + hide mode
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_hide_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) {
        const bool allow = parse_i32(allow_it->second, 1) != 0;
        if (!allow && control_.hide_active_) exit_hide_mode();
        control_.hide_allowed_ = allow;
    }
    const auto win_it = params.find("window");
    if (win_it != params.end()) {
        control_.hide_window_.clear();
        std::string raw = win_it->second;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t comma = raw.find(',', pos);
            std::string tok = raw.substr(pos, comma == std::string::npos
                                                 ? std::string::npos
                                                 : comma - pos);
            // trim spaces
            const auto b = tok.find_first_not_of(" \t\r\n");
            const auto en = tok.find_last_not_of(" \t\r\n");
            tok = (b == std::string::npos) ? "" : tok.substr(b, en - b + 1);
            if (!tok.empty()) control_.hide_window_.push_back(tok);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
}

void GameRuntime::RuntimeState::apply_hide_visibility(bool hidden) {
    for (const std::string& wid : control_.hide_window_) {
        const oa::render::Layer* l = scene().find(wid);
        if (!l) continue;
        const bool visible_now = l->visible != 0.0;
        if (hidden) {
            if (!control_.hide_snapshot_.count(wid)) control_.hide_snapshot_[wid] = visible_now;
            if (visible_now) scene().set_props(wid, {{"visible", "0"}});
        } else {
            const bool was_visible = control_.hide_snapshot_.count(wid) ? control_.hide_snapshot_[wid] : true;
            control_.hide_snapshot_.erase(wid);
            if (was_visible && !visible_now) scene().set_props(wid, {{"visible", "1"}});
        }
    }
}

void GameRuntime::RuntimeState::toggle_hide_mode() {
    if (control_.hide_active_) {
        exit_hide_mode();
    } else {
        if (!control_.hide_allowed_) return;
        control_.hide_active_ = true;
        apply_hide_visibility(true);
        dispatch_mode_input("hidein");
    }
}

void GameRuntime::RuntimeState::exit_hide_mode() {
    if (!control_.hide_active_) return;
    control_.hide_active_ = false;
    apply_hide_visibility(false);
    dispatch_mode_input("hideout");
}

// ---------------------------------------------------------------------------
// skip / automode state machines
// (set_skip_mode/set_automode_mode, apply_skip_config/apply_automode_config)
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::reveal_all_text() {
    if (!text().is_reveal_complete()) text().reveal_all();
}

void GameRuntime::RuntimeState::dispatch_mode_input(const std::string& event_name) {
    // Mode handlers register under (event, "") via [setonautomodein] etc.
    // ( input_handlers; enqueue_mode_input_handler ).
    const auto* row = scene().get_input_handler(event_name, "");
    if (!row) return;
    dispatch_input_row("seton" + event_name, *row, {{"type", event_name}},
                       PointerDispatch::Kind::Push);
}

void GameRuntime::RuntimeState::set_skip_mode(bool enabled) {
    if (enabled && !control_.skip_allowed_) return;
    if (enabled && automode_active()) set_automode_mode(false);
    const bool was_active = skip_active();
    control_.skip_enabled_ = enabled;
    const bool now_active = skip_active();
    if (was_active == now_active) return;
    if (!now_active) {
        reveal_all_text(); // reveal on skip exit so new text is not stuck
    }
    // s.status.* 必须在事件分发**之前**落地：框架的 commandskip_in_se
    // 用 `$s.status.commandskip` 决定画不画左上角的 SKIP 标识，读到的是
    // 上一帧的旧值就会走"开始时又结束"的分支。
    sync_control_status();
    dispatch_mode_input(now_active ? "commandskipin" : "commandskipout");
}

void GameRuntime::RuntimeState::set_automode_mode(bool enabled) {
    // allow は「キーで入れるか」だけを決める（handle_role_key_edge が見る）。
    // スクリプトの [exec command="automode"] はここへ直接来る —— test13 の
    // automode_start は [automode allow="0"] を書いてから exec するので、
    // ここで allow を見るとオートが永久に入らなくなる（ボタン無反応）。
    if (enabled && skip_active()) set_skip_mode(false);
    const bool was_active = automode_active();
    control_.automode_ = enabled;
    if (was_active == automode_active()) return;
    // leaving automode drops the voice-sync gate — the Lua side
    // (autoskip.lua) also clears it with [automode syncse=""], this makes the
    // native exec path symmetric so a later native re-entry starts ungated.
    if (!automode_active()) control_.automode_syncse_.clear();
    // 同上：automode_in_se / automode_out_se 读 `$s.status.automode`
    // 决定 AUTO 图标的显隐，镜像必须先于事件落地。
    sync_control_status();
    dispatch_mode_input(automode_active() ? "automodein" : "automodeout");
}

void GameRuntime::RuntimeState::update_control_skip_hold(const std::set<int>& keys_down) {
    // role 14 keys (Ctrl default; FPM csv.advkey.ctrl) act while held.
    const bool held = std::any_of(keys_down.begin(), keys_down.end(),
                                  [this](int k) { return has_role(kRoleControlSkip, k); });
    const bool was_effective = control_skip_effective();
    if (!held) control_.control_skip_blocked_ = false;
    control_.control_skip_pressed_ = held;
    const bool now_effective = control_skip_effective();
    if (was_effective == now_effective) return;
    if (!now_effective) reveal_all_text();
    sync_control_status();
    dispatch_mode_input(now_effective ? "controlskipin" : "controlskipout");
}

// ---------------------------------------------------------------------------
// config tags ([skip]/[automode]/[keyconfig]/[rclick]/[hide]/[exec]/[backlog])
// ---------------------------------------------------------------------------
void GameRuntime::RuntimeState::apply_skip_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) {
        const bool allow = parse_i32(allow_it->second, 1) != 0;
        if (!allow) {
            if (skip_active()) set_skip_mode(false);
            // a held role-14 key is blocked until it is released
            control_.control_skip_blocked_ = control_.control_skip_pressed_;
        }
        control_.skip_allowed_ = allow;
    }
    // unread stop-to-skip ([skip unread]) is not ported.
}

void GameRuntime::RuntimeState::apply_automode_config(const std::map<std::string, std::string>& params) {
    const auto allow_it = params.find("allow");
    if (allow_it != params.end()) {
        const bool allow = parse_i32(allow_it->second, 1) != 0;
        if (!allow && automode_active()) set_automode_mode(false);
        control_.automode_allowed_ = allow;
    }
    // stopbyclick: プレイヤーのクリックでオートを止める（既定 on）。
    const auto stop_it = params.find("stopbyclick");
    if (stop_it != params.end())
        control_.automode_stopbyclick_ = parse_i32(stop_it->second, 1) != 0;
    // syncse: automode flip waits for these voice ids to stop.
    const auto sync_it = params.find("syncse");
    if (sync_it != params.end()) control_.automode_syncse_ = sync_it->second;
}

void GameRuntime::RuntimeState::apply_exec_command(const std::map<std::string, std::string>& params) {
    const auto cmd_it = params.find("command");
    if (cmd_it == params.end()) return;
    const std::string& command = cmd_it->second;
    const auto mode_it = params.find("mode");
    const bool has_mode = mode_it != params.end();
    const int mode = has_mode ? parse_i32(mode_it->second, 1) : 0;
    if (command == "skip") {
        set_skip_mode(has_mode ? mode != 0 : !control_.skip_enabled_);
    } else if (command == "automode") {
        set_automode_mode(has_mode ? mode != 0 : !automode_active());
    } else if (command == "rclick") {
        if (!trigger_rclick()) {
            // fall back to the push key=2 chain ( enqueue_exec_input)
            const auto* row = scene().get_input_handler("push", "2");
            if (row) dispatch_input_row("setonpush", *row,
                                        {{"key", "2"}, {"type", "click"}},
                                        PointerDispatch::Kind::Push);
        }
    } else if (command == "hide") {
        toggle_hide_mode();
    } else if (command == "backlog") {
        dispatch_mode_input("backlogin");
    } else if (command == "backlogclose") {
        dispatch_mode_input("backlogout");
    } else {
        // exec commands without an engine mapping are a diagnostic no-op
        std::fprintf(stderr, "[runtime] exec command not mapped: %s\n", command.c_str());
    }
}

void GameRuntime::RuntimeState::sync_control_status() {
    if (!interpreter_) return;
    // 框架（artemis.lua / autoskip.lua / vsync.lua）不直接问引擎，只读
    // `s.status.*`：get_skip() 看 commandskip/controlskip，紧急回避看 avoid。
    // 这些是瞬态值，不参与存盘（syssave 跳过 s.status. 前缀）。
    // 框架状态镜像：automode 与 skip 一样是 s.status.* 家族的一员，
    // 脚本用 `$s.status.automode` 选择 AUTO 图标与按钮分支。
    interpreter_->set_variable("s.status.automode",
                               oa::runtime::Value::make_int(automode_active() ? 1 : 0));
    interpreter_->set_variable("s.status.commandskip",
                               oa::runtime::Value::make_int(skip_active() ? 1 : 0));
    interpreter_->set_variable("s.status.controlskip",
                               oa::runtime::Value::make_int(control_skip_effective() ? 1 : 0));
    interpreter_->set_variable("s.status.avoid",
                               oa::runtime::Value::make_int(control_.avoid_active_ ? 1 : 0));
}

void GameRuntime::RuntimeState::start_avoid() {
    if (control_.avoid_active_ || control_.avoid_file_.empty()) return;
    control_.avoid_active_ = true;
    // 「音声・画面の動作を停止」——全音声を止め、映像も止める。
    if (automode_active()) set_automode_mode(false);
    if (skip_active()) set_skip_mode(false);
    media_.audio_.stop_all_sounds();
    if (media_.media_players_) media_.media_players_->stop_all();
    media_.video_.stop_all_videos();
    // 最前面（overlay スロット）に全画面で置く。id ソートでは数値 < 文字列
    // なので "9999" ではメッセージ層に負ける —— overlay スロットだけが
    // 構造的に「最後に描く」。
    std::map<std::string, std::string> props;
    props["file"] = control_.avoid_file_;
    props["left"] = "0";
    props["top"] = "0";
    props["width"] = std::to_string(stage_width());
    props["height"] = std::to_string(rt_->project_.config.stage_height);
    scene().set_overlay_file();
    // create（不是 set_props）：file 键只在 Create 路径上绑定到
    // Layer::file（set_props 只把它当 verbatim 属性）。
    scene().create(oa::render::kOverlayNodeId, props);
    renderer_.note(RNote::Layer);
    sync_control_status();
    std::fprintf(stderr, "[runtime] avoid: on (%s)\n", control_.avoid_file_.c_str());
}

void GameRuntime::RuntimeState::end_avoid() {
    if (!control_.avoid_active_) return;
    control_.avoid_active_ = false;
    scene().clear_overlay_file();
    renderer_.note(RNote::Layer);
    sync_control_status();
    std::fprintf(stderr, "[runtime] avoid: off\n");
}

bool GameRuntime::RuntimeState::apply_control_config(const oa::runtime::Event& e) {
    if (e.kind != oa::runtime::Event::Kind::ConfigEvent) return false;
    if (e.tag == "avoid") {
        // [avoid file=... windowbutton=0] —— 緊急回避で出す画像。
        // 実体は role 15 が来たときに最前面へ出す（start_avoid）。
        const auto file = e.params.find("file");
        if (file != e.params.end()) control_.avoid_file_ = file->second;
        const auto wb = e.params.find("windowbutton");
        if (wb != e.params.end()) control_.avoid_windowbutton_ = parse_i32(wb->second, 1) != 0;
        return true;
    }
    if (e.tag == "keyconfig") {
        apply_keyconfig(e.params);
        return true;
    }
    if (e.tag == "rclick") {
        apply_rclick_config(e.params);
        return true;
    }
    if (e.tag == "hide") {
        apply_hide_config(e.params);
        return true;
    }
    if (e.tag == "skip") {
        apply_skip_config(e.params);
        return true;
    }
    if (e.tag == "automode") {
        apply_automode_config(e.params);
        return true;
    }
    if (e.tag == "mouse") {
        apply_mouse_config(e.params);
        return true;
    }
    if (e.tag == "exec") {
        apply_exec_command(e.params);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Host input (SDL conversion already done in main.cpp)
// ---------------------------------------------------------------------------
namespace {

void input_begin_frame(HostInput& in)
{
    in.frame.keys_down = in.kbd_down;
    for (const int mk : in.mouse_held) in.frame.keys_down.insert(mk);
    in.frame.left_down = in.left_down;
    in.frame.left_click_edge = in.left_pressed;
    in.left_pressed = false;
    in.frame.keys_down.insert(in.frame.key_down_edges.begin(),
                              in.frame.key_down_edges.end());

    for (auto it = in.wheel_hold_frames.begin(); it != in.wheel_hold_frames.end();) {
        const int vk = it->first;
        if (it->second > 0) {
            in.frame.keys_down.insert(vk);
            --it->second;
            if (it->second == 0) in.frame.key_up_edges.push_back(vk);
            ++it;
        } else {
            it = in.wheel_hold_frames.erase(it);
        }
    }
}

void input_end_frame(HostInput& in)
{
    in.frame.key_down_edges.clear();
    in.frame.key_up_edges.clear();
}

} // namespace

void GameRuntime::RuntimeState::key_event(int vk, bool down)
{
    if (vk <= 0) return;
    if (down) {
        if (input_.kbd_down.insert(vk).second)
            input_.frame.key_down_edges.push_back(vk);
    } else if (input_.kbd_down.erase(vk) > 0) {
        input_.frame.key_up_edges.push_back(vk);
    }
}

void GameRuntime::RuntimeState::mouse_move(float window_x, float window_y)
{
    float x = window_x, y = window_y;
    if (renderer_.get_renderer_coordinates(window_x, window_y, &x, &y)) {
        input_.frame.mouse_x = int(x);
        input_.frame.mouse_y = int(y);
    } else {
        input_.frame.mouse_x = int(window_x);
        input_.frame.mouse_y = int(window_y);
    }
}

void GameRuntime::RuntimeState::mouse_button(int vk, bool down)
{
    if (vk == 1) {
        input_.left_down = down;
        if (down) input_.left_pressed = true;
    } else if (vk == 2 || vk == 3) {
        if (down) {
            if (input_.mouse_held.insert(vk).second)
                input_.frame.key_down_edges.push_back(vk);
        } else if (input_.mouse_held.erase(vk) > 0) {
            input_.frame.key_up_edges.push_back(vk);
        }
    }
}

void GameRuntime::RuntimeState::mouse_wheel(int direction)
{
    if (direction == 0) return;
    const int vk = direction > 0 ? 136 : 137;
    input_.frame.key_down_edges.push_back(vk);
    input_.wheel_hold_frames[vk] = 2;
}

void GameRuntime::RuntimeState::finish_frame()
{
    // Host path only: apply the events this tick queued, then render. Tests
    // using tick(delta, FrameInput) drain manually and never reach this.
    while (!pending_events_.empty()) {
        const Event e = pending_events_.front();
        pending_events_.pop_front();
        renderer_.process_event(e);
    }
    if (renderer_.renderer_ok())
        renderer_.render_frame(now_ms_, all_delete_fade());
}

void GameRuntime::RuntimeState::tick_input(uint64_t delta_ms)
{
    input_begin_frame(input_);
    tick(delta_ms, input_.frame);
    finish_frame();
    input_end_frame(input_);
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
struct App::Impl {
    using Clock = std::chrono::steady_clock;

    AppConfig cfg;
    std::shared_ptr<GameRuntime> rt;

    bool shutdown_done = false;
    Clock::time_point last_tick{};
    uint64_t frames = 0;
    bool quit = false;
    /// 最近一次 tick 抛出的异常文本（宿主拿来弹窗/落盘，不再是静默退出）。
    std::string last_error;

    bool boot();
    AppResult tick();
    void reset_clock();
    void shutdown();
    void dump_frame();
    void dump_state();
};

namespace {

/// 日志用：这次停驻在等什么（不参与任何判定）。
std::string wait_desc(const oa::runtime::Wait* w)
{
    if (!w) return "none";
    switch (w->stage) {
        case oa::runtime::Wait::Stage::Trans: return "trans";
        case oa::runtime::Wait::Stage::Video: return "video";
        case oa::runtime::Wait::Stage::VideoLayer: return "video('" + w->stage_id + "')";
        case oa::runtime::Wait::Stage::Se: return "se('" + w->stage_id + "')";
        case oa::runtime::Wait::Stage::AllDelete: return "alldelete";
        case oa::runtime::Wait::Stage::Tween: return "tween('" + w->stage_id + "')";
        case oa::runtime::Wait::Stage::None: break;
    }
    if (w->timed()) return "timed(" + std::to_string(w->timer_ms) + ")";
    if (w->on_reveal_done) return "reveal";
    if (w->reveal_clear) return "reveal-clear";
    if (!w->keys.empty()) return "key";
    if (w->waiting_click()) return "click";
    return "park('" + w->stage_id + "')";
}

bool write_ppm(const std::string& path, uint32_t w, uint32_t h,
               const std::vector<uint8_t>& rgba)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            std::fputc(p[0], f);
            std::fputc(p[1], f);
            std::fputc(p[2], f);
        }
    }
    std::fclose(f);
    return true;
}

} // namespace

void App::Impl::dump_frame()
{
    if (cfg.dump.empty() || !rt) return;
    uint32_t w = 0, h = 0;
    auto rgba = rt->capture_frame(&w, &h);
    if (!rgba || w == 0 || h == 0) {
        std::fprintf(stderr, "[app] dump failed: no frame (%s)\n",
                     cfg.dump.c_str());
        return;
    }
    if (write_ppm(cfg.dump, w, h, *rgba))
        std::printf("[app] dump %s (%ux%u)\n", cfg.dump.c_str(), w, h);
    else
        std::fprintf(stderr, "[app] dump failed: cannot write %s\n",
                     cfg.dump.c_str());
}

void App::Impl::dump_state()
{
    if (!rt) return;

    if (const char* dump = std::getenv("OA_SCENEDUMP")) {
        const std::string prefix(dump);
        for (const oa::render::Layer* l : rt->scene().draw_order()) {
            if (!prefix.empty() && l->id.rfind(prefix, 0) != 0) continue;
            std::printf("[scene] %-16s left=%.0f top=%.0f w=%.0f h=%.0f "
                        "vis=%.0f alpha=%.3f eff=%d file=%s\n",
                        l->id.c_str(), l->left, l->top, l->width, l->height,
                        l->visible ? 1.0 : 0.0, l->alpha,
                        rt->scene().is_effectively_visible(l->id) ? 1 : 0,
                        l->file.c_str());
        }
    }
    if (std::getenv("OA_TXTDUMP")) {
        for (const auto& [id, mlp] : rt->text().all_layers()) {
            const oa::render::MessageLayer& ml = *mlp;
            std::string raw;
            for (const auto& u : ml.page)
                raw += u.kind == oa::render::PageUnit::Kind::Newline
                           ? std::string("\\n")
                           : u.data;
            const oa::render::Layer* node = rt->scene().find(id);
            std::fprintf(stderr,
                         "[txt] %-24s chars=%zu shown=%zu done=%d hidden=%d "
                         "positioned=%d page=%zu alpha=%.2f vis=%d eff=%d "
                         "box=(%.0f,%.0f %.0fx%.0f) size=%.0f align=%s "
                         "sample='%.40s'\n",
                         id.c_str(), ml.char_count, ml.shown,
                         ml.done ? 1 : 0, ml.text_hidden ? 1 : 0,
                         ml.positioned ? 1 : 0, ml.page.size(),
                         node ? node->alpha : -1.0,
                         node ? (node->visible ? 1 : 0) : -1,
                         node ? (rt->scene().is_effectively_visible(id) ? 1 : 0)
                              : -1,
                         ml.left, ml.top, ml.width, ml.height, ml.font.size(),
                         ml.font.get_or("align", "-").c_str(), raw.c_str());
        }
    }
    if (const char* vd = std::getenv("OA_VARDUMP")) {
        const std::string prefix(vd);
        const auto& vars = rt->interpreter().variables();
        auto dump_map = [&](const auto& map) {
            for (const auto& [k, v] : map) {
                if (!prefix.empty() && k.rfind(prefix, 0) != 0) continue;
                std::fprintf(stderr, "[var] %-28s = %s\n", k.c_str(),
                             v.to_string().c_str());
            }
        };
        dump_map(vars.local);
        dump_map(vars.temp);
        dump_map(vars.global);
        dump_map(vars.system);
    }

    size_t handler_layers = 0;
    for (const oa::render::Layer* l : rt->scene().draw_order())
        if (!l->event_handlers.empty()) ++handler_layers;

    std::printf("[app] headless end frames=%llu layers=%zu layer_ev=%zu "
                "text_ev=%zu switch_ev=%zu msg_lines=%zu handlers=%zu "
                "wait=%s media_ev=%zu audio_players=%zu bgm=%d se=%zu voice=%zu\n",
                (unsigned long long)frames, rt->scene().size(),
                rt->scene_layer_events(), rt->text_events(),
                rt->message_switch_events(),
                rt->text().visible_content_layers().size(), handler_layers,
                wait_desc(rt->current_wait()).c_str(), rt->media_events(),
                rt->media_players().active_players(),
                rt->audio().is_bgm_playing() ? 1 : 0,
                rt->audio().state().se_channels.size(),
                rt->audio().state().voice_channels.size());
    std::fflush(stdout);
}

bool App::Impl::boot()
{
    oa::log_build_info();

    const std::string& path = cfg.project;
    std::error_code ec;
    const bool is_dir = std::filesystem::is_directory(path, ec);
    std::shared_ptr<const oa::fs::IFileSystem> fs;
    try {
        fs = std::make_shared<oa::fs::PhysFileSystem>(path);
        if (is_dir) {
            std::printf("[app] folder project: %s\n", path.c_str());
        } else if (std::strcmp(fs->kind(), "pfs") == 0) {
            std::printf("[app] pfs project: %s\n", path.c_str());
        } else {
            const std::string side =
                std::filesystem::path(path).parent_path().string();
            std::printf("[app] pfs project: %s (sidecar dir layer: %s)\n",
                        path.c_str(), side.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cannot open project source: %s\n", e.what());
        return false;
    }

    rt = std::make_shared<GameRuntime>(fs);
    rt->enable_decode_pool();

    if (!cfg.save_root.empty()) {
        auto store = std::make_shared<oa::runtime::DirSaveStore>(cfg.save_root);
        std::error_code sec;
        std::filesystem::create_directories(std::filesystem::path(cfg.save_root), sec);
        rt->set_save_store(store);
        std::printf("[app] save store root: %s\n",
                    std::filesystem::absolute(cfg.save_root).string().c_str());
    }

    try {
        rt->open_project(cfg.platform);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "project open failed: %s\n", e.what());
        return false;
    }
    try {
        rt->boot_project();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "project boot failed: %s\n", e.what());
        return false;
    }
    rt->audio().set_master_volume(cfg.mute ? 0.0f : 1.0f);
    std::printf("[app] project booted\n");
    return true;
}

AppResult App::Impl::tick()
{
    if (!rt) return AppResult::Error;

    const Clock::time_point frame_start = Clock::now();
    uint64_t delta = 0;
    if (cfg.fixed_delta_ms > 0)
        delta = uint64_t(cfg.fixed_delta_ms);
    else if (cfg.headless)
        delta = 16;
    else
        delta = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_start - last_tick).count();
    last_tick = frame_start;
    try {
        rt->tick(delta > 0 ? delta : 1);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[app] tick error: %s\n", e.what());
        last_error = e.what();
        return AppResult::Error;
    }
    if (rt->exit_requested()) quit = true;

    ++frames;
    if (cfg.frames_target > 0 && frames >= cfg.frames_target) {
        quit = true;
        if (cfg.headless) dump_state();
        dump_frame();
    }
    if (cfg.headless) return quit ? AppResult::Exit : AppResult::Continue;

    const int cap = cfg.fps > 0
                        ? cfg.fps
                        : (rt->vsync_active() && rt->frame_presented()
                               ? rt->project().config.fps
                               : 0);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - frame_start).count();
    if (cap > 0) {
        const long want = 1000 / cap;
        if (elapsed < want)
            std::this_thread::sleep_for(std::chrono::milliseconds(want - elapsed));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    return quit ? AppResult::Exit : AppResult::Continue;
}

void App::Impl::reset_clock()
{
    last_tick = Clock::now();
}

void App::Impl::shutdown()
{
    if (shutdown_done) return;
    shutdown_done = true;
    std::printf("[app] bye (frames=%llu)\n",
                static_cast<unsigned long long>(frames));
    std::fflush(stdout);
    rt.reset();
}

App::App(AppConfig config) : impl_(std::make_unique<Impl>())
{
    impl_->cfg = std::move(config);
    impl_->reset_clock();
}

App::~App()
{
    impl_->shutdown();
}

bool App::boot()
{
    return impl_->boot();
}

bool App::attach_window(void* native_window)
{
    // 静音模式连音频设备都不开（headless / --mute）：没有 sink，任何一条
    // 音频通路（含视频自带音轨）都无处输出。
    if (!impl_->rt || !impl_->rt->attach_window(native_window, impl_->cfg.renderer,
                                                !impl_->cfg.mute))
        return false;
    impl_->reset_clock();
    return true;
}

int App::stage_width() const
{
    return impl_->rt ? impl_->rt->stage_width() : 0;
}

int App::stage_height() const
{
    return impl_->rt ? impl_->rt->stage_height() : 0;
}

AppResult App::run_headless()
{
    AppResult r = AppResult::Continue;
    while (r == AppResult::Continue) r = impl_->tick();
    return r;
}

void App::reset_clock()
{
    impl_->reset_clock();
}

void App::request_repaint()
{
    if (impl_->rt) impl_->rt->request_repaint();
}

void App::key_event(int vk, bool down)
{
    if (impl_->rt) impl_->rt->key_event(vk, down);
}

void App::mouse_move(float window_x, float window_y)
{
    if (impl_->rt) impl_->rt->mouse_move(window_x, window_y);
}

void App::mouse_button(int vk, bool down)
{
    if (impl_->rt) impl_->rt->mouse_button(vk, down);
}

void App::mouse_wheel(int direction)
{
    if (impl_->rt) impl_->rt->mouse_wheel(direction);
}

AppResult App::tick()
{
    return impl_->tick();
}

const std::string& App::last_error() const { return impl_->last_error; }

GameRuntime& App::runtime() { return *impl_->rt; }
const GameRuntime& App::runtime() const { return *impl_->rt; }

} // namespace oa::runtime
