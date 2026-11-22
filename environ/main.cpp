// openartemis — SDL layer. All SDL types/events stay here; the engine host
// only receives translated input and frame ticks.
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_touch.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/runtime/runtime.h"
#include "core/runtime/runtime_game.h"
#include "platform/Platform.h"

namespace {

enum class TouchPhase { Idle, SingleFinger, MultiFinger };

struct TouchFinger {
    float x = 0.0f;
    float y = 0.0f;
    float start_x = 0.0f;
    float start_y = 0.0f;
    bool moved = false;
};

struct HostState {
    oa::runtime::App* app = nullptr;
    SDL_Window* window = nullptr;
    bool sdl_inited = false;
    bool plat_inited = false;
    bool error_reported = false;
    /// 已经弹过提示的"未实现指令"（同一名字只提示一次）。
    std::set<std::string> reported_unknown;

    TouchPhase touch_phase = TouchPhase::Idle;
    std::map<SDL_FingerID, TouchFinger> fingers;
    Uint8 pending_button = 0;
    float pending_x = 0.0f;
    float pending_y = 0.0f;
};

// 日志时间戳。
std::string local_stamp()
{
    char stamp[32] = "";
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    if (localtime_s(&tmv, &now) == 0) std::strftime(stamp, sizeof(stamp), "%F %T", &tmv);
#else
    if (localtime_r(&now, &tmv) != nullptr) std::strftime(stamp, sizeof(stamp), "%F %T", &tmv);
#endif
    return stamp;
}

// 引擎不认识的指令：写日志 + 提示一次。
//
// 静默吞掉的后果是"页面出来了但没字""按钮点了没反应"这类哑故障 ——
// `[tag data="print,..."]` 就是这么丢了很久（回顾页只剩空行框）。宁可
// 打断一下，也要让"某条指令没实现"这件事可见。
void report_unknown_tags(HostState* host)
{
    return; // 无阻碍模式
    if (!host || !host->app) return;
    auto& rt = host->app->runtime();
    const auto& unknown = rt.interpreter().unknown_tags();
    if (unknown.empty()) return;

    std::vector<std::string> fresh;
    for (const auto& [name, count] : unknown) {
        (void)count;
        if (host->reported_unknown.insert(name).second) fresh.push_back(name);
    }
    if (fresh.empty()) return;

    std::sort(fresh.begin(), fresh.end());
    const std::string* script = rt.interpreter().current_script();
    const std::string where = script ? *script + ":" + std::to_string(rt.interpreter().current_line())
                                     : std::string();

    // 日志
    std::string log_path;
    {
        std::string dir;
        if (const char* base = SDL_GetBasePath()) dir = base;
        if (dir.empty()) {
            std::error_code ec;
            dir = std::filesystem::current_path(ec).string();
        }
        if (!dir.empty()) log_path = dir + "oa_error.log";
    }
    if (!log_path.empty()) {
        if (std::FILE* f = std::fopen(log_path.c_str(), "a")) {
            const std::string stamp = local_stamp();
            for (const std::string& name : fresh)
                std::fprintf(f, "[%s] unknown tag: %s (at %s)\n", stamp.c_str(), name.c_str(),
                             where.c_str());
            std::fclose(f);
        }
    }

    std::string msg = "以下指令本引擎还没有实现，相关表现可能缺失：\n\n";
    for (size_t i = 0; i < fresh.size() && i < 12; ++i) {
        msg += "  • " + fresh[i] + "\n";
    }
    if (fresh.size() > 12) msg += "  … 共 " + std::to_string(fresh.size()) + " 个\n";
    if (!where.empty()) msg += "\n位置: " + where;
    if (!log_path.empty()) msg += "\n日志: " + log_path;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "openartemis 未实现指令", msg.c_str(),
                             host->window);
}

