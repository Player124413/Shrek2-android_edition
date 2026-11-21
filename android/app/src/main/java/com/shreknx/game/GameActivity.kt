package com.shreknx.game

import android.app.AlertDialog
import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.os.Process
import android.view.Display
import android.view.InputDevice
import android.view.KeyEvent
import android.view.Surface
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.RelativeLayout
import org.libsdl.app.SDLActivity

// Игра: SDLActivity (libSDL2.so + libmain.so, SDL_main из host/boot/main.c) в отдельном процессе :game.
// Поверх поверхности SDL — экранное управление (TouchControlsView) и, по шестерёнке, редактор раскладки.
class GameActivity : SDLActivity(), InputManager.InputDeviceListener, DisplayManager.DisplayListener {
    private lateinit var settings: Settings
    private var touch: TouchControlsView? = null
    private var editor: LayoutEditor? = null
    private var inputManager: InputManager? = null
    private var displayManager: DisplayManager? = null
    private val poll = object : Runnable {
        override fun run() {
            val t = touch ?: return
            val st = try { SnxNative.state() } catch (e: UnsatisfiedLinkError) { 0 }
            t.hiddenByMenu = st and SnxNative.STATE_MENU != 0
            t.postDelayed(this, 100)
        }
    }

    // универсальный APK: код русской версии игры — в libmain_rus.so (GameVariant), английской — в libmain.so
    override fun getLibraries(): Array<String> =
        if (GameVariant.isUniversal() && GameVariant.effective(this) == GameVariant.RUS) arrayOf("SDL2", "main_rus")
        else arrayOf("SDL2", "main")

    override fun getArguments(): Array<String> = intent.getStringArrayExtra(EXTRA_ARGS) ?: Settings(this).gameArgs()

    override fun onCreate(savedInstanceState: Bundle?) {
        settings = Settings(this)
        super.onCreate(savedInstanceState)
        if (SDLActivity.mBrokenLibraries || SDLActivity.mLayout == null) return
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // стабильная производительность: частоты ниже пика, но без троттлинга через 5–10 минут
        if (settings.sustained && Build.VERSION.SDK_INT >= 24) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            if (pm.isSustainedPerformanceModeSupported) window.setSustainedPerformanceMode(true)
        }
        val t = TouchControlsView(this, settings, editMode = false)
        t.onGear = { gearMenu() }
        SDLActivity.mLayout.addView(t, RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        touch = t
        inputManager = (getSystemService(Context.INPUT_SERVICE) as InputManager).also {
            it.registerInputDeviceListener(this, null)
        }
        updateGamepad()
        t.post(poll)
        pickDisplayMode()
        displayManager = (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).also {
            it.registerDisplayListener(this, null)
        }
        reportDisplay()
    }

    // Режим экрана под ограничение FPS: частота, равная ему (60 Гц при 60 FPS), иначе самая низкая кратная.
    // Без этого на экране 90/120 Гц ограничитель засыпал между обновлениями экрана и кадры шли рывками
    // (8/17/25 мс вперемешку), а при равной частоте кадры ровно отмеряет vsync (platform/window.c plat_set_display).
    private fun pickDisplayMode() {
        val cap = settings.fpsCap
        if (cap <= 0 || Build.VERSION.SDK_INT < 23) return
        @Suppress("DEPRECATION")
        val d = windowManager.defaultDisplay ?: return
        val cur = d.mode
        fun mult(m: Display.Mode): Int {
            val k = Math.round(m.refreshRate / cap)
            return if (k in 1..4 && Math.abs(m.refreshRate - k * cap) < 1.5f) k else 0
        }
        val best = d.supportedModes
            .filter { it.physicalWidth == cur.physicalWidth && it.physicalHeight == cur.physicalHeight && mult(it) > 0 }
            .minByOrNull { it.refreshRate } ?: return
        val lp = window.attributes
        lp.preferredDisplayModeId = best.modeId
        lp.preferredRefreshRate = best.refreshRate
        window.attributes = lp
    }

