package com.shreknx.game

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings as SysSettings
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.io.RandomAccessFile
import java.util.concurrent.atomic.AtomicBoolean

// Лаунчер: папка игры (импорт или путь), графика (пресеты по силе телефона), управление (редактор раскладки),
// дополнительно (кэш, переменные SNX_*, журнал). «Играть» запускает GameActivity в процессе :game.
class LauncherActivity : Activity() {
    private lateinit var s: Settings
    private lateinit var content: LinearLayout
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        s = Settings(this)
        build()
    }

    override fun onResume() {
        super.onResume()
        if (stale) {  // игра (процесс :game) или редактор могли поменять настройки: перечитать и перерисовать
            stale = false
            s = Settings(this)
            build()
        }
        refreshStatus()
    }

    private var stale = false

    private fun build() {
        val scroll = ScrollView(this).apply { setBackgroundColor(Ui.BG) }
        content = Ui.vbox(this, 18)
        scroll.addView(content)
        setContentView(scroll)

        // заголовок и «Играть»
        val head = Ui.hbox(this).apply { setPadding(0, 0, 0, Ui.dp(this@LauncherActivity, 12f)) }
        val titles = Ui.vbox(this)
        titles.addView(Ui.title(this, getString(R.string.app_name), 28f))
        titles.addView(Ui.label(this, "shrekNX ${BuildConfig.VERSION_NAME} · ${if (Settings.variant() == "rus") "русская версия" else "English"}",
            dim = true, size = 13f))
        status = Ui.label(this, "", size = 14f).apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, 0) }
        titles.addView(status)
        head.addView(titles, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(Ui.button(this, "▶  Играть", primary = true) { play() })
        content.addView(head)

        val cols = Ui.hbox(this).apply { gravity = Gravity.TOP }
        val left = Ui.vbox(this)
        val right = Ui.vbox(this)
        val gap = Ui.dp(this, 12f)
        cols.addView(left, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { rightMargin = gap })
        cols.addView(right, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        content.addView(cols)

        left.addView(gameCard())
        left.addView(controlsCard())
        right.addView(graphicsCard())
        right.addView(advancedCard())
    }

    // ---------------------------------------------------------------- папка игры
    private fun gameCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, "Файлы игры"))
        val path = Ui.label(this, s.gameDir, dim = true, size = 12f)
        c.addView(path)
        c.addView(Ui.label(this, "Нужна папка установленной игры с ПК (System, Maps, Textures, Sounds…). Проще всего — " +
            "скопировать её по USB в папку по умолчанию:\n${Settings.defaultGameDir(this).absolutePath}",
            dim = true, size = 12f).apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, Ui.dp(this@LauncherActivity, 8f)) })
        val row1 = Ui.hbox(this)
        row1.addView(Ui.button(this, "Импорт из папки…") { pickImport() }, wrapGap())
        row1.addView(Ui.button(this, "Указать папку…") { pickPath() }, wrapGap())
        c.addView(row1)
        val row2 = Ui.hbox(this).apply { setPadding(0, Ui.dp(this@LauncherActivity, 8f), 0, 0) }
        row2.addView(Ui.button(this, "По умолчанию") {
            s.gameDir = Settings.defaultGameDir(this).absolutePath
            Settings.defaultGameDir(this).mkdirs()
            path.text = s.gameDir
            refreshStatus()
        }, wrapGap())
        c.addView(row2)
        pathLabel = path
        return c
    }

    private var pathLabel: TextView? = null

    private fun wrapGap() = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        .apply { rightMargin = Ui.dp(this@LauncherActivity, 8f) }

    private fun refreshStatus() {
        if (!::status.isInitialized) return
        val err = GameFolder.check(s.gameDir)
        status.text = err?.let { "⚠ $it" } ?: "✓ Игра найдена"
        status.setTextColor(if (err == null) Ui.ACCENT else 0xFFFFB74D.toInt())
        pathLabel?.text = s.gameDir
    }

    private fun pickImport() {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_IMPORT)
    }

    private fun pickPath() {
        if (!haveFileAccess()) {
            requestFileAccess()
            return
        }
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_PATH)
    }

    private fun haveFileAccess(): Boolean = if (Build.VERSION.SDK_INT >= 30) Environment.isExternalStorageManager()
        else checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED

    private fun requestFileAccess() {
        AlertDialog.Builder(this).setTitle("Доступ к файлам")
            .setMessage("Чтобы игра читала файлы прямо из выбранной папки, нужно разрешение на доступ ко всем файлам. " +
                "Без него используй «Импорт» — файлы скопируются в папку приложения.")
            .setPositiveButton("Разрешить") { _, _ ->
                if (Build.VERSION.SDK_INT >= 30) {
                    try {
                        startActivity(Intent(SysSettings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:$packageName")))
                    } catch (e: Exception) {
                        startActivity(Intent(SysSettings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
                    }
                } else requestPermissions(arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE), REQ_PERM)
            }.setNegativeButton("Отмена", null).show()
    }

    @Deprecated("Activity API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data ?: return
        if (resultCode != RESULT_OK) return
        when (requestCode) {
            REQ_PATH -> {
                val p = GameFolder.pathOfTree(uri)
                if (p == null) {
                    toastDialog("Эту папку нельзя открыть по пути. Используй «Импорт».")
                    return
                }
                s.gameDir = p
                refreshStatus()
            }
            REQ_IMPORT -> runImport(uri)
        }
    }

    private fun runImport(tree: Uri) {
        val dst = Settings.defaultGameDir(this)
        val box = Ui.vbox(this, 20)
        val what = Ui.label(this, "Подготовка…")
        val bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { isIndeterminate = true }
        box.addView(what)
        box.addView(bar)
        val cancelled = AtomicBoolean(false)
        val dlg = AlertDialog.Builder(this).setTitle("Импорт игры").setView(box).setCancelable(false)
            .setNegativeButton("Отмена") { _, _ -> cancelled.set(true) }.show()
        Thread {
            var error: String? = null
            try {
                GameFolder.import(contentResolver, tree, dst, { cancelled.get() }) { p ->
                    runOnUiThread { what.text = "${p.files} файлов, ${p.bytes / (1 shl 20)} МБ\n${p.current}" }
                }
            } catch (e: Exception) {
                error = e.message ?: e.toString()
            }
            runOnUiThread {
                dlg.dismiss()
                if (!cancelled.get()) {
                    s.gameDir = dst.absolutePath
                    refreshStatus()
                    toastDialog(error?.let { "Ошибка импорта: $it" } ?: (GameFolder.check(dst.absolutePath)
                        ?.let { "Скопировано, но: $it" } ?: "Готово: игра скопирована."))
                }
            }
        }.start()
    }

    // ---------------------------------------------------------------- графика
    private fun graphicsCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, "Графика и скорость"))
        val tier = Device.tier(this)
        val ram = "%.1f".format(Device.ramMb(this) / 1024.0)
        c.addView(Ui.label(this, "Телефон: $ram ГБ ОЗУ, ЦП до ${Device.maxFreqMhz()} МГц → «${tier.name}»",
            dim = true, size = 12f))
        val holder = Ui.vbox(this)
        fun fill() {
            holder.removeAllViews()
            val scales = listOf(0.5f, 0.6f, 0.75f, 0.85f, 1.0f)
            holder.addView(Ui.choiceRow(this, "Разрешение (доля экрана)", scales.map { "${(it * 100).toInt()} %" },
                scales.indexOfFirst { it >= s.renderScale - 0.01f }.coerceAtLeast(0)) { s.renderScale = scales[it] })
            val fps = listOf(30, 45, 60, 0)
            holder.addView(Ui.choiceRow(this, "Ограничение FPS", listOf("30", "45", "60", "Нет"),
                fps.indexOf(s.fpsCap).let { if (it < 0) 3 else it }) { s.fpsCap = fps[it] })
            holder.addView(Ui.switchRow(this, "16-битные текстуры", "Меньше памяти и быстрее на слабых GPU, чуть хуже цвета",
                s.tex16) { s.tex16 = it })
            holder.addView(Ui.switchRow(this, "Стабильная производительность",
                "Частоты ниже пика, зато без перегрева и падения FPS через 10 минут (если телефон умеет)", s.sustained) { s.sustained = it })
            holder.addView(Ui.switchRow(this, "Счётчик FPS", null, s.showFps) { s.showFps = it })
        }
        c.addView(Ui.choiceRow(this, "Пресет", listOf(Device.LOW.name, Device.MID.name, Device.HIGH.name),
            listOf(Device.LOW, Device.MID, Device.HIGH).indexOfFirst { it.scale == s.renderScale && it.fps == s.fpsCap }
                .let { if (it < 0) 1 else it }) {
            s.applyTier(listOf(Device.LOW, Device.MID, Device.HIGH)[it])
            fill()
        })
        fill()
        c.addView(holder)
        return c
    }

    // ---------------------------------------------------------------- управление
    private fun controlsCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, "Управление"))
        c.addView(Ui.button(this, "✎  Редактор раскладки") {
            startActivity(Intent(this, LayoutEditorActivity::class.java))
        })
        c.addView(Ui.label(this, "В игре — шестерёнка в левом верхнем углу. Палец на свободном месте экрана — камера.",
            dim = true, size = 12f).apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, 0) })
        c.addView(Ui.switchRow(this, "Экранное управление", null, s.touchEnabled) { s.touchEnabled = it })
        c.addView(Ui.switchRow(this, "Прятать при геймпаде", "Подключён геймпад — кнопки на экране не нужны",
            s.hideWithGamepad) { s.hideWithGamepad = it })
        c.addView(Ui.switchRow(this, "Вибрация кнопок", null, s.haptics) { s.haptics = it })
        c.addView(Ui.seekRow(this, "Чувствительность камеры", 38, ((s.lookSensitivity - 0.1f) * 10f).toInt(),
            { "%.1f".format(0.1f + it / 10f) }) { s.lookSensitivity = 0.1f + it / 10f })
        c.addView(Ui.seekRow(this, "Прозрачность кнопок", 18, ((s.opacity - 0.1f) * 20f).toInt(),
            { "${((0.1f + it / 20f) * 100).toInt()} %" }) { s.opacity = 0.1f + it / 20f })
        return c
    }

    // ---------------------------------------------------------------- дополнительно
    private fun advancedCard(): LinearLayout {
        val c = Ui.card(this)
        c.addView(Ui.title(this, "Дополнительно"))
        val caches = listOf(64, 96, 128, 256, 512, 1024)
        c.addView(Ui.choiceRow(this, "Кэш файлов игры в памяти, МБ", caches.map { it.toString() },
            caches.indexOf(s.fileCacheMb).let { if (it < 0) 3 else it }) { s.fileCacheMb = caches[it] })
        val row = Ui.hbox(this).apply { setPadding(0, Ui.dp(this@LauncherActivity, 6f), 0, 0) }
        row.addView(Ui.button(this, "Журнал") { showLog() }, wrapGap())
        row.addView(Ui.button(this, "Переменные SNX_*") { editEnv() }, wrapGap())
        c.addView(row)
        c.addView(Ui.button(this, "Очистить кэш шейдеров") {
            File(cacheDir, "shaders").deleteRecursively()
            toastDialog("Кэш шейдеров очищен: при следующем запуске они соберутся заново.")
        }.apply { layoutParams = wrapGap().apply { topMargin = Ui.dp(this@LauncherActivity, 8f) } })
        return c
    }

    private fun editEnv() {
        val e = EditText(this).apply {
            setText(s.extraEnv)
            hint = "SNX_NOVSYNC=1"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            minLines = 4
            setTextColor(Ui.TEXT)
        }
        AlertDialog.Builder(this).setTitle("Переменные окружения (ИМЯ=ЗНАЧЕНИЕ)").setView(e)
            .setPositiveButton("Сохранить") { _, _ -> s.extraEnv = e.text.toString() }
            .setNegativeButton("Отмена", null).show()
    }

    private fun showLog() {
        val f = Settings.logFile(this)
        val text = if (!f.exists()) "Журнала пока нет — запусти игру." else try {
            RandomAccessFile(f, "r").use { r ->
                val n = minOf(r.length(), 64L * 1024)
                r.seek(r.length() - n)
                val b = ByteArray(n.toInt())
                r.readFully(b)
                String(b)
            }
        } catch (e: Exception) { e.toString() }
        val tv = TextView(this).apply {
            this.text = text
            textSize = 11f
            setTextIsSelectable(true)
            typeface = android.graphics.Typeface.MONOSPACE
            val p = Ui.dp(this@LauncherActivity, 12f)
            setPadding(p, p, p, p)
        }
        val sv = ScrollView(this).apply { addView(tv); post { fullScroll(ScrollView.FOCUS_DOWN) } }
        AlertDialog.Builder(this).setTitle(f.absolutePath).setView(sv)
            .setPositiveButton("Скопировать") { _, _ ->
                (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("snx.log", text))
            }.setNegativeButton("Закрыть", null).show()
    }

    // ---------------------------------------------------------------- запуск
    private fun play() {
        val err = GameFolder.check(s.gameDir)
        if (err != null) {
            AlertDialog.Builder(this).setTitle("Игра не найдена").setMessage(err)
                .setPositiveButton("Всё равно запустить") { _, _ -> launch() }
                .setNegativeButton("Отмена", null).show()
            return
        }
        launch()
    }

    private fun launch() {
        stale = true
        startActivity(Intent(this, GameActivity::class.java).putExtra(GameActivity.EXTRA_ARGS, s.gameArgs()))
    }

    private fun toastDialog(msg: String) {
        AlertDialog.Builder(this).setMessage(msg).setPositiveButton("OK", null).show()
    }

    companion object {
        private const val REQ_IMPORT = 1
        private const val REQ_PATH = 2
        private const val REQ_PERM = 3
    }
}
