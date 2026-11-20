package com.shreknx.game

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

// Экранное управление поверх игры (и редактор раскладки). Один полноэкранный View разбирает все пальцы сам:
// элементы (кнопки, стики, крестовина) -> виртуальный геймпад (SnxNative.pad), палец на свободном месте — камера
// (SnxNative.look). Кнопки «скользят»: палец, переехавший с кнопки на соседнюю, отпускает первую и жмёт вторую.
// В меню игры (SnxNative.state) элементы прячутся, а касания проходят к SDL под этим View — там их разбирает меню.
// Рисуется только при изменениях (без постоянной перерисовки — не тратит GPU и батарею).
class TouchControlsView(ctx: Context, private val settings: Settings, val editMode: Boolean) : View(ctx) {
    var controls: MutableList<Control> = TouchLayout.load(ctx)
        set(v) { field = v; releaseAll(); invalidate() }

    // игра: состояние меню и подключённый геймпад
    var hiddenByMenu = false
        set(v) { if (field != v) { field = v; releaseAll(); invalidate() } }
    var hiddenByGamepad = false
        set(v) { if (field != v) { field = v; releaseAll(); invalidate() } }
    var onGear: (() -> Unit)? = null
    var onSelect: ((Control?) -> Unit)? = null  // редактор: выбранный элемент
    var selected: Control? = null
        private set

