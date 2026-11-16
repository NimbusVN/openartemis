package org.openartemis;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Objects;

/**
 * openartemis 内核显示面：SDLActivity + 原生库 libopenartemis.so。
 *
 * 宿主用 {@link #createIntent} 启动（extra 键见 {@link #EXTRA_ARGV}）。
 * argv 解析见 environ/main.cpp（`[options] [project.pfs]`）：
 *   <数据源路径> --platform android [--renderer gles]
 * 数据源可以是单个 .pfs 包，也可以是含 system.ini 的解包目录。
 */
public class GameActivity extends SDLActivity {
    /** 每游戏存档根（可选）：引擎的存档根策略读环境变量 OA_SAVE_ROOT。 */
    public static final String EXTRA_SAVE_ROOT = "oa_save_root";
    /** 宿主启动本内核的入口契约：argv 列表。 */
    public static final String EXTRA_ARGV = "org.openartemis.ARGV";

    private ArrayList<String> m_gameargs = new ArrayList<>();

    public static Intent createIntent(Context context, ArrayList<String> argv, String saveRoot) {
        Intent intent = new Intent(context, GameActivity.class);
        intent.putStringArrayListExtra(EXTRA_ARGV, argv);
        if (saveRoot != null && !saveRoot.isEmpty()) {
            intent.putExtra(EXTRA_SAVE_ROOT, saveRoot);
        }
        return intent;
    }

    // override sdl functions
    static {
        System.loadLibrary("SDL3");
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "openartemis"
        };
    }

    @Override
    protected String[] getArguments() {
        return m_gameargs.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        SDLActivity.nativeSetenv("SDL_ANDROID_ALLOW_RECREATE_ACTIVITY", "1");
        Intent intent = getIntent();
        ArrayList<String> args = intent.getStringArrayListExtra(EXTRA_ARGV);
        if (args != null) m_gameargs = args;
        // 存档隔离：引擎在 main() 里读 OA_SAVE_ROOT（见 host_save_root），
        // 必须在原生线程启动前设好 —— 这里在 super.onCreate 之前。
        String saveRoot = intent.getStringExtra(EXTRA_SAVE_ROOT);
        if (saveRoot != null && !saveRoot.isEmpty()) {
            SDLActivity.nativeSetenv("OA_SAVE_ROOT", saveRoot);
            Log.i("## openartemis", "OA_SAVE_ROOT=" + saveRoot);
        }
        Log.i("## openartemis", "args=" + m_gameargs);
        super.onCreate(savedInstanceState);
        this.fullscreen();
    }

    @Override
    protected void onResume() {
        super.onResume();
        this.fullscreen();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (hasFocus) this.fullscreen();
    }

    /**
     * 只负责 UI 层的两件事：藏 ActionBar、锁横屏。
     *
     * **全屏交给 SDL**：窗口带 SDL_WINDOW_FULLSCREEN 建立（见
     * environ/main.cpp 的 win_flags），SDL 的 JNI 会调 SDLActivity.setWindowStyle
     * 置 immersive-sticky 那套 flag，并置 mFullscreenModeActive —— 之后系统栏被
     * 划出来还会由 SDL 的 onSystemUiVisibilityChange 自动重新隐藏。
     *
     * 这里**不要**再自己 setSystemUiVisibility：那样只设了 flag、没置 SDL 的
     * mFullscreenModeActive，两个状态互相打断，效果就是"全屏没生效"。
     */
    private void fullscreen() {
        try {
            Objects.requireNonNull(this.getSupportActionBar()).hide();
        } catch (NullPointerException ignored) {
        }
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }
    // 注：openartemis 的原生侧不需要 AssetManager（数据源由 argv 指定的
    // 路径经 PhysicsFS 挂载）。
}