// 脚本/引擎错误不再让窗口无声消失：写一行日志，然后弹一个原生对话框告诉
// 玩家发生了什么、日志在哪。没有这个之前，任何 ScriptError 都会让进程
// 直接退出（玩家报的"点太快就闪退"）。
void report_fatal_error(HostState* host)
{
    if (!host || !host->app || host->error_reported) return;
    host->error_reported = true;

    std::string what = host->app->last_error();
    if (what.empty()) what = "unknown error";

    // 出错位置（能取到就带上，日志里比弹窗更有用）。
    std::string where;
    auto& rt = host->app->runtime();
    if (const std::string* s = rt.interpreter().current_script())
        where = *s + ":" + std::to_string(rt.interpreter().current_line());

    // 日志落在 exe 旁边（写不进去就退到当前目录）。
    std::string log_path;
    {
        std::string dir;
        if (const char* base = SDL_GetBasePath()) dir = base;
        if (dir.empty()) {
            std::error_code ec;
            dir = std::filesystem::current_path(ec).string();
        }
        if (!dir.empty()) log_path = dir + "oa_error.log";
    }
    if (!log_path.empty()) {
        if (std::FILE* f = std::fopen(log_path.c_str(), "a")) {
            const std::string stamp = local_stamp();
            std::fprintf(f, "[%s] %s%s%s\n", stamp.c_str(), what.c_str(), where.empty() ? "" : " @ ",
                         where.c_str());
            std::fclose(f);
        }
    }

    std::string msg = "游戏遇到一个错误，已停止运行。\n\n" + what;
    if (!where.empty()) msg += "\n位置: " + where;
    if (!log_path.empty()) msg += "\n\n详细信息已写入:\n" + log_path;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "openartemis 运行错误", msg.c_str(),
                             host->window);
}

void print_usage(const char* argv0)
{
    std::printf(
        "usage: %s [options] [project.pfs]\n"
        "\n"
        "options:\n"
        "  --frames N           run N frames then exit\n"
        "  --dump PATH          write the final frame as PPM\n"
        "  --headless           no window/renderer, fixed 16ms ticks\n"
        "  --fixed-delta-ms N   deterministic tick step (windowed too)\n"
        "  --fps N              target frame rate (default: real-time pacing)\n"
        "  --platform NAME      project platform (default: build OS)\n"
        "  --renderer NAME      'sdl' (default) or 'gles'\n"
        "  --save-dir PATH      writable save directory (overrides OA_SAVE_ROOT)\n"
        "  --mute               mute audio\n"
        "  -h, --help           show this help\n"
        "\n"
        "env:\n"
        "  OA_PFS=path          default project\n"
        "  OA_SAVE_ROOT=path    override the writable save root\n"
        "  OA_RENDERER=sdl|gles default renderer\n",
        argv0 ? argv0 : "openartemis");
}

// 0 = ok, 1 = help printed, 2 = usage error printed.
int parse_args(int argc, char** argv, oa::runtime::AppConfig& cfg)
{
#if defined(__ANDROID__)
    cfg.platform = "android";
#elif defined(__EMSCRIPTEN__)
    cfg.platform = "wasm";
#else
    cfg.platform = "windows";
#endif
    if (const char* p = std::getenv("OA_PFS"); p && *p) cfg.project = p;
    if (const char* r = std::getenv("OA_RENDERER"); r && *r) cfg.renderer = r;

    auto usage_error = [&](const std::string& msg) {
        std::fprintf(stderr, "openartemis: %s\n", msg.c_str());
        print_usage(argv[0]);
        return 2;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 1;
        }

        const auto eq = a.find('=');
        const std::string name = eq == std::string::npos ? a : a.substr(0, eq);
        auto value_of = [&](std::string& out) -> bool {
            if (eq != std::string::npos) {
                out = a.substr(eq + 1);
                return true;
            }
            if (i + 1 < argc) {
                out = argv[++i];
                return true;
            }
            return false;
        };

        if (name == "--mute") {
            cfg.mute = true;
        } else if (name == "--headless") {
            cfg.headless = true;
        } else if (name == "--frames") {
            std::string v;
            if (!value_of(v)) return usage_error("--frames needs a positive frame count");
            char* end = nullptr;
            errno = 0;
            const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (errno || !end || *end || n == 0)
                return usage_error("--frames: expected a positive integer, got '" + v + "'");
            cfg.frames_target = n;
        } else if (name == "--fixed-delta-ms") {
            std::string v;
            if (!value_of(v)) return usage_error("--fixed-delta-ms needs a positive value");
            char* end = nullptr;
            errno = 0;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (errno || !end || *end || n <= 0)
                return usage_error("--fixed-delta-ms: expected a positive integer, got '" + v + "'");
            cfg.fixed_delta_ms = static_cast<int>(n);
        } else if (name == "--dump") {
            std::string v;
            if (!value_of(v) || v.empty()) return usage_error("--dump needs a file path");
            cfg.dump = v;
        } else if (name == "--fps") {
            std::string v;
            if (!value_of(v)) return usage_error("--fps needs a positive frame rate");
            char* end = nullptr;
            errno = 0;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (errno || !end || *end || n <= 0)
                return usage_error("--fps: expected a positive integer, got '" + v + "'");
            cfg.fps = static_cast<int>(n);
        } else if (name == "--platform") {
            std::string v;
            if (!value_of(v) || v.empty()) return usage_error("--platform needs a name");
            cfg.platform = v;
        } else if (name == "--renderer") {
            std::string v;
            if (!value_of(v) || v.empty()) return usage_error("--renderer needs a name");
            cfg.renderer = v;
        } else if (name == "--save-dir") {
            std::string v;
            if (!value_of(v) || v.empty()) return usage_error("--save-dir needs a path");
            cfg.save_root = v;
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            return usage_error("unknown option '" + a + "'");
        } else {
            cfg.project = a;
        }
    }

    if (cfg.renderer != "sdl" && cfg.renderer != "gles")
        return usage_error("--renderer: expected 'sdl' or 'gles', got '" +
                           cfg.renderer + "'");
    return 0;
}

