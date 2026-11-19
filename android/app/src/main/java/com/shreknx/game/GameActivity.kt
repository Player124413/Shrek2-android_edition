package com.shreknx.game

import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.os.Process
import android.view.InputDevice
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.RelativeLayout
import org.libsdl.app.SDLActivity

// Игра: SDLActivity (libSDL2.so + libmain.so, SDL_main из host/boot/main.c) в отдельном процессе :game.
// Поверх поверхности SDL — экранное управление (TouchControlsView) и, по шестерёнке, редактор раскладки.
class GameActivity : SDLActivity(), InputManager.InputDeviceListener {
    private lateinit var settings: Settings
    private var touch: TouchControlsView? = null
    private var editor: LayoutEditor? = null
    private var inputManager: InputManager? = null
    private val poll = object : Runnable {
        override fun run() {
            val t = touch ?: return
            val st = try { SnxNative.state() } catch (e: UnsatisfiedLinkError) { 0 }
            t.hiddenByMenu = st and SnxNative.STATE_MENU != 0
            t.postDelayed(this, 100)
        }
    }

    override fun getLibraries(): Array<String> = arrayOf("SDL2", "main")

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
        t.onGear = { openEditor() }
        SDLActivity.mLayout.addView(t, RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        touch = t
        inputManager = (getSystemService(Context.INPUT_SERVICE) as InputManager).also {
            it.registerInputDeviceListener(this, null)
        }
        updateGamepad()
        t.post(poll)
    }

    override fun onDestroy() {
        inputManager?.unregisterInputDeviceListener(this)
        val finishing = isFinishing
        super.onDestroy()
        // libmain.so не умеет запускаться второй раз в том же процессе (окно гостя, глобальные состояния):
        // процесс :game завершается вместе с игрой, следующий запуск — с чистого листа
        if (finishing) Process.killProcess(Process.myPid())
    }

    override fun onPause() {
        touch?.releaseAll()
        super.onPause()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) immersive()
    }

    @Suppress("DEPRECATION")
    private fun immersive() {
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN)
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
        @Suppress("DEPRECATION")
        super.onBackPressed()
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