    // Частота поверхности (Android 11+): системе — что игра идёт ровно cap кадров в секунду
    private fun setSurfaceFrameRate() {
        val cap = settings.fpsCap
        if (cap <= 0 || Build.VERSION.SDK_INT < 30) return
        try {
            val s = SDLActivity.mSurface?.holder?.surface ?: return
            if (s.isValid) s.setFrameRate(cap.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        } catch (e: Exception) {
        }
    }

    private fun reportDisplay() {
        @Suppress("DEPRECATION")
        val hz = windowManager.defaultDisplay?.refreshRate ?: return
        try {
            SnxNative.display(hz, settings.fpsCap)
        } catch (e: UnsatisfiedLinkError) {
        }
    }

    override fun onDisplayAdded(displayId: Int) {}
    override fun onDisplayRemoved(displayId: Int) {}
    override fun onDisplayChanged(displayId: Int) {
        if (displayId == Display.DEFAULT_DISPLAY) reportDisplay()
    }

    override fun onDestroy() {
        inputManager?.unregisterInputDeviceListener(this)
        displayManager?.unregisterDisplayListener(this)
        // libmain.so не умеет запускаться второй раз в том же процессе (окно гостя, глобальные состояния):
        // процесс :game завершается вместе с игрой, следующий запуск — с чистого листа. Сразу, до
        // SDLActivity.onDestroy: тот ждёт конца потока SDL без срока, а игра на SDL_QUIT может и не выйти
        if (isFinishing) quitNow()
        super.onDestroy()
    }

    private fun quitNow() {
        try {
            SnxNative.exitNow()  // сбросить записанное игрой (сохранения, User.ini) и выйти
        } catch (e: UnsatisfiedLinkError) {
        }
        Process.killProcess(Process.myPid())
    }

    override fun onPause() {
        touch?.releaseAll()
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        // возврат из фона: пальцы, бывшие на экране, забыты; опрос состояния игры — заново (мог остановиться)
        touch?.let {
            it.releaseAll()
            it.removeCallbacks(poll)
            it.post(poll)
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            immersive()
            setSurfaceFrameRate()
            reportDisplay()
        }
    }

    @Suppress("DEPRECATION")
    private fun immersive() {
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN)
    }

    // шестерёнка: редактор раскладки; при включённых читах — выбор «Раскладка» / «Читы»
    private fun gearMenu() {
        if (!settings.cheats) return openEditor()
        touch?.releaseAll()
        AlertDialog.Builder(this)
            .setItems(arrayOf(L.t("✎  Редактор раскладки", "✎  Layout editor"), L.t("★  Читы…", "★  Cheats…"))) { _, i ->
                if (i == 0) openEditor() else showCheats()
            }
            .setOnDismissListener { immersive() }
            .show()
    }

    private var cheatsOpen = false

    private fun showCheats() {
        if (cheatsOpen) return
        cheatsOpen = true
        touch?.releaseAll()
        Cheats.show(this) { cheatsOpen = false; immersive() }
    }

    // геймпад: оба стика нажаты (L3+R3) — меню читов (если включены в лаунчере)
    private var thumbL = false
    private var thumbR = false

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val down = event.action == KeyEvent.ACTION_DOWN
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_THUMBL -> thumbL = down
            KeyEvent.KEYCODE_BUTTON_THUMBR -> thumbR = down
        }
        if (down && thumbL && thumbR && settings.cheats && event.repeatCount == 0) {
            thumbL = false
            thumbR = false
            showCheats()
        }
        return super.dispatchKeyEvent(event)
    }

    // редактор поверх игры (игра идёт дальше, экранный геймпад отпущен)
    private fun openEditor() {
        if (editor != null) return
        touch?.releaseAll()
        val ed = LayoutEditor(this, settings) { saved ->
            SDLActivity.mLayout.removeView(editor!!.root)
            editor = null
            if (saved) touch?.controls = TouchLayout.load(this)
            touch?.invalidate()
            touch?.visibility = View.VISIBLE
        }
        editor = ed
        touch?.visibility = View.GONE
        ed.field.setBackgroundColor(0x99000000.toInt())
        SDLActivity.mLayout.addView(ed.root, RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }

    override fun onBackPressed() {
        val ed = editor
        if (ed != null) {
            SDLActivity.mLayout.removeView(ed.root)
            editor = null
            touch?.visibility = View.VISIBLE
            return
        }
        // «Назад» не закрывает игру молча (несохранённое пропало бы): спросить
        touch?.releaseAll()
        AlertDialog.Builder(this)
            .setTitle(L.t("Выйти из игры?", "Quit the game?"))
            .setMessage(L.t("Несохранённый прогресс пропадёт. Меню игры — кнопка «Меню».",
                "Unsaved progress will be lost. The game menu is the “Menu” button."))
            .setPositiveButton(L.t("Выйти", "Quit")) { _, _ -> quitNow() }
            .setNegativeButton(L.t("Отмена", "Cancel"), null)
            .setOnDismissListener { immersive() }
            .show()
    }

    // ---------------------------------------------------------------- геймпад: экранное управление прячется
    private fun updateGamepad() {
        var pad = false
        for (id in InputDevice.getDeviceIds()) {
            val d = InputDevice.getDevice(id) ?: continue
            val s = d.sources
            if (!d.isVirtual && (s and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                    s and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)) pad = true
        }
        touch?.hiddenByGamepad = pad && settings.hideWithGamepad
    }

    override fun onInputDeviceAdded(deviceId: Int) = updateGamepad()
    override fun onInputDeviceRemoved(deviceId: Int) = updateGamepad()
    override fun onInputDeviceChanged(deviceId: Int) = updateGamepad()

    companion object {
        const val EXTRA_ARGS = "snx.args"
    }
}