int sdl_key_to_vk(SDL_Keycode key)
{
    if (key == SDLK_ESCAPE) return 0;
    SDL_Keymod mods = 0;
    const SDL_Scancode sc = SDL_GetScancodeFromKey(key, &mods);
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_0 && sc <= SDL_SCANCODE_9)
        return '0' + (sc - SDL_SCANCODE_0);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 112 + (sc - SDL_SCANCODE_F1);
    switch (key) {
        case SDLK_RETURN: return 13;
        case SDLK_SPACE: return 32;
        case SDLK_BACKSPACE: return 8;
        case SDLK_TAB: return 9;
        case SDLK_LEFT: return 37;
        case SDLK_UP: return 38;
        case SDLK_RIGHT: return 39;
        case SDLK_DOWN: return 40;
        case SDLK_HOME: return 36;
        case SDLK_END: return 35;
        case SDLK_PAGEUP: return 33;
        case SDLK_PAGEDOWN: return 34;
        case SDLK_INSERT: return 45;
        case SDLK_DELETE: return 46;
        default: break;
    }
    if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL) return 17;
    if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT) return 16;
    if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT) return 18;
    return 0;
}

int sdl_button_to_vk(Uint8 button)
{
    return button == SDL_BUTTON_LEFT     ? 1
           : button == SDL_BUTTON_RIGHT  ? 2
           : button == SDL_BUTTON_MIDDLE ? 3
                                         : 0;
}

void touch_tap(HostState* host, int vk, float x, float y)
{
    host->app->mouse_move(x, y);
    host->app->mouse_button(vk, true);
    host->pending_button = Uint8(vk);
    host->pending_x = x;
    host->pending_y = y;
}

