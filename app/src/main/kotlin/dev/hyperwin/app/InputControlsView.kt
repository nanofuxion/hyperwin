package dev.hyperwin.app

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.min
import kotlin.math.round

/**
 * Full-screen translucent touch controls overlay with Winlator-style rendering, full multitouch
 * support (gamepad XInput + keyboard + mouse bindings), and built-in snap-to-grid edit mode.
 */
class InputControlsView(context: Context) : View(context) {
    companion object {
        private const val STICK_DEAD_ZONE = 0.15f
        private const val DPAD_DEAD_ZONE = 0.28f
        private const val TICK_MS = 16L
        private const val COLOR_NORMAL = 0xFFFFFFFF.toInt()
        private const val COLOR_ACCENT = 0xFF4FC3F7.toInt()
        private const val COLOR_TOGGLE = 0xFF81C784.toInt()
    }

    var profile: ControlsProfile? = null
        set(value) {
            releaseAll()
            field = value
            selectedElement = null
            GamepadInput.setTouchActive(value != null && value.usesGamepad() && !editMode && visibility == VISIBLE)
            onSelectionChanged?.invoke(null)
            invalidate()
        }

    var editMode: Boolean = false
        set(value) {
            if (field == value) return
            releaseAll()
            field = value
            if (!value) selectedElement = null
            GamepadInput.setTouchActive(!value && profile?.usesGamepad() == true && visibility == VISIBLE)
            onSelectionChanged?.invoke(selectedElement)
            invalidate()
        }

    var overlayOpacity: Float = 0.65f
        set(value) {
            field = value.coerceIn(0.15f, 1.0f)
            invalidate()
        }

    var blockOutsideTouch: Boolean = false

    var selectedElement: ControlElement? = null
        private set

    var onSelectionChanged: ((ControlElement?) -> Unit)? = null
    var onProfileChanged: (() -> Unit)? = null

    // ---- runtime state per element / pointer ----

    private data class ElementRuntime(
        var pressed: Boolean = false,
        var toggled: Boolean = false,
        var knobX: Float = 0f,      // -1..1 (right positive)
        var knobY: Float = 0f,      // -1..1 (down positive in screen space)
        var dpadMask: Int = 0,      // bits 0..3 for Up, Right, Down, Left
        var rangeScroll: Float = 0f,
        var rangePressedIndex: Int = -1,
    )

    private data class PointerTrack(
        val element: ControlElement,
        var lastX: Float,
        var lastY: Float,
        var startX: Float,
        var startY: Float,
        var moved: Boolean = false,
    )

    private val runtime = LinkedHashMap<ControlElement, ElementRuntime>()
    private val pointers = HashMap<Int, PointerTrack>()
    private val heldKeys = HashMap<Int, Int>()
    private val heldMouseButtons = HashMap<Int, Int>()
    private val heldGamepadButtons = HashMap<Int, Int>()
    private var heldLtCount = 0
    private var heldRtCount = 0
    private var editDragPointerId = -1
    private var editDragOffsetX = 0f
    private var editDragOffsetY = 0f

    private var mouseFracX = 0f
    private var mouseFracY = 0f
    private val ui = Handler(Looper.getMainLooper())
    private var ticking = false

    private val tickRunnable = object : Runnable {
        override fun run() {
            val active = tickContinuousInputs()
            if (active && !editMode && visibility == VISIBLE) {
                ui.postDelayed(this, TICK_MS)
            } else {
                ticking = false
            }
        }
    }

