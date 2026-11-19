package com.shreknx.game

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView

// Редактор раскладки: поле с элементами (TouchControlsView в режиме правки) и панель свойств выбранного.
// Перетаскивание — переместить, двумя пальцами — размер; на панели — размер, прозрачность, видимость,
// «плавающий» стик, залипание кнопки. Используется в лаунчере (LayoutEditorActivity) и поверх игры (GameActivity).
class LayoutEditor(private val ctx: Context, private val settings: Settings, private val onClose: (saved: Boolean) -> Unit) {
    val field = TouchControlsView(ctx, settings, editMode = true)
    val root = FrameLayout(ctx)
    private val panel = Ui.vbox(ctx, 10)
    private val props = Ui.vbox(ctx)
    private var atTop = false

    init {
        root.addView(field, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        panel.background = Ui.rounded(0xE61C2414.toInt(), Ui.dp(ctx, 14f).toFloat(), Ui.dp(ctx, 1f), 0x558DC63F)
        val scroll = ScrollView(ctx).apply { addView(panel) }
        root.addView(scroll, panelParams())
        field.onSelect = { showProps(it) }

        val top = Ui.hbox(ctx)
        top.addView(Ui.title(ctx, "Раскладка", 16f), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        top.addView(Ui.button(ctx, "⇅") {
            atTop = !atTop
            scroll.layoutParams = panelParams()
        })
        panel.addView(top)
        panel.addView(Ui.label(ctx, "Тяни элемент — переместить, двумя пальцами — размер. Скрытые — пунктиром.",
            dim = true, size = 12f))
        panel.addView(Ui.seekRow(ctx, "Прозрачность всего", 18, ((settings.opacity - 0.1f) * 20f).toInt(),
            { "${((0.1f + it / 20f) * 100).toInt()} %" }) {
            settings.opacity = 0.1f + it / 20f
            field.invalidate()
        })
        panel.addView(props)
        val buttons = Ui.hbox(ctx).apply { setPadding(0, Ui.dp(ctx, 8f), 0, 0) }
        buttons.addView(Ui.button(ctx, "Сброс") {
            AlertDialog.Builder(ctx).setMessage("Вернуть раскладку по умолчанию?")
                .setPositiveButton("Да") { _, _ ->
                    field.controls = TouchLayout.defaults().map { it.copy() }.toMutableList()
                    field.select(null)
                }.setNegativeButton("Нет", null).show()
        }, gap())
        buttons.addView(Ui.button(ctx, "Отмена") { onClose(false) }, gap())
        buttons.addView(Ui.button(ctx, "Сохранить", primary = false) {
            TouchLayout.save(ctx, field.controls)
            onClose(true)
        })
        panel.addView(buttons)
        showProps(null)
    }

    private fun gap() = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        .apply { rightMargin = Ui.dp(ctx, 8f) }

    private fun panelParams() = FrameLayout.LayoutParams(Ui.dp(ctx, 330f), ViewGroup.LayoutParams.WRAP_CONTENT).apply {
        gravity = Gravity.CENTER_HORIZONTAL or (if (atTop) Gravity.TOP else Gravity.BOTTOM)
        val m = Ui.dp(ctx, 8f)
        setMargins(m, m, m, m)
    }

    private fun nameOf(c: Control): String = when {
        c.kind == Kind.STICK -> if (c.rightStick) "Правый стик (камера)" else "Левый стик (ходьба)"
        c.kind == Kind.DPAD -> "Крестовина"
        c.label.isNotEmpty() -> "Кнопка «${c.label}»"
        else -> c.id
    }

    private fun showProps(c: Control?) {
        props.removeAllViews()
        if (c == null) {
            props.addView(Ui.label(ctx, "Выбери элемент на экране.", dim = true))
            props.addView(Ui.button(ctx, "Показать скрытые элементы…") { pickHidden() })
            return
        }
        props.addView(Ui.title(ctx, nameOf(c), 15f))
        props.addView(Ui.switchRow(ctx, "Показывать", null, c.visible) { c.visible = it; field.invalidate() })
        props.addView(Ui.seekRow(ctx, "Размер", 49, ((c.size - TouchLayout.MIN_SIZE) / (TouchLayout.MAX_SIZE - TouchLayout.MIN_SIZE) * 49).toInt(),
            { "${(TouchLayout.MIN_SIZE * 100 + it).toInt()}" }) {
            c.size = TouchLayout.MIN_SIZE + (TouchLayout.MAX_SIZE - TouchLayout.MIN_SIZE) * it / 49f
            field.invalidate()
        })
        props.addView(Ui.seekRow(ctx, "Непрозрачность", 9, ((c.opacity - 0.1f) * 10f).toInt(),
            { "${((0.1f + it / 10f) * 100).toInt()} %" }) {
            c.opacity = 0.1f + it / 10f
            field.invalidate()
        })
        if (c.kind == Kind.STICK)
            props.addView(Ui.switchRow(ctx, "Плавающий", "Появляется там, где коснулся пальцем (в своей половине экрана)",
                c.floating) { c.floating = it; field.invalidate() })
        if (c.kind == Kind.BUTTON)
            props.addView(Ui.switchRow(ctx, "Залипание", "Нажал — держится до следующего нажатия (бег, блок)",
                c.toggle) { c.toggle = it })
    }

    private fun pickHidden() {
        val hidden = field.controls.filter { !it.visible }
        if (hidden.isEmpty()) return
        AlertDialog.Builder(ctx).setTitle("Скрытые элементы")
            .setItems(hidden.map { nameOf(it) }.toTypedArray()) { _, i ->
                hidden[i].visible = true
                field.select(hidden[i])
            }.show()
    }
}

// Редактор из лаунчера: на весь экран, над тёмной сеткой
class LayoutEditorActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        @Suppress("DEPRECATION")
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_LAYOUT_STABLE)
        val ed = LayoutEditor(this, Settings(this)) { saved ->
            if (saved) setResult(RESULT_OK)
            finish()
        }
        setContentView(ed.root)
    }
}