void touch_handle(HostState* host, const SDL_Event& ev)
{
    if (!host->window || !host->app) return;
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(host->window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    const float wx = ev.tfinger.x * float(win_w);
    const float wy = ev.tfinger.y * float(win_h);

    if (ev.type == SDL_EVENT_FINGER_DOWN) {
        TouchFinger f;
        f.x = f.start_x = wx;
        f.y = f.start_y = wy;
        host->fingers[ev.tfinger.fingerID] = f;
        if (host->fingers.size() == 1) {
            host->touch_phase = TouchPhase::SingleFinger;
            // 没有真实 hover：按下即把指针落到位，让"按住不动"等价于桌面端把鼠标停在该处。
            host->app->mouse_move(wx, wy);
        } else if (host->fingers.size() == 2) {
            host->touch_phase = TouchPhase::MultiFinger;
        } else {
            host->touch_phase = TouchPhase::Idle;
        }
        return;
    }

    if (ev.type == SDL_EVENT_FINGER_CANCELED) {
        // 手势被系统打断（返回手势/来电/切后台）：必须摘掉手指，否则残留
        // 的 finger 会让下一次单指触摸被当成双指（右键）。
        host->fingers.erase(ev.tfinger.fingerID);
        if (host->fingers.empty())
            host->touch_phase = TouchPhase::Idle;
        else if (host->fingers.size() == 1)
            host->touch_phase = TouchPhase::SingleFinger;
        else
            host->touch_phase = TouchPhase::MultiFinger;
        return;
    }

    if (ev.type == SDL_EVENT_FINGER_MOTION) {
        auto it = host->fingers.find(ev.tfinger.fingerID);
        if (it == host->fingers.end()) return;
        TouchFinger& f = it->second;
        f.x = wx;
        f.y = wy;
        const float dx = wx - f.start_x;
        const float dy = wy - f.start_y;
        constexpr float kMoveThreshold = 12.0f;
        if (dx * dx + dy * dy > kMoveThreshold * kMoveThreshold) f.moved = true;
        if (host->touch_phase == TouchPhase::SingleFinger)
            host->app->mouse_move(f.x, f.y);
        return;
    }

    if (ev.type == SDL_EVENT_FINGER_UP) {
        auto it = host->fingers.find(ev.tfinger.fingerID);
        if (it == host->fingers.end()) return;
        const TouchFinger f = it->second;
        const int remaining = int(host->fingers.size()) - 1;
        host->fingers.erase(it);
        if (remaining > 0) return;

        // One finger = left click, two fingers = right click. Drag is not a tap.
        if (host->touch_phase == TouchPhase::MultiFinger && !f.moved)
            touch_tap(host, 2, f.x, f.y);
        else if (host->touch_phase == TouchPhase::SingleFinger && !f.moved)
            touch_tap(host, 1, f.x, f.y);
        host->touch_phase = TouchPhase::Idle;
    }
}

void touch_flush_release(HostState* host)
{
    if (!host->pending_button || !host->app) return;
    host->app->mouse_button(int(host->pending_button), false);
    host->pending_button = 0;
}

} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    oa::runtime::AppConfig cfg;
    const int parsed = parse_args(argc, argv, cfg);
    if (parsed == 1) return SDL_APP_SUCCESS;
    if (parsed == 2) return SDL_APP_FAILURE;
    if (cfg.project.empty()) {
        std::fprintf(stderr, "openartemis: no project given\n");
        print_usage(argv[0]);
        return SDL_APP_FAILURE;
    }

    auto* host = new HostState();
    *appstate = host;

    SDL_SetHintWithPriority(SDL_HINT_MAIN_CALLBACK_RATE, "0", SDL_HINT_OVERRIDE);
    const bool headless = cfg.headless;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (!headless) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        std::fprintf(stderr, "[app] headless: SDL video unavailable (%s); "
                             "running without a renderer\n", SDL_GetError());
    } else {
        host->sdl_inited = true;
    }
    oa::plat::init();
    host->plat_inited = true;

    // 存档根：命令行优先，其次环境变量（Android 壳历史写法），最后平台默认。
    if (cfg.save_root.empty()) {
        if (const char* root = std::getenv("OA_SAVE_ROOT"); root && *root) {
            cfg.save_root = root;
        } else {
            std::error_code ec;
            const bool is_dir = std::filesystem::is_directory(cfg.project, ec);
            cfg.save_root = oa::plat::default_save_root(cfg.project, is_dir);
        }
    }
    const std::string renderer = cfg.renderer;

    host->app = new oa::runtime::App(std::move(cfg));
    if (!host->app->boot()) return SDL_APP_FAILURE;
    if (headless) {
        if (host->sdl_inited) {
            host->window = SDL_CreateWindow(
                "openartemis", host->app->stage_width(),
                host->app->stage_height(), SDL_WINDOW_HIDDEN);
            if (!host->window) {
                std::fprintf(stderr, "[app] headless window failed: %s\n",
                             SDL_GetError());
            } else if (!host->app->attach_window(host->window)) {
                std::fprintf(stderr, "[app] headless renderer failed: %s\n",
                             SDL_GetError());
            }
        }
        const oa::runtime::AppResult r = host->app->run_headless();
        return r == oa::runtime::AppResult::Error ? SDL_APP_FAILURE
                                                  : SDL_APP_SUCCESS;
    }

    Uint32 flags = SDL_WINDOW_RESIZABLE;
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(__IPHONEOS__)
    flags = (flags | SDL_WINDOW_FULLSCREEN) & ~SDL_WINDOW_RESIZABLE;