    // ---- paints ----

    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
    }
    private val gridPaint = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 1f
        color = 0x33FFFFFF
    }
    private val tempRect = RectF()
    private val tempPath = Path()

    private fun rt(el: ControlElement): ElementRuntime = runtime.getOrPut(el) { ElementRuntime() }

    /** Winlator-style grid unit based on viewport height. */
    fun snappingSize(): Float = (min(width, height).coerceAtLeast(360) / 22f).coerceIn(16f, 64f)

    override fun setVisibility(visibility: Int) {
        val wasVisible = this.visibility == VISIBLE
        super.setVisibility(visibility)
        if (wasVisible && visibility != VISIBLE) releaseAll()
        GamepadInput.setTouchActive(visibility == VISIBLE && !editMode && profile?.usesGamepad() == true)
    }

    override fun onDetachedFromWindow() {
        releaseAll()
        super.onDetachedFromWindow()
    }

    // ---- Edit-mode helpers ----

    fun addElement(type: ElementType = ElementType.BUTTON): ControlElement? {
        val p = profile ?: return null
        val el = ControlElement(
            type = type,
            shape = ElementShape.CIRCLE,
            x = 0.5f,
            y = 0.5f,
        )
        p.elements.add(el)
        selectElement(el)
        onProfileChanged?.invoke()
        invalidate()
        return el
    }

    fun removeSelectedElement(): Boolean {
        val p = profile ?: return false
        val el = selectedElement ?: return false
        p.elements.remove(el)
        runtime.remove(el)
        selectElement(null)
        onProfileChanged?.invoke()
        invalidate()
        return true
    }

    fun selectElement(el: ControlElement?) {
        selectedElement = el
        onSelectionChanged?.invoke(el)
        invalidate()
    }

    fun notifyElementEdited() {
        onProfileChanged?.invoke()
        invalidate()
    }

    // ---- Geometry ----

    private fun halfSize(el: ControlElement): Pair<Float, Float> {
        val u = snappingSize()
        val (hw, hh) = when (el.type) {
            ElementType.BUTTON -> when (el.shape) {
                ElementShape.CIRCLE -> (u * 2.5f) to (u * 2.5f)
                ElementShape.SQUARE -> (u * 2.5f) to (u * 2.5f)
                ElementShape.RECT -> (u * 3.8f) to (u * 2.0f)
                ElementShape.ROUND_RECT -> (u * 3.8f) to (u * 2.0f)
            }
            ElementType.D_PAD -> (u * 5.2f) to (u * 5.2f)
            ElementType.STICK -> (u * 5.6f) to (u * 5.6f)
            ElementType.TRACKPAD -> (u * 6.0f) to (u * 6.0f)
            ElementType.RANGE_BUTTON -> {
                val n = el.visibleCount.coerceIn(3, el.range.bindings.size)
                val longHalf = u * 1.9f * n
                val shortHalf = u * 1.8f
                if (el.orientation == 1) shortHalf to longHalf else longHalf to shortHalf
            }
        }
        return (hw * el.scale) to (hh * el.scale)
    }

    private fun boundsOf(el: ControlElement, out: RectF = RectF()): RectF {
        val cx = el.x * width
        val cy = el.y * height
        val (hw, hh) = halfSize(el)
        out.set(cx - hw, cy - hh, cx + hw, cy + hh)
        return out
    }

    private fun hitTest(px: Float, py: Float): ControlElement? {
        val els = profile?.elements ?: return null
        val r = RectF()
        for (i in els.indices.reversed()) {
            val el = els[i]
            boundsOf(el, r)
            val pad = snappingSize() * 0.35f
            if (el.type == ElementType.STICK || el.type == ElementType.TRACKPAD ||
                (el.type == ElementType.BUTTON && el.shape == ElementShape.CIRCLE)
            ) {
                val radius = r.width() / 2f + pad
                if (hypot(px - r.centerX(), py - r.centerY()) <= radius) return el
            } else {
                if (px >= r.left - pad && px <= r.right + pad && py >= r.top - pad && py <= r.bottom + pad) return el
            }
        }
        return null
    }

    // ---- Drawing ----

    override fun onDraw(canvas: Canvas) {
        val p = profile ?: return
        if (width <= 0 || height <= 0) return

        if (editMode) drawGrid(canvas)

        val u = snappingSize()
        for (el in p.elements) {
            val state = rt(el)
            val selected = editMode && el === selectedElement
            val alpha = ((overlayOpacity * el.opacity).coerceIn(0.08f, 1.0f) * 255).toInt()
            val primaryColor = when {
                selected -> COLOR_ACCENT
                state.toggled -> COLOR_TOGGLE
                state.pressed -> COLOR_ACCENT
                else -> COLOR_NORMAL
            }
            boundsOf(el, tempRect)
            strokePaint.strokeWidth = (u * 0.16f).coerceIn(2f, 5f)
            strokePaint.color = withAlpha(primaryColor, alpha)
            fillPaint.color = withAlpha(
                if (state.pressed || state.toggled || selected) primaryColor else Color.BLACK,
                (alpha * (if (state.pressed || state.toggled || selected) 0.30f else 0.34f)).toInt(),
            )
            textPaint.color = withAlpha(primaryColor, (alpha * 1.15f).toInt().coerceAtMost(255))

            when (el.type) {
                ElementType.BUTTON -> drawButton(canvas, el, tempRect, u)
                ElementType.D_PAD -> drawDpad(canvas, el, state, tempRect, alpha, u)
                ElementType.STICK -> drawStick(canvas, el, state, tempRect, primaryColor, alpha)
                ElementType.TRACKPAD -> drawTrackpad(canvas, tempRect, primaryColor, alpha, u)
                ElementType.RANGE_BUTTON -> drawRangeButton(canvas, el, state, tempRect, alpha, u)
            }
        }
    }

    private fun drawGrid(canvas: Canvas) {
        val step = snappingSize()
        var x = step
        while (x < width) {
            canvas.drawLine(x, 0f, x, height.toFloat(), gridPaint)
            x += step
        }
        var y = step
        while (y < height) {
            canvas.drawLine(0f, y, width.toFloat(), y, gridPaint)
            y += step
        }
    }

    private fun drawButton(canvas: Canvas, el: ControlElement, r: RectF, u: Float) {
        when (el.shape) {
            ElementShape.CIRCLE -> {
                val rad = min(r.width(), r.height()) / 2f
                canvas.drawCircle(r.centerX(), r.centerY(), rad, fillPaint)
                canvas.drawCircle(r.centerX(), r.centerY(), rad, strokePaint)
            }
            ElementShape.RECT, ElementShape.SQUARE -> {
                val rad = u * 0.35f * el.scale
                canvas.drawRoundRect(r, rad, rad, fillPaint)
                canvas.drawRoundRect(r, rad, rad, strokePaint)
            }
            ElementShape.ROUND_RECT -> {
                val rad = min(r.width(), r.height()) * 0.45f
                canvas.drawRoundRect(r, rad, rad, fillPaint)
                canvas.drawRoundRect(r, rad, rad, strokePaint)
            }
        }
        val label = el.displayText()
        if (label.isNotEmpty()) {
            val maxW = r.width() * 0.78f
            var ts = (min(r.height() * 0.42f, u * 1.7f * el.scale)).coerceAtLeast(18f)
            textPaint.textSize = ts
            val measured = textPaint.measureText(label)
            if (measured > maxW && measured > 0f) {
                ts *= maxW / measured
                textPaint.textSize = ts
            }
            val fm = textPaint.fontMetrics
            val baseline = r.centerY() - (fm.ascent + fm.descent) / 2f
            canvas.drawText(label, r.centerX(), baseline, textPaint)
        }
    }

    private fun drawDpad(canvas: Canvas, el: ControlElement, state: ElementRuntime, r: RectF, alpha: Int, u: Float) {
        val cx = r.centerX()
        val cy = r.centerY()
        val arm = r.width() * 0.5f
        val halfThick = r.width() * 0.165f
        val cr = u * 0.25f * el.scale

        // Cross outline
        tempPath.reset()
        tempPath.moveTo(cx - halfThick, cy - arm)
        tempPath.lineTo(cx + halfThick, cy - arm)
        tempPath.lineTo(cx + halfThick, cy - halfThick)
        tempPath.lineTo(cx + arm, cy - halfThick)
        tempPath.lineTo(cx + arm, cy + halfThick)
        tempPath.lineTo(cx + halfThick, cy + halfThick)
        tempPath.lineTo(cx + halfThick, cy + arm)
        tempPath.lineTo(cx - halfThick, cy + arm)
        tempPath.lineTo(cx - halfThick, cy + halfThick)
        tempPath.lineTo(cx - arm, cy + halfThick)
        tempPath.lineTo(cx - arm, cy - halfThick)
        tempPath.lineTo(cx - halfThick, cy - halfThick)
        tempPath.close()

        canvas.drawPath(tempPath, fillPaint)
        canvas.drawPath(tempPath, strokePaint)

        // Highlight active directions
        val hiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.FILL
            color = withAlpha(COLOR_ACCENT, (alpha * 0.55f).toInt())
        }
        val armRect = RectF()
        if (state.dpadMask and 1 != 0) { // Up
            armRect.set(cx - halfThick, cy - arm, cx + halfThick, cy - halfThick)
            canvas.drawRoundRect(armRect, cr, cr, hiPaint)
        }
        if (state.dpadMask and 2 != 0) { // Right
            armRect.set(cx + halfThick, cy - halfThick, cx + arm, cy + halfThick)
            canvas.drawRoundRect(armRect, cr, cr, hiPaint)
        }
        if (state.dpadMask and 4 != 0) { // Down
            armRect.set(cx - halfThick, cy + halfThick, cx + halfThick, cy + arm)
            canvas.drawRoundRect(armRect, cr, cr, hiPaint)
        }
        if (state.dpadMask and 8 != 0) { // Left
            armRect.set(cx - arm, cy - halfThick, cx - halfThick, cy + halfThick)
            canvas.drawRoundRect(armRect, cr, cr, hiPaint)
        }
    }

    private fun drawStick(
        canvas: Canvas,
        el: ControlElement,
        state: ElementRuntime,
        r: RectF,
        primaryColor: Int,
        alpha: Int,
    ) {
        val cx = r.centerX()
        val cy = r.centerY()
        val outerR = min(r.width(), r.height()) / 2f
        val knobR = outerR * 0.44f
        val maxTravel = outerR - knobR * 0.72f

        canvas.drawCircle(cx, cy, outerR, fillPaint)
        canvas.drawCircle(cx, cy, outerR, strokePaint)

        val kx = cx + state.knobX * maxTravel
        val ky = cy + state.knobY * maxTravel
        val knobFill = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.FILL
            color = withAlpha(primaryColor, (alpha * 0.45f).toInt())
        }
        canvas.drawCircle(kx, ky, knobR, knobFill)
        canvas.drawCircle(kx, ky, knobR, strokePaint)

        // Small L / R hint when bound to left/right thumbstick
        val hint = when {
            el.bindings.any { it.isLeftStick } -> "L"
            el.bindings.any { it.isRightStick } -> "R"
            else -> ""
        }
        if (hint.isNotEmpty()) {
            textPaint.textSize = knobR * 0.75f
            val fm = textPaint.fontMetrics
            canvas.drawText(hint, kx, ky - (fm.ascent + fm.descent) / 2f, textPaint)
        }
    }

    private fun drawTrackpad(canvas: Canvas, r: RectF, primaryColor: Int, alpha: Int, u: Float) {
        val rad = u * 1.1f
        canvas.drawRoundRect(r, rad, rad, fillPaint)
        canvas.drawRoundRect(r, rad, rad, strokePaint)

        // Subtle crosshair guide inside trackpad (Winlator look)
        val guide = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = withAlpha(primaryColor, (alpha * 0.45f).toInt())
            strokeWidth = strokePaint.strokeWidth * 0.8f
        }
        val cx = r.centerX()
        val cy = r.centerY()
        val len = min(r.width(), r.height()) * 0.14f
        canvas.drawLine(cx - len, cy, cx + len, cy, guide)
        canvas.drawLine(cx, cy - len, cx, cy + len, guide)
    }

    private fun drawRangeButton(
        canvas: Canvas,
        el: ControlElement,
        state: ElementRuntime,
        r: RectF,
        alpha: Int,
        u: Float,
    ) {
        val items = el.range.bindings
        val visible = el.visibleCount.coerceIn(3, items.size)
        val vertical = el.orientation == 1
        val cellSize = (if (vertical) r.height() else r.width()) / visible
        val maxScroll = (items.size - visible).coerceAtLeast(0) * cellSize
        state.rangeScroll = state.rangeScroll.coerceIn(0f, maxScroll)

        val rad = u * 0.5f * el.scale
        canvas.drawRoundRect(r, rad, rad, fillPaint)
        canvas.drawRoundRect(r, rad, rad, strokePaint)

        canvas.save()
        canvas.clipRect(r)
        val cellRect = RectF()
        val hiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.FILL
            color = withAlpha(COLOR_ACCENT, (alpha * 0.48f).toInt())
        }
        val divPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = withAlpha(COLOR_NORMAL, (alpha * 0.4f).toInt())
            strokeWidth = 2f
        }
        textPaint.textSize = (cellSize * 0.42f).coerceIn(18f, u * 1.6f * el.scale)
        val fm = textPaint.fontMetrics

        for (i in items.indices) {
            val offset = i * cellSize - state.rangeScroll
            if (vertical) {
                cellRect.set(r.left, r.top + offset, r.right, r.top + offset + cellSize)
                if (cellRect.bottom < r.top || cellRect.top > r.bottom) continue
            } else {
                cellRect.set(r.left + offset, r.top, r.left + offset + cellSize, r.bottom)
                if (cellRect.right < r.left || cellRect.left > r.right) continue
            }
            if (state.rangePressedIndex == i) {
                canvas.drawRect(cellRect, hiPaint)
            }
            if (i > 0) {
                if (vertical) canvas.drawLine(r.left, cellRect.top, r.right, cellRect.top, divPaint)
                else canvas.drawLine(cellRect.left, r.top, cellRect.left, r.bottom, divPaint)
            }
            val label = items[i].shortLabel
            canvas.drawText(label, cellRect.centerX(), cellRect.centerY() - (fm.ascent + fm.descent) / 2f, textPaint)
        }
        canvas.restore()
    }

    private fun withAlpha(color: Int, alpha: Int): Int =
        (color and 0x00FFFFFF) or (alpha.coerceIn(0, 255) shl 24)

    // ---- Touch handling ----

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (profile == null || visibility != VISIBLE) return false
        return if (editMode) handleEditTouch(event) else handlePlayTouch(event)
    }

    private fun handleEditTouch(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val hit = hitTest(event.x, event.y)
                selectElement(hit)
                if (hit != null) {
                    editDragPointerId = event.getPointerId(0)
                    editDragOffsetX = hit.x * width - event.x
                    editDragOffsetY = hit.y * height - event.y
                } else {
                    editDragPointerId = -1
                }
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val el = selectedElement ?: return true
                val idx = event.findPointerIndex(editDragPointerId)
                if (idx >= 0) {
                    val step = snappingSize()
                    val rawX = (event.getX(idx) + editDragOffsetX).coerceIn(step, width - step)
                    val rawY = (event.getY(idx) + editDragOffsetY).coerceIn(step, height - step)
                    val snappedX = round(rawX / step) * step
                    val snappedY = round(rawY / step) * step
                    el.x = (snappedX / width).coerceIn(0.02f, 0.98f)
                    el.y = (snappedY / height).coerceIn(0.02f, 0.98f)
                    invalidate()
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (editDragPointerId != -1) {
                    editDragPointerId = -1
                    onProfileChanged?.invoke()
                }
                return true
            }
        }
        return true
    }

    private fun handlePlayTouch(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val idx = event.actionIndex
                val pid = event.getPointerId(idx)
                val px = event.getX(idx)
                val py = event.getY(idx)
                val hit = hitTest(px, py)
                if (hit == null) {
                    // Only consume if we already track another finger OR user blocked outside touch
                    return pointers.isNotEmpty() || blockOutsideTouch
                }
                val track = PointerTrack(hit, px, py, px, py)
                pointers[pid] = track
                onElementPointerDown(hit, px, py)
                invalidate()
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (pointers.isEmpty()) return blockOutsideTouch
                for (i in 0 until event.pointerCount) {
                    val pid = event.getPointerId(i)
                    val track = pointers[pid] ?: continue
                    val px = event.getX(i)
                    val py = event.getY(i)
                    onElementPointerMove(track, px, py)
                    track.lastX = px
                    track.lastY = py
                }
                invalidate()
                return true
            }
            MotionEvent.ACTION_POINTER_UP -> {
                val idx = event.actionIndex
                val pid = event.getPointerId(idx)
                val track = pointers.remove(pid)
                if (track != null) {
                    onElementPointerUp(track)
                    invalidate()
                    return true
                }
                return pointers.isNotEmpty() || blockOutsideTouch
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val had = pointers.isNotEmpty()
                val copy = pointers.values.toList()
                pointers.clear()
                copy.forEach { onElementPointerUp(it) }
                invalidate()
                return had || blockOutsideTouch
            }
        }
        return pointers.isNotEmpty() || blockOutsideTouch
    }

    private fun onElementPointerDown(el: ControlElement, px: Float, py: Float) {
        val state = rt(el)
        when (el.type) {
            ElementType.BUTTON -> {
                if (el.toggleSwitch) {
                    state.toggled = !state.toggled
                    state.pressed = state.toggled
                    el.activeBindings().forEach { applyDigitalBinding(it, state.toggled) }
                } else {
                    if (!state.pressed) {
                        state.pressed = true
                        el.activeBindings().forEach { applyDigitalBinding(it, true) }
                    }
                }
                ensureTickIfNeeded()
            }
            ElementType.D_PAD -> {
                state.pressed = true
                updateDpad(el, state, px, py)
            }
            ElementType.STICK -> {
                state.pressed = true
                updateStick(el, state, px, py)
                ensureTickIfNeeded()
            }
            ElementType.TRACKPAD -> {
                state.pressed = true
            }
            ElementType.RANGE_BUTTON -> {
                state.pressed = true
                val idx = rangeCellAt(el, state, px, py)
                state.rangePressedIndex = idx
                if (idx in el.range.bindings.indices) {
                    applyDigitalBinding(el.range.bindings[idx], true)
                }
            }
        }
    }

    private fun onElementPointerMove(track: PointerTrack, px: Float, py: Float) {
        val el = track.element
        val state = rt(el)
        when (el.type) {
            ElementType.D_PAD -> updateDpad(el, state, px, py)
            ElementType.STICK -> updateStick(el, state, px, py)
            ElementType.TRACKPAD -> {
                val dx = px - track.lastX
                val dy = py - track.lastY
                handleTrackpadDelta(el, dx, dy)
            }
            ElementType.RANGE_BUTTON -> {
                val travel = hypot(px - track.startX, py - track.startY)
                if (!track.moved && travel > snappingSize() * 0.6f) {
                    track.moved = true
                    if (state.rangePressedIndex in el.range.bindings.indices) {
                        applyDigitalBinding(el.range.bindings[state.rangePressedIndex], false)
                        state.rangePressedIndex = -1
                    }
                }
                if (track.moved) {
                    val delta = if (el.orientation == 1) track.lastY - py else track.lastX - px
                    state.rangeScroll += delta
                }
            }
            ElementType.BUTTON -> Unit
        }
    }

    private fun onElementPointerUp(track: PointerTrack) {
        val el = track.element
        val state = rt(el)
        // If another finger is still on the same element, leave it active.
        if (pointers.values.any { it.element === el }) return
        when (el.type) {
            ElementType.BUTTON -> {
                if (!el.toggleSwitch && state.pressed) {
                    state.pressed = false
                    el.activeBindings().forEach { applyDigitalBinding(it, false) }
                }
            }
            ElementType.D_PAD -> {
                state.pressed = false
                setDpadMask(el, state, 0)
            }
            ElementType.STICK -> {
                state.pressed = false
                state.knobX = 0f
                state.knobY = 0f
                applyStickOutputs(el, state, 0f, 0f)
            }
            ElementType.TRACKPAD -> {
                state.pressed = false
                if (el.bindings.any { it.isLeftStick }) GamepadInput.setTouchStick(false, 0f, 0f)
                if (el.bindings.any { it.isRightStick }) GamepadInput.setTouchStick(true, 0f, 0f)
            }
            ElementType.RANGE_BUTTON -> {
                state.pressed = false
                if (state.rangePressedIndex in el.range.bindings.indices) {
                    applyDigitalBinding(el.range.bindings[state.rangePressedIndex], false)
                    state.rangePressedIndex = -1
                }
            }
        }
    }

    private fun updateDpad(el: ControlElement, state: ElementRuntime, px: Float, py: Float) {
        boundsOf(el, tempRect)
        val nx = ((px - tempRect.centerX()) / (tempRect.width() / 2f)).coerceIn(-1f, 1f)
        val ny = ((py - tempRect.centerY()) / (tempRect.height() / 2f)).coerceIn(-1f, 1f)
        var mask = 0
        if (ny < -DPAD_DEAD_ZONE) mask = mask or 1 // Up
        if (nx > DPAD_DEAD_ZONE) mask = mask or 2  // Right
        if (ny > DPAD_DEAD_ZONE) mask = mask or 4  // Down
        if (nx < -DPAD_DEAD_ZONE) mask = mask or 8 // Left
        setDpadMask(el, state, mask)
    }

    private fun setDpadMask(el: ControlElement, state: ElementRuntime, newMask: Int) {
        val diff = state.dpadMask xor newMask
        if (diff == 0) return
        for (i in 0 until 4) {
            val bit = 1 shl i
            if (diff and bit != 0) {
                val down = (newMask and bit) != 0
                applyDigitalBinding(el.getBinding(i), down)
            }
        }
        state.dpadMask = newMask
    }

    private fun updateStick(el: ControlElement, state: ElementRuntime, px: Float, py: Float) {
        boundsOf(el, tempRect)
        val radius = (min(tempRect.width(), tempRect.height()) / 2f).coerceAtLeast(1f)
        var nx = (px - tempRect.centerX()) / radius
        var ny = (py - tempRect.centerY()) / radius
        val mag = hypot(nx, ny)
        if (mag > 1f) {
            nx /= mag
            ny /= mag
        }
        state.knobX = nx
        state.knobY = ny
        applyStickOutputs(el, state, nx, ny)
    }

    private fun applyStickOutputs(el: ControlElement, state: ElementRuntime, nx: Float, ny: Float) {
        val hasLeftStick = el.bindings.any { it.isLeftStick }
        val hasRightStick = el.bindings.any { it.isRightStick }
        val mag = hypot(nx, ny)
        val effX = if (mag < STICK_DEAD_ZONE) 0f else nx
        val effY = if (mag < STICK_DEAD_ZONE) 0f else ny
        // Gamepad analog stick (XInput +Y is UP, screen +Y is DOWN)
        if (hasLeftStick) GamepadInput.setTouchStick(false, effX, -effY)
        if (hasRightStick) GamepadInput.setTouchStick(true, effX, -effY)

        // Digital bindings (e.g. WASD or D-Pad bound to a stick)
        var mask = 0
        if (ny < -DPAD_DEAD_ZONE) mask = mask or 1
        if (nx > DPAD_DEAD_ZONE) mask = mask or 2
        if (ny > DPAD_DEAD_ZONE) mask = mask or 4
        if (nx < -DPAD_DEAD_ZONE) mask = mask or 8
        val diff = state.dpadMask xor mask
        if (diff != 0) {
            for (i in 0 until 4) {
                val bit = 1 shl i
                if (diff and bit != 0) {
                    val b = el.getBinding(i)
                    if (!b.isLeftStick && !b.isRightStick && !b.isMouseMove) {
                        applyDigitalBinding(b, (mask and bit) != 0)
                    }
                }
            }
            state.dpadMask = mask
        }
    }

    private fun handleTrackpadDelta(el: ControlElement, dx: Float, dy: Float) {
        val speed = (profile?.cursorSpeed ?: 1.0f) * 1.5f
        val hasMouse = el.bindings.any { it.isMouseMove }
        if (hasMouse) {
            mouseFracX += dx * speed
            mouseFracY += dy * speed
            val ix = mouseFracX.toInt()
            val iy = mouseFracY.toInt()
            mouseFracX -= ix
            mouseFracY -= iy
            if (ix != 0 || iy != 0) VsockInput.virtualMouseMove(ix, iy)
        }
        val hasLeftStick = el.bindings.any { it.isLeftStick }
        val hasRightStick = el.bindings.any { it.isRightStick }
        if (hasLeftStick || hasRightStick) {
            val sens = (snappingSize() * 1.2f).coerceAtLeast(12f)
            val sx = (dx * speed / sens).coerceIn(-1f, 1f)
            val sy = (-dy * speed / sens).coerceIn(-1f, 1f)
            if (hasLeftStick) GamepadInput.setTouchStick(false, sx, sy)
            if (hasRightStick) GamepadInput.setTouchStick(true, sx, sy)
        }
    }

    private fun rangeCellAt(el: ControlElement, state: ElementRuntime, px: Float, py: Float): Int {
        boundsOf(el, tempRect)
        val items = el.range.bindings
        val visible = el.visibleCount.coerceIn(3, items.size)
        val vertical = el.orientation == 1
        val cellSize = (if (vertical) tempRect.height() else tempRect.width()) / visible
        val local = if (vertical) (py - tempRect.top) + state.rangeScroll else (px - tempRect.left) + state.rangeScroll
        return (local / cellSize).toInt().coerceIn(-1, items.size)
    }

    private fun ensureTickIfNeeded() {
        if (ticking) return
        val p = profile ?: return
        val needs = p.elements.any { el ->
            val st = runtime[el] ?: return@any false
            st.pressed && (el.bindings.any { it.isMouseMove } ||
                (el.type == ElementType.BUTTON && el.bindings.any { it == TouchBinding.MOUSE_SCROLL_UP || it == TouchBinding.MOUSE_SCROLL_DOWN }))
        }
        if (needs) {
            ticking = true
            ui.postDelayed(tickRunnable, TICK_MS)
        }
    }

    private fun tickContinuousInputs(): Boolean {
        val p = profile ?: return false
        val speed = (profile?.cursorSpeed ?: 1.0f) * 14f
        var any = false
        var dx = 0f
        var dy = 0f
        for (el in p.elements) {
            val st = runtime[el] ?: continue
            if (!st.pressed) continue
            if (el.type == ElementType.STICK && el.bindings.any { it.isMouseMove }) {
                any = true
                if (hypot(st.knobX, st.knobY) >= STICK_DEAD_ZONE) {
                    dx += st.knobX * speed
                    dy += st.knobY * speed
                }
            } else if (el.type == ElementType.BUTTON) {
                for (b in el.activeBindings()) {
                    when (b) {
                        TouchBinding.MOUSE_MOVE_UP -> { any = true; dy -= speed * 0.7f }
                        TouchBinding.MOUSE_MOVE_DOWN -> { any = true; dy += speed * 0.7f }
                        TouchBinding.MOUSE_MOVE_LEFT -> { any = true; dx -= speed * 0.7f }
                        TouchBinding.MOUSE_MOVE_RIGHT -> { any = true; dx += speed * 0.7f }
                        else -> Unit
                    }
                }
            }
        }
        if (abs(dx) > 0.01f || abs(dy) > 0.01f) {
            mouseFracX += dx
            mouseFracY += dy
            val ix = mouseFracX.toInt()
            val iy = mouseFracY.toInt()
            mouseFracX -= ix
            mouseFracY -= iy
            if (ix != 0 || iy != 0) VsockInput.virtualMouseMove(ix, iy)
        }
        return any
    }

    // ---- Reference-counted input dispatch ----

    private fun applyDigitalBinding(b: TouchBinding, down: Boolean) {
        if (b == TouchBinding.NONE) return
        when (b.category) {
            BindingCategory.KEYBOARD -> {
                if (b.keyCode == 0) return
                val cur = heldKeys[b.keyCode] ?: 0
                val next = if (down) cur + 1 else (cur - 1).coerceAtLeast(0)
                if (next == 0) heldKeys.remove(b.keyCode) else heldKeys[b.keyCode] = next
                if (cur == 0 && next > 0) VsockInput.virtualKey(b.keyCode, true)
                else if (cur > 0 && next == 0) VsockInput.virtualKey(b.keyCode, false)
            }
            BindingCategory.MOUSE -> {
                when (b) {
                    TouchBinding.MOUSE_SCROLL_UP -> if (down) VsockInput.virtualMouseMove(0, 0, 120)
                    TouchBinding.MOUSE_SCROLL_DOWN -> if (down) VsockInput.virtualMouseMove(0, 0, -120)
                    else -> if (b.mouseMask != 0) {
                        val cur = heldMouseButtons[b.mouseMask] ?: 0
                        val next = if (down) cur + 1 else (cur - 1).coerceAtLeast(0)
                        if (next == 0) heldMouseButtons.remove(b.mouseMask) else heldMouseButtons[b.mouseMask] = next
                        if (cur == 0 && next > 0) VsockInput.virtualMouseButton(b.mouseMask, true)
                        else if (cur > 0 && next == 0) VsockInput.virtualMouseButton(b.mouseMask, false)
                    }
                }
            }
            BindingCategory.GAMEPAD -> {
                when (b) {
                    TouchBinding.GAMEPAD_BUTTON_L2 -> {
                        val prev = heldLtCount
                        heldLtCount = if (down) prev + 1 else (prev - 1).coerceAtLeast(0)
                        if (prev == 0 && heldLtCount > 0) GamepadInput.setTouchTrigger(false, 255)
                        else if (prev > 0 && heldLtCount == 0) GamepadInput.setTouchTrigger(false, 0)
                    }
                    TouchBinding.GAMEPAD_BUTTON_R2 -> {
                        val prev = heldRtCount
                        heldRtCount = if (down) prev + 1 else (prev - 1).coerceAtLeast(0)
                        if (prev == 0 && heldRtCount > 0) GamepadInput.setTouchTrigger(true, 255)
                        else if (prev > 0 && heldRtCount == 0) GamepadInput.setTouchTrigger(true, 0)
                    }
                    TouchBinding.GAMEPAD_LEFT_THUMB_UP -> GamepadInput.setTouchStick(false, 0f, if (down) 1f else 0f)
                    TouchBinding.GAMEPAD_LEFT_THUMB_DOWN -> GamepadInput.setTouchStick(false, 0f, if (down) -1f else 0f)
                    TouchBinding.GAMEPAD_LEFT_THUMB_LEFT -> GamepadInput.setTouchStick(false, if (down) -1f else 0f, 0f)
                    TouchBinding.GAMEPAD_LEFT_THUMB_RIGHT -> GamepadInput.setTouchStick(false, if (down) 1f else 0f, 0f)
                    TouchBinding.GAMEPAD_RIGHT_THUMB_UP -> GamepadInput.setTouchStick(true, 0f, if (down) 1f else 0f)
                    TouchBinding.GAMEPAD_RIGHT_THUMB_DOWN -> GamepadInput.setTouchStick(true, 0f, if (down) -1f else 0f)
                    TouchBinding.GAMEPAD_RIGHT_THUMB_LEFT -> GamepadInput.setTouchStick(true, if (down) -1f else 0f, 0f)
                    TouchBinding.GAMEPAD_RIGHT_THUMB_RIGHT -> GamepadInput.setTouchStick(true, if (down) 1f else 0f, 0f)
                    else -> if (b.gamepadBit != 0) {
                        val cur = heldGamepadButtons[b.gamepadBit] ?: 0
                        val next = if (down) cur + 1 else (cur - 1).coerceAtLeast(0)
                        if (next == 0) heldGamepadButtons.remove(b.gamepadBit) else heldGamepadButtons[b.gamepadBit] = next
                        if (cur == 0 && next > 0) GamepadInput.setTouchButton(b.gamepadBit, true)
                        else if (cur > 0 && next == 0) GamepadInput.setTouchButton(b.gamepadBit, false)
                    }
                }
            }
            BindingCategory.NONE -> Unit
        }
    }

    /** Cleanly releases every held key, mouse button, and virtual gamepad input. */
    fun releaseAll() {
        ui.removeCallbacks(tickRunnable)
        ticking = false
        pointers.clear()
        runtime.clear()
        heldKeys.keys.toList().forEach { VsockInput.virtualKey(it, false) }
        heldKeys.clear()
        heldMouseButtons.keys.toList().forEach { VsockInput.virtualMouseButton(it, false) }
        heldMouseButtons.clear()
        heldGamepadButtons.clear()
        heldLtCount = 0
        heldRtCount = 0
        mouseFracX = 0f
        mouseFracY = 0f
        GamepadInput.clearTouchState()
        invalidate()
    }
}