    private val density = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2.5f * density }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }
    private val dashed = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
        pathEffect = DashPathEffect(floatArrayOf(8f * density, 6f * density), 0f)
    }
    private val grid = Paint().apply { strokeWidth = 1f; color = 0x22FFFFFF }
    private val path = Path()
    private val rect = RectF()

    // ---------------------------------------------------------------- состояние пальцев (игра)
    private enum class Bind { NONE, CONTROL, LOOK, GEAR }
    private class Finger(var bind: Bind = Bind.NONE, var ctl: Control? = null, var lastX: Float = 0f, var lastY: Float = 0f)
    private val fingers = HashMap<Int, Finger>()

    private class StickState(var pointer: Int = -1, var cx: Float = 0f, var cy: Float = 0f, var vx: Float = 0f, var vy: Float = 0f)
    private val sticks = HashMap<String, StickState>()
    private val latched = HashSet<String>()      // залипшие кнопки (toggle)
    private var dpadMask = 0                      // крестовина: биты DPAD_*

    private var sentButtons = -1
    private var sentAxes = FloatArray(4) { Float.NaN }
    private val heldVk = HashSet<Int>()

    private fun w() = width.toFloat()
    private fun h() = height.toFloat()
    private fun cx(c: Control) = c.x * w()
    private fun cy(c: Control) = c.y * h()
    private fun radius(c: Control) = c.size * h() / 2f
    private fun gearRadius() = 18f * density
    private fun gearX() = 30f * density
    private fun gearY() = 30f * density

    private fun active(): Boolean = editMode || (!hiddenByMenu && !hiddenByGamepad && settings.touchEnabled)

    // ---------------------------------------------------------------- рисование
    override fun onDraw(canvas: Canvas) {
        if (editMode) drawGrid(canvas)
        val base = settings.opacity
        if (active()) {
            for (c in controls) {
                if (!c.visible && !editMode) continue
                val a = if (c.visible) base * c.opacity else 0.25f
                when (c.kind) {
                    Kind.BUTTON -> drawButton(canvas, c, a)
                    Kind.STICK -> drawStick(canvas, c, a)
                    Kind.DPAD -> drawDpad(canvas, c, a)
                }
                if (editMode && c === selected) {
                    ring.color = 0xFFFFD54F.toInt()
                    canvas.drawCircle(cx(c), cy(c), radius(c) + 6f * density, dashed.apply { color = 0xFFFFD54F.toInt() })
                }
                if (editMode && !c.visible) {
                    dashed.color = 0x99FFFFFF.toInt()
                    canvas.drawCircle(cx(c), cy(c), radius(c), dashed)
                }
            }
        }
        if (!editMode) drawGear(canvas)
    }

    private fun drawGrid(canvas: Canvas) {
        canvas.drawColor(0xFF1A2212.toInt())
        var i = 1
        while (i < 20) {
            val x = w() * i / 20f
            val y = h() * i / 20f
            canvas.drawLine(x, 0f, x, h(), grid)
            canvas.drawLine(0f, y, w(), y, grid)
            i++
        }
    }

    private fun alpha(a: Float) = (a.coerceIn(0f, 1f) * 255).toInt()

    private fun drawButton(canvas: Canvas, c: Control, a: Float) {
        val pressed = isPressed(c)
        val x = cx(c); val y = cy(c); val r = radius(c)
        fill.color = if (pressed) 0x8DC63F else 0x101010
        fill.alpha = alpha(if (pressed) a * 0.9f else a * 0.55f)
        canvas.drawCircle(x, y, r, fill)
        ring.color = 0xFFFFFF
        ring.alpha = alpha(a)
        canvas.drawCircle(x, y, r - ring.strokeWidth / 2, ring)
        if (c.toggle && c.id in latched) canvas.drawCircle(x, y, r * 0.8f, ring)
        drawLabel(canvas, c.label, x, y, r, a)
    }

    private fun drawLabel(canvas: Canvas, s: String, x: Float, y: Float, r: Float, a: Float) {
        if (s.isEmpty()) return
        text.color = 0xFFFFFF
        text.alpha = alpha(min(1f, a * 1.3f))
        text.textSize = if (s.length <= 2) r * 0.8f else r * 0.42f
        // длинная надпись — ужать до ширины кнопки
        val maxW = r * 1.7f
        val tw = text.measureText(s)
        if (tw > maxW) text.textSize *= maxW / tw
        canvas.drawText(s, x, y - (text.descent() + text.ascent()) / 2, text)
    }

    private fun drawStick(canvas: Canvas, c: Control, a: Float) {
        val st = sticks[c.id]
        val activeNow = st != null && st.pointer >= 0
        if (c.floating && !activeNow && !editMode && a < 0.3f) return
        val bx = if (activeNow) st.cx else cx(c)
        val by = if (activeNow) st.cy else cy(c)
        val r = radius(c)
        fill.color = 0x101010
        fill.alpha = alpha(a * 0.4f)
        canvas.drawCircle(bx, by, r, fill)
        ring.color = 0xFFFFFF
        ring.alpha = alpha(a * 0.8f)
        canvas.drawCircle(bx, by, r - ring.strokeWidth / 2, ring)
        val kx = bx + (st?.vx ?: 0f) * r * 0.6f
        val ky = by + (st?.vy ?: 0f) * r * 0.6f
        fill.color = if (activeNow) 0x8DC63F else 0xDDDDDD
        fill.alpha = alpha(a * 0.85f)
        canvas.drawCircle(kx, ky, r * 0.38f, fill)
    }

    private fun drawDpad(canvas: Canvas, c: Control, a: Float) {
        val x = cx(c); val y = cy(c); val r = radius(c); val t = r * 0.36f
        val dirs = intArrayOf(SnxNative.DPAD_UP, SnxNative.DPAD_RIGHT, SnxNative.DPAD_DOWN, SnxNative.DPAD_LEFT)
        for ((i, bit) in dirs.withIndex()) {
            canvas.save()
            canvas.rotate(90f * i, x, y)
            rect.set(x - t, y - r, x + t, y - t * 0.6f)
            val on = dpadMask and (1 shl bit) != 0
            fill.color = if (on) 0x8DC63F else 0x101010
            fill.alpha = alpha(if (on) a * 0.9f else a * 0.55f)
            canvas.drawRoundRect(rect, t * 0.4f, t * 0.4f, fill)
            ring.color = 0xFFFFFF
            ring.alpha = alpha(a)
            canvas.drawRoundRect(rect, t * 0.4f, t * 0.4f, ring)
            path.reset()
            path.moveTo(x, y - r * 0.86f)
            path.lineTo(x - t * 0.45f, y - r * 0.62f)
            path.lineTo(x + t * 0.45f, y - r * 0.62f)
            path.close()
            fill.color = 0xFFFFFF
            fill.alpha = alpha(a)
            canvas.drawPath(path, fill)
            canvas.restore()
        }
    }

    private fun drawGear(canvas: Canvas) {
        // кнопка редактора/настроек управления: всегда видна (бледно), даже в меню игры
        val x = gearX(); val y = gearY(); val r = gearRadius()
        fill.color = 0x000000
        fill.alpha = 90
        canvas.drawCircle(x, y, r, fill)
        ring.color = 0xFFFFFF
        ring.alpha = 140
        canvas.drawCircle(x, y, r * 0.45f, ring)
        for (i in 0 until 8) {
            canvas.save()
            canvas.rotate(45f * i, x, y)
            canvas.drawLine(x, y - r * 0.55f, x, y - r * 0.8f, ring)
            canvas.restore()
        }
    }

    private fun isPressed(c: Control): Boolean {
        if (c.toggle && c.id in latched) return true
        return fingers.values.any { it.bind == Bind.CONTROL && it.ctl === c }
    }

    // ---------------------------------------------------------------- касания
    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (editMode) return editTouch(e)
        val idx = e.actionIndex
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // меню или скрытое управление: касание — не наше (его получит SDL), кроме шестерёнки
                val x = e.getX(idx); val y = e.getY(idx)
                if (!onGearAt(x, y) && !active()) return false
                down(e.getPointerId(idx), x, y)
            }
            MotionEvent.ACTION_POINTER_DOWN -> down(e.getPointerId(idx), e.getX(idx), e.getY(idx))
            MotionEvent.ACTION_MOVE -> for (i in 0 until e.pointerCount) move(e.getPointerId(i), e.getX(i), e.getY(i))
            MotionEvent.ACTION_POINTER_UP -> up(e.getPointerId(idx), e.getX(idx), e.getY(idx))
            MotionEvent.ACTION_UP -> up(e.getPointerId(idx), e.getX(idx), e.getY(idx))
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        send()
        return true
    }

    private fun onGearAt(x: Float, y: Float) = hypot(x - gearX(), y - gearY()) < gearRadius() * 1.6f

    private fun hit(x: Float, y: Float, kinds: Set<Kind>): Control? {
        var best: Control? = null
        var bestD = Float.MAX_VALUE
        for (c in controls) {
            if (!c.visible || c.kind !in kinds) continue
            if (c.kind == Kind.STICK && c.floating) continue
            val d = hypot(x - cx(c), y - cy(c))
            val reach = radius(c) * (if (c.kind == Kind.BUTTON) 1.12f else 1.25f)
            if (d < reach && d / reach < bestD) { best = c; bestD = d / reach }
        }
        return best
    }

    // плавающий стик: левая (или правая — для правого стика) часть экрана, где нет других элементов
    private fun floatingStickAt(x: Float): Control? {
        val left = x < w() * 0.42f
        return controls.firstOrNull {
            it.visible && it.kind == Kind.STICK && it.floating && it.rightStick != left &&
                (sticks[it.id]?.pointer ?: -1) < 0
        }
    }

    private fun down(id: Int, x: Float, y: Float) {
        val f = Finger(lastX = x, lastY = y)
        fingers[id] = f
        if (onGearAt(x, y)) { f.bind = Bind.GEAR; return }
        if (!active()) return
        val c = hit(x, y, ALL_KINDS)
        if (c != null) {
            f.bind = Bind.CONTROL
            f.ctl = c
            when (c.kind) {
                Kind.STICK -> startStick(c, id, cx(c), cy(c), x, y)
                Kind.DPAD -> updateDpad(c, x, y)
                Kind.BUTTON -> {
                    if (c.toggle) { if (!latched.remove(c.id)) latched.add(c.id) }
                    buzz()
                }
            }
        } else {
            val fs = floatingStickAt(x)
            if (fs != null) {
                f.bind = Bind.CONTROL
                f.ctl = fs
                startStick(fs, id, x, y, x, y)
            } else f.bind = Bind.LOOK
        }
        invalidate()
    }

    private fun move(id: Int, x: Float, y: Float) {
        val f = fingers[id] ?: return
        when (f.bind) {
            Bind.LOOK -> {
                val k = settings.lookSensitivity * 1000f / max(1f, h())
                SnxNative.look((x - f.lastX) * k, (y - f.lastY) * k)
            }
            Bind.CONTROL -> {
                val c = f.ctl!!
                when (c.kind) {
                    Kind.STICK -> moveStick(c, x, y)
                    Kind.DPAD -> updateDpad(c, x, y)
                    Kind.BUTTON -> if (!c.toggle) {
                        // скольжение на соседнюю кнопку
                        val n = hit(x, y, BUTTONS)
                        if (n != null && n !== c && !n.toggle) {
                            f.ctl = n
                            buzz()
                            invalidate()
                        }
                    }
                }
            }
            else -> {}
        }
        f.lastX = x
        f.lastY = y
    }

    private fun up(id: Int, x: Float, y: Float) {
        val f = fingers.remove(id) ?: return
        if (f.bind == Bind.GEAR && onGearAt(x, y)) onGear?.invoke()
        val c = f.ctl
        if (c != null) {
            if (c.kind == Kind.STICK) sticks[c.id]?.let { if (it.pointer == id) { it.pointer = -1; it.vx = 0f; it.vy = 0f } }
            if (c.kind == Kind.DPAD && fingers.values.none { it.ctl === c }) dpadMask = 0
        }
        invalidate()
    }

    private fun startStick(c: Control, id: Int, ox: Float, oy: Float, x: Float, y: Float) {
        val st = sticks.getOrPut(c.id) { StickState() }
        st.pointer = id
        st.cx = ox
        st.cy = oy
        moveStick(c, x, y)
    }

    private fun moveStick(c: Control, x: Float, y: Float) {
        val st = sticks[c.id] ?: return
        val r = radius(c) * 0.6f
        var dx = (x - st.cx) / r
        var dy = (y - st.cy) / r
        val m = hypot(dx, dy)
        if (m > 1f) {
            // центр стоит на месте (и у плавающего — там, где коснулись): палец за краем — просто полный наклон
            dx /= m; dy /= m
        }
        // Прилипание к осям: палец на стекле не ведёт ровно вбок, и при «влево» шла ещё и составляющая
        // «вперёд/назад» — на карнизе Шрек вместо перебирания руками лез вверх или срывался, при ходьбе вбок —
        // уходил по диагонали. В пределах SNAP_DEG от оси — строго по оси (с той же силой); между — диагональ.
        if (dx != 0f || dy != 0f) {
            val ang = Math.toDegrees(atan2(abs(dy).toDouble(), abs(dx).toDouble()))  // 0 — вбок, 90 — вверх/вниз
            val mag = min(1f, hypot(dx, dy))
            if (ang < SNAP_DEG) { dx = if (dx < 0) -mag else mag; dy = 0f }
            else if (ang > 90 - SNAP_DEG) { dy = if (dy < 0) -mag else mag; dx = 0f }
        }
        st.vx = dx
        st.vy = dy
        invalidate()
    }

    private fun updateDpad(c: Control, x: Float, y: Float) {
        val dx = x - cx(c); val dy = y - cy(c)
        var mask = 0
        if (hypot(dx, dy) > radius(c) * 0.2f) {
            // 8 направлений: сектора по 45°
            val ang = Math.toDegrees(atan2(dy.toDouble(), dx.toDouble()))  // 0 — вправо, 90 — вниз
            val sector = (((ang + 360 + 22.5) % 360) / 45).toInt()
            mask = when (sector) {
                0 -> 1 shl SnxNative.DPAD_RIGHT
                1 -> (1 shl SnxNative.DPAD_RIGHT) or (1 shl SnxNative.DPAD_DOWN)
                2 -> 1 shl SnxNative.DPAD_DOWN
                3 -> (1 shl SnxNative.DPAD_DOWN) or (1 shl SnxNative.DPAD_LEFT)
                4 -> 1 shl SnxNative.DPAD_LEFT
                5 -> (1 shl SnxNative.DPAD_LEFT) or (1 shl SnxNative.DPAD_UP)
                6 -> 1 shl SnxNative.DPAD_UP
                else -> (1 shl SnxNative.DPAD_UP) or (1 shl SnxNative.DPAD_RIGHT)
            }
        }
        if (mask != dpadMask) {
            if (mask != 0 && (mask and dpadMask.inv()) != 0) buzz()
            dpadMask = mask
            invalidate()
        }
    }

    private fun buzz() {
        if (settings.haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
    }

    fun releaseAll() {
        fingers.clear()
        sticks.values.forEach { it.pointer = -1; it.vx = 0f; it.vy = 0f }
        dpadMask = 0
        if (!editMode) {
            latched.clear()
            send()
        }
    }

    // состояние -> игра (только при изменении)
    private fun send() {
        if (editMode) return
        var buttons = dpadMask
        val vks = HashSet<Int>()
        fun press(c: Control) {
            if (c.button >= 0) buttons = buttons or (1 shl c.button) else if (c.vk != 0) vks.add(c.vk)
        }
        for (f in fingers.values) {
            val c = f.ctl ?: continue
            if (f.bind == Bind.CONTROL && c.kind == Kind.BUTTON && !c.toggle) press(c)
        }
        for (id in latched) controls.firstOrNull { it.id == id }?.let { press(it) }
        val ax = FloatArray(4)
        for (c in controls) {
            if (c.kind != Kind.STICK) continue
            val st = sticks[c.id] ?: continue
            val o = if (c.rightStick) 2 else 0
            if (abs(st.vx) > abs(ax[o])) ax[o] = st.vx
            if (abs(st.vy) > abs(ax[o + 1])) ax[o + 1] = st.vy
        }
        if (buttons != sentButtons || !ax.contentEquals(sentAxes)) {
            try {
                SnxNative.pad(buttons, ax[0], ax[1], ax[2], ax[3])
            } catch (e: UnsatisfiedLinkError) {
                return
            }
            sentButtons = buttons
            sentAxes = ax
        }
        for (vk in vks) if (heldVk.add(vk)) SnxNative.key(vk, true)
        val it = heldVk.iterator()
        while (it.hasNext()) {
            val vk = it.next()
            if (vk !in vks) { SnxNative.key(vk, false); it.remove() }
        }
    }

    // ---------------------------------------------------------------- редактор
    private var dragId = -1
    private var dragDx = 0f
    private var dragDy = 0f
    private var pinchStart = 0f
    private var sizeStart = 0f

    private fun editHit(x: Float, y: Float): Control? {
        // в редакторе — и скрытые, и плавающие (за их место по умолчанию)
        var best: Control? = null
        var bestD = Float.MAX_VALUE
        for (c in controls) {
            val d = hypot(x - cx(c), y - cy(c))
            if (d < radius(c) * 1.1f && d / radius(c) < bestD) { best = c; bestD = d / radius(c) }
        }
        return best
    }

    fun select(c: Control?) {
        selected = c
        onSelect?.invoke(c)
        invalidate()
    }

    private fun editTouch(e: MotionEvent): Boolean {
        val idx = e.actionIndex
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val x = e.getX(idx); val y = e.getY(idx)
                val c = editHit(x, y)
                select(c)
                if (c != null) {
                    dragId = e.getPointerId(idx)
                    dragDx = cx(c) - x
                    dragDy = cy(c) - y
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> if (e.pointerCount == 2 && selected != null) {
                pinchStart = hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1))
                sizeStart = selected!!.size
                dragId = -1  // двумя пальцами — только размер
            }
            MotionEvent.ACTION_MOVE -> {
                val c = selected ?: return true
                if (e.pointerCount >= 2 && pinchStart > 0f) {
                    val d = hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1))
                    c.size = (sizeStart * d / pinchStart).coerceIn(TouchLayout.MIN_SIZE, TouchLayout.MAX_SIZE)
                    onSelect?.invoke(c)
                } else if (dragId >= 0) {
                    val i = e.findPointerIndex(dragId)
                    if (i >= 0) {
                        // привязка к сетке 0.5 %: ровные ряды кнопок
                        c.x = snap(((e.getX(i) + dragDx) / w()).coerceIn(0.02f, 0.98f))
                        c.y = snap(((e.getY(i) + dragDy) / h()).coerceIn(0.03f, 0.97f))
                    }
                }
                invalidate()
            }
            MotionEvent.ACTION_POINTER_UP -> if (e.pointerCount <= 2) pinchStart = 0f
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { dragId = -1; pinchStart = 0f }
        }
        return true
    }

    private fun snap(v: Float) = Math.round(v * 200f) / 200f

    companion object {
        private val ALL_KINDS = setOf(Kind.BUTTON, Kind.STICK, Kind.DPAD)
        private val BUTTONS = setOf(Kind.BUTTON)
        private const val SNAP_DEG = 25.0  // стик: ближе к оси — строго по оси
    }
}