#endif
    if (renderer == "gles") flags |= SDL_WINDOW_OPENGL;

    host->window = SDL_CreateWindow("openartemis", host->app->stage_width(),
                                    host->app->stage_height(), flags);
    if (!host->window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(__IPHONEOS__)
    if (!SDL_SetWindowFullscreen(host->window, true))
        std::fprintf(stderr, "[app] SDL_SetWindowFullscreen failed: %s\n",
                     SDL_GetError());
#endif
    if (!host->app->attach_window(host->window)) return SDL_APP_FAILURE;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* ev)
{
    HostState* host = static_cast<HostState*>(appstate);
    if (!host || !host->app) return SDL_APP_CONTINUE;

    if (ev->type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;
    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE)
        return SDL_APP_SUCCESS;

    if (ev->type == SDL_EVENT_WINDOW_RESIZED ||
        ev->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        ev->type == SDL_EVENT_WINDOW_EXPOSED) {
        host->app->request_repaint();
        return SDL_APP_CONTINUE;
    }

    if (ev->type == SDL_EVENT_FINGER_DOWN || ev->type == SDL_EVENT_FINGER_MOTION ||
        ev->type == SDL_EVENT_FINGER_UP || ev->type == SDL_EVENT_FINGER_CANCELED) {
        touch_handle(host, *ev);
        return SDL_APP_CONTINUE;
    }

    if (ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP) {
        const int vk = sdl_key_to_vk(ev->key.key);
        if (vk > 0) host->app->key_event(vk, ev->type == SDL_EVENT_KEY_DOWN);
    } else if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        host->app->mouse_wheel(ev->wheel.y > 0 ? 1 : (ev->wheel.y < 0 ? -1 : 0));
    } else if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        // SDL 会把触摸额外合成一套鼠标事件（which=SDL_TOUCH_MOUSEID）。
        // 触屏已由 touch_handle 单独翻译，这套合成事件再进来就会让一次
        // 点按变成两次点击（按下补字、抬起翻页）。真实鼠标/触控板事件
        // 的 which 是鼠标实例 id，不受影响，照常进入引擎。
        if (ev->motion.which == SDL_TOUCH_MOUSEID) return SDL_APP_CONTINUE;
        host->app->mouse_move(ev->motion.x, ev->motion.y);
    } else if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
               ev->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (ev->button.which == SDL_TOUCH_MOUSEID) return SDL_APP_CONTINUE;
        const int vk = sdl_button_to_vk(ev->button.button);
        if (vk > 0)
            host->app->mouse_button(vk, ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    HostState* host = static_cast<HostState*>(appstate);
    if (!host || !host->app) return SDL_APP_SUCCESS;

    if (oa::plat::take_lifecycle_resume()) host->app->reset_clock();

    switch (host->app->tick()) {
        case oa::runtime::AppResult::Continue:
            report_unknown_tags(host);
            touch_flush_release(host);
            return SDL_APP_CONTINUE;
        case oa::runtime::AppResult::Exit:
            return SDL_APP_SUCCESS;
        case oa::runtime::AppResult::Error:
            report_fatal_error(host);
            return SDL_APP_FAILURE;
    }
    return SDL_APP_FAILURE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    (void)result;
    HostState* host = static_cast<HostState*>(appstate);
    if (!host) return;

    delete host->app;
    if (host->window) SDL_DestroyWindow(host->window);
    if (host->plat_inited) oa::plat::shutdown();
    if (host->sdl_inited) SDL_Quit();
    delete host;
}
