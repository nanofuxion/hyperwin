package dev.hyperwin.app

import android.content.Context
import android.view.KeyEvent
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * On-screen touch controls data model and profile manager, modeled on Winlator's Input Controls
 * UX and compatible with Winlator `.icp` JSON profile files.
 */
enum class BindingCategory(val label: String) {
    NONE("None"),
    GAMEPAD("Gamepad"),
    KEYBOARD("Keyboard"),
    MOUSE("Mouse"),
}

enum class TouchBinding(
    val label: String,
    val shortLabel: String,
    val category: BindingCategory,
    val keyCode: Int = 0,
    val gamepadBit: Int = 0,
    val mouseMask: Int = 0,
) {
    NONE("None", "", BindingCategory.NONE),

    // ---- Mouse ----
    MOUSE_LEFT_BUTTON("Left Click", "LMB", BindingCategory.MOUSE, mouseMask = 1),
    MOUSE_RIGHT_BUTTON("Right Click", "RMB", BindingCategory.MOUSE, mouseMask = 2),
    MOUSE_MIDDLE_BUTTON("Middle Click", "MMB", BindingCategory.MOUSE, mouseMask = 4),
    MOUSE_MOVE_UP("Mouse Move Up", "M↑", BindingCategory.MOUSE),
    MOUSE_MOVE_RIGHT("Mouse Move Right", "M→", BindingCategory.MOUSE),
    MOUSE_MOVE_DOWN("Mouse Move Down", "M↓", BindingCategory.MOUSE),
    MOUSE_MOVE_LEFT("Mouse Move Left", "M←", BindingCategory.MOUSE),
    MOUSE_SCROLL_UP("Scroll Up", "WH↑", BindingCategory.MOUSE),
    MOUSE_SCROLL_DOWN("Scroll Down", "WH↓", BindingCategory.MOUSE),

    // ---- Gamepad (XInput) ----
    GAMEPAD_BUTTON_A("A", "A", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_A),
    GAMEPAD_BUTTON_B("B", "B", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_B),
    GAMEPAD_BUTTON_X("X", "X", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_X),
    GAMEPAD_BUTTON_Y("Y", "Y", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_Y),
    GAMEPAD_BUTTON_L1("L1 / LB", "L1", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_LEFT_SHOULDER),
    GAMEPAD_BUTTON_R1("R1 / RB", "R1", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_RIGHT_SHOULDER),
    GAMEPAD_BUTTON_L2("L2 / LT", "L2", BindingCategory.GAMEPAD),
    GAMEPAD_BUTTON_R2("R2 / RT", "R2", BindingCategory.GAMEPAD),
    GAMEPAD_BUTTON_L3("L3 / LS", "L3", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_LEFT_THUMB),
    GAMEPAD_BUTTON_R3("R3 / RS", "R3", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_RIGHT_THUMB),
    GAMEPAD_BUTTON_START("Start", "START", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_START),
    GAMEPAD_BUTTON_SELECT("Select / Back", "BACK", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_BACK),
    GAMEPAD_BUTTON_GUIDE("Guide", "HOME", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_GUIDE),
    GAMEPAD_DPAD_UP("D-Pad Up", "↑", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_DPAD_UP),
    GAMEPAD_DPAD_RIGHT("D-Pad Right", "→", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_DPAD_RIGHT),
    GAMEPAD_DPAD_DOWN("D-Pad Down", "↓", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_DPAD_DOWN),
    GAMEPAD_DPAD_LEFT("D-Pad Left", "←", BindingCategory.GAMEPAD, gamepadBit = GamepadInput.XINPUT_DPAD_LEFT),
    GAMEPAD_LEFT_THUMB_UP("Left Stick Up", "L↑", BindingCategory.GAMEPAD),
    GAMEPAD_LEFT_THUMB_RIGHT("Left Stick Right", "L→", BindingCategory.GAMEPAD),
    GAMEPAD_LEFT_THUMB_DOWN("Left Stick Down", "L↓", BindingCategory.GAMEPAD),
    GAMEPAD_LEFT_THUMB_LEFT("Left Stick Left", "L←", BindingCategory.GAMEPAD),
    GAMEPAD_RIGHT_THUMB_UP("Right Stick Up", "R↑", BindingCategory.GAMEPAD),
    GAMEPAD_RIGHT_THUMB_RIGHT("Right Stick Right", "R→", BindingCategory.GAMEPAD),
    GAMEPAD_RIGHT_THUMB_DOWN("Right Stick Down", "R↓", BindingCategory.GAMEPAD),
    GAMEPAD_RIGHT_THUMB_LEFT("Right Stick Left", "R←", BindingCategory.GAMEPAD),

    // ---- Keyboard ----
    KEY_ESC("Esc", "ESC", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_ESCAPE),
    KEY_TAB("Tab", "TAB", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_TAB),
    KEY_ENTER("Enter", "ENT", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_ENTER),
    KEY_SPACE("Space", "SPC", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_SPACE),
    KEY_SHIFT_L("Left Shift", "SHF", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_SHIFT_LEFT),
    KEY_SHIFT_R("Right Shift", "RSH", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_SHIFT_RIGHT),
    KEY_CTRL_L("Left Ctrl", "CTL", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_CTRL_LEFT),
    KEY_CTRL_R("Right Ctrl", "RCT", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_CTRL_RIGHT),
    KEY_ALT_L("Left Alt", "ALT", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_ALT_LEFT),
    KEY_ALT_R("Right Alt", "RAL", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_ALT_RIGHT),
    KEY_BACKSPACE("Backspace", "BKSP", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_DEL),
    KEY_DEL("Delete", "DEL", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_FORWARD_DEL),
    KEY_UP("Arrow Up", "↑", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_DPAD_UP),
    KEY_DOWN("Arrow Down", "↓", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_DPAD_DOWN),
    KEY_LEFT("Arrow Left", "←", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_DPAD_LEFT),
    KEY_RIGHT("Arrow Right", "→", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_DPAD_RIGHT),
    KEY_HOME("Home", "HOM", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_MOVE_HOME),
    KEY_END("End", "END", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_MOVE_END),
    KEY_PRIOR("Page Up", "PGU", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_PAGE_UP),
    KEY_NEXT("Page Down", "PGD", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_PAGE_DOWN),
    KEY_INSERT("Insert", "INS", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_INSERT),
    KEY_CAPS_LOCK("Caps Lock", "CAP", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_CAPS_LOCK),

    KEY_A("A", "A", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_A),
    KEY_B("B", "B", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_B),
    KEY_C("C", "C", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_C),
    KEY_D("D", "D", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_D),
    KEY_E("E", "E", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_E),
    KEY_F("F", "F", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F),
    KEY_G("G", "G", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_G),
    KEY_H("H", "H", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_H),
    KEY_I("I", "I", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_I),
    KEY_J("J", "J", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_J),
    KEY_K("K", "K", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_K),
    KEY_L("L", "L", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_L),
    KEY_M("M", "M", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_M),
    KEY_N("N", "N", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_N),
    KEY_O("O", "O", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_O),
    KEY_P("P", "P", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_P),
    KEY_Q("Q", "Q", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_Q),
    KEY_R("R", "R", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_R),
    KEY_S("S", "S", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_S),
    KEY_T("T", "T", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_T),
    KEY_U("U", "U", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_U),
    KEY_V("V", "V", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_V),
    KEY_W("W", "W", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_W),
    KEY_X("X", "X", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_X),
    KEY_Y("Y", "Y", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_Y),
    KEY_Z("Z", "Z", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_Z),

    KEY_0("0", "0", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_0),
    KEY_1("1", "1", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_1),
    KEY_2("2", "2", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_2),
    KEY_3("3", "3", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_3),
    KEY_4("4", "4", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_4),
    KEY_5("5", "5", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_5),
    KEY_6("6", "6", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_6),
    KEY_7("7", "7", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_7),
    KEY_8("8", "8", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_8),
    KEY_9("9", "9", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_9),

    KEY_F1("F1", "F1", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F1),
    KEY_F2("F2", "F2", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F2),
    KEY_F3("F3", "F3", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F3),
    KEY_F4("F4", "F4", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F4),
    KEY_F5("F5", "F5", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F5),
    KEY_F6("F6", "F6", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F6),
    KEY_F7("F7", "F7", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F7),
    KEY_F8("F8", "F8", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F8),
    KEY_F9("F9", "F9", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F9),
    KEY_F10("F10", "F10", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F10),
    KEY_F11("F11", "F11", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F11),
    KEY_F12("F12", "F12", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_F12),

    KEY_MINUS("-", "-", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_MINUS),
    KEY_EQUAL("=", "=", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_EQUALS),
    KEY_BRACKET_LEFT("[", "[", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_LEFT_BRACKET),
    KEY_BRACKET_RIGHT("]", "]", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_RIGHT_BRACKET),
    KEY_SEMICOLON(";", ";", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_SEMICOLON),
    KEY_APOSTROPHE("'", "'", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_APOSTROPHE),
    KEY_GRAVE("`", "`", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_GRAVE),
    KEY_BACKSLASH("\\", "\\", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_BACKSLASH),
    KEY_COMMA(",", ",", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_COMMA),
    KEY_PERIOD(".", ".", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_PERIOD),
    KEY_SLASH("/", "/", BindingCategory.KEYBOARD, KeyEvent.KEYCODE_SLASH);

    /** Controller names change presentation only; saved bindings keep their positional meaning. */
    fun controllerLabel(playStation: Boolean): String = when (this) {
        GAMEPAD_BUTTON_A -> if (playStation) "✕" else "A"
        GAMEPAD_BUTTON_B -> if (playStation) "○" else "B"
        GAMEPAD_BUTTON_X -> if (playStation) "□" else "X"
        GAMEPAD_BUTTON_Y -> if (playStation) "△" else "Y"
        GAMEPAD_BUTTON_L1 -> if (playStation) "L1" else "LB"
        GAMEPAD_BUTTON_R1 -> if (playStation) "R1" else "RB"
        GAMEPAD_BUTTON_L2 -> if (playStation) "L2" else "LT"
        GAMEPAD_BUTTON_R2 -> if (playStation) "R2" else "RT"
        GAMEPAD_BUTTON_L3 -> if (playStation) "L3" else "LS"
        GAMEPAD_BUTTON_R3 -> if (playStation) "R3" else "RS"
        GAMEPAD_BUTTON_SELECT -> if (playStation) "Share" else "View"
        GAMEPAD_BUTTON_START -> if (playStation) "Options" else "Menu"
        GAMEPAD_BUTTON_GUIDE -> if (playStation) "PS" else "Xbox"
        else -> shortLabel
    }

    val isLeftStick: Boolean
        get() = this == GAMEPAD_LEFT_THUMB_UP || this == GAMEPAD_LEFT_THUMB_RIGHT ||
            this == GAMEPAD_LEFT_THUMB_DOWN || this == GAMEPAD_LEFT_THUMB_LEFT

    val isRightStick: Boolean
        get() = this == GAMEPAD_RIGHT_THUMB_UP || this == GAMEPAD_RIGHT_THUMB_RIGHT ||
            this == GAMEPAD_RIGHT_THUMB_DOWN || this == GAMEPAD_RIGHT_THUMB_LEFT

    val isMouseMove: Boolean
        get() = this == MOUSE_MOVE_UP || this == MOUSE_MOVE_RIGHT ||
            this == MOUSE_MOVE_DOWN || this == MOUSE_MOVE_LEFT

    companion object {
        fun byCategory(category: BindingCategory): List<TouchBinding> =
            entries.filter { it.category == category }

        fun fromName(name: String?): TouchBinding {
            if (name.isNullOrBlank()) return NONE
            entries.firstOrNull { it.name == name }?.let { return it }
            // Compatibility aliases for Winlator .icp files
            return when (name) {
                "KEY_BKSP" -> KEY_BACKSPACE
                "KEY_PAGE_UP" -> KEY_PRIOR
                "KEY_PAGE_DOWN" -> KEY_NEXT
                "KEY_PRTSCN" -> NONE
                "MOUSE_LEFT" -> MOUSE_LEFT_BUTTON
                "MOUSE_RIGHT" -> MOUSE_RIGHT_BUTTON
                "MOUSE_MIDDLE" -> MOUSE_MIDDLE_BUTTON
                else -> NONE
            }
        }
    }
}

enum class ElementType(val label: String) {
    BUTTON("Button"),
    D_PAD("D-Pad"),
    STICK("Stick"),
    TRACKPAD("Trackpad"),
    RANGE_BUTTON("Range Button");

    companion object {
        fun fromName(name: String?): ElementType = entries.firstOrNull { it.name == name } ?: BUTTON
    }
}

enum class ElementShape(val label: String) {
    CIRCLE("Circle"),
    RECT("Rectangle"),
    ROUND_RECT("Rounded Rect"),
    SQUARE("Square");

    companion object {
        fun fromName(name: String?): ElementShape = entries.firstOrNull { it.name == name } ?: CIRCLE
    }
}

enum class ElementRange(val label: String, val bindings: List<TouchBinding>) {
    FROM_0_TO_9(
        "0 to 9",
        listOf(
            TouchBinding.KEY_1, TouchBinding.KEY_2, TouchBinding.KEY_3, TouchBinding.KEY_4, TouchBinding.KEY_5,
            TouchBinding.KEY_6, TouchBinding.KEY_7, TouchBinding.KEY_8, TouchBinding.KEY_9, TouchBinding.KEY_0,
        ),
    ),
    FROM_A_TO_Z(
        "A to Z",
        listOf(
            TouchBinding.KEY_A, TouchBinding.KEY_B, TouchBinding.KEY_C, TouchBinding.KEY_D, TouchBinding.KEY_E,
            TouchBinding.KEY_F, TouchBinding.KEY_G, TouchBinding.KEY_H, TouchBinding.KEY_I, TouchBinding.KEY_J,
            TouchBinding.KEY_K, TouchBinding.KEY_L, TouchBinding.KEY_M, TouchBinding.KEY_N, TouchBinding.KEY_O,
            TouchBinding.KEY_P, TouchBinding.KEY_Q, TouchBinding.KEY_R, TouchBinding.KEY_S, TouchBinding.KEY_T,
            TouchBinding.KEY_U, TouchBinding.KEY_V, TouchBinding.KEY_W, TouchBinding.KEY_X, TouchBinding.KEY_Y,
            TouchBinding.KEY_Z,
        ),
    ),
    FROM_F1_TO_F12(
        "F1 to F12",
        listOf(
            TouchBinding.KEY_F1, TouchBinding.KEY_F2, TouchBinding.KEY_F3, TouchBinding.KEY_F4,
            TouchBinding.KEY_F5, TouchBinding.KEY_F6, TouchBinding.KEY_F7, TouchBinding.KEY_F8,
            TouchBinding.KEY_F9, TouchBinding.KEY_F10, TouchBinding.KEY_F11, TouchBinding.KEY_F12,
        ),
    );

    companion object {
        fun fromName(name: String?): ElementRange = entries.firstOrNull { it.name == name } ?: FROM_0_TO_9
    }
}

/** One customizable control on the overlay. Coordinates x, y are normalized 0..1 across the view. */
data class ControlElement(
    var type: ElementType = ElementType.BUTTON,
    var shape: ElementShape = ElementShape.CIRCLE,
    var bindings: MutableList<TouchBinding> = mutableListOf(
        TouchBinding.NONE, TouchBinding.NONE, TouchBinding.NONE, TouchBinding.NONE,
    ),
    var scale: Float = 1.0f,
    var opacity: Float = 1.0f,
    var x: Float = 0.5f,
    var y: Float = 0.5f,
    var toggleSwitch: Boolean = false,
    var text: String = "",
    var orientation: Int = 0, // 0 = horizontal, 1 = vertical (for RANGE_BUTTON)
    var range: ElementRange = ElementRange.FROM_0_TO_9,
    var visibleCount: Int = 5, // visible buttons in RANGE_BUTTON strip
) {
    fun getBinding(index: Int): TouchBinding = bindings.getOrElse(index) { TouchBinding.NONE }

    fun setBinding(index: Int, binding: TouchBinding) {
        while (bindings.size <= index) bindings.add(TouchBinding.NONE)
        bindings[index] = binding
    }

    fun activeBindings(): List<TouchBinding> = bindings.filter { it != TouchBinding.NONE }

    fun displayText(playStation: Boolean = false): String {
        if (text.isNotBlank()) return text
        val active = activeBindings()
        if (active.isEmpty()) return ""
        return if (active.size == 1) active[0].controllerLabel(playStation)
        else active.take(2).joinToString("+") { it.controllerLabel(playStation) }
    }

    fun usesGamepad(): Boolean = bindings.any { it.category == BindingCategory.GAMEPAD }

    fun deepCopy(): ControlElement = copy(bindings = bindings.toMutableList())

    fun toJson(): JSONObject = JSONObject().apply {
        put("type", type.name)
        put("shape", shape.name)
        put("bindings", JSONArray().apply {
            for (i in 0 until 4) put(getBinding(i).name)
        })
        put("scale", scale.toDouble())
        put("opacity", opacity.toDouble())
        put("x", x.toDouble())
        put("y", y.toDouble())
        put("toggleSwitch", toggleSwitch)
        put("text", text)
        if (type == ElementType.RANGE_BUTTON) {
            put("orientation", orientation)
            put("range", range.name)
            put("visibleCount", visibleCount)
        }
    }

    companion object {
        fun fromJson(o: JSONObject): ControlElement {
            val type = ElementType.fromName(o.optString("type", "BUTTON"))
            val shape = ElementShape.fromName(o.optString("shape", "CIRCLE"))
            val arr = o.optJSONArray("bindings")
            val list = MutableList(4) { i ->
                TouchBinding.fromName(arr?.optString(i, "NONE"))
            }
            val scale = o.optDouble("scale", 1.0).toFloat().coerceIn(0.4f, 2.5f)
            val opacity = o.optDouble("opacity", 1.0).toFloat().coerceIn(0.1f, 1.0f)
            val x = o.optDouble("x", 0.5).toFloat().coerceIn(0.02f, 0.98f)
            val y = o.optDouble("y", 0.5).toFloat().coerceIn(0.02f, 0.98f)
            val toggle = o.optBoolean("toggleSwitch", false)
            val text = o.optString("text", "")
            val orientation = o.optInt("orientation", 0).coerceIn(0, 1)
            val range = ElementRange.fromName(o.optString("range", "FROM_0_TO_9"))
            val visibleCount = o.optInt("visibleCount", 5).coerceIn(3, 10)
            return ControlElement(type, shape, list, scale, opacity, x, y, toggle, text, orientation, range, visibleCount)
        }
    }
}

data class ControlsProfile(
    val id: Int,
    var name: String,
    var cursorSpeed: Float = 1.0f,
    val elements: MutableList<ControlElement> = mutableListOf(),
) {
    fun usesGamepad(): Boolean = elements.any { it.usesGamepad() }

    fun deepCopy(newId: Int = id, newName: String = name): ControlsProfile =
        ControlsProfile(newId, newName, cursorSpeed, elements.map { it.deepCopy() }.toMutableList())

    fun toJson(): JSONObject = JSONObject().apply {
        put("id", id)
        put("name", name)
        put("cursorSpeed", cursorSpeed.toDouble())
        put("elements", JSONArray().apply {
            elements.forEach { put(it.toJson()) }
        })
    }

    companion object {
        fun fromJson(o: JSONObject, fallbackId: Int): ControlsProfile {
            val id = o.optInt("id", fallbackId).takeIf { it > 0 } ?: fallbackId
            val name = o.optString("name", "Profile $id").ifBlank { "Profile $id" }
            val speed = o.optDouble("cursorSpeed", 1.0).toFloat().coerceIn(0.25f, 3.0f)
            val els = mutableListOf<ControlElement>()
            val arr = o.optJSONArray("elements")
            if (arr != null) {
                for (i in 0 until arr.length()) {
                    arr.optJSONObject(i)?.let { els.add(ControlElement.fromJson(it)) }
                }
            }
            return ControlsProfile(id, name, speed, els)
        }
    }
}

/**
 * Manages persisted `.icp` profiles in `filesDir/inputcontrols/` and user overlay preferences.
 */
object TouchControlsManager {
    private const val PREFS = "hyperwin"
    const val PREF_ACTIVE_PROFILE = "touchControlsProfileId"
    const val PREF_OVERLAY_OPACITY = "touchControlsOpacity"
    const val PREF_BLOCK_OUTSIDE_TOUCH = "touchControlsBlockOutside"

    private fun dir(ctx: Context): File = File(ctx.filesDir, "inputcontrols").apply { mkdirs() }
    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun activeProfileId(ctx: Context): Int = prefs(ctx).getInt(PREF_ACTIVE_PROFILE, -1)

    fun setActiveProfileId(ctx: Context, id: Int) {
        prefs(ctx).edit().putInt(PREF_ACTIVE_PROFILE, id).apply()
    }

    fun overlayOpacity(ctx: Context): Float =
        prefs(ctx).getFloat(PREF_OVERLAY_OPACITY, 0.65f).coerceIn(0.15f, 1.0f)

    fun setOverlayOpacity(ctx: Context, opacity: Float) {
        prefs(ctx).edit().putFloat(PREF_OVERLAY_OPACITY, opacity.coerceIn(0.15f, 1.0f)).apply()
    }

    fun blockOutsideTouch(ctx: Context): Boolean = prefs(ctx).getBoolean(PREF_BLOCK_OUTSIDE_TOUCH, false)

    fun setBlockOutsideTouch(ctx: Context, block: Boolean) {
        prefs(ctx).edit().putBoolean(PREF_BLOCK_OUTSIDE_TOUCH, block).apply()
    }

    fun ensureDefaults(ctx: Context) {
        val folder = dir(ctx)
        val existing = folder.listFiles { f -> f.isFile && f.name.endsWith(".icp") }
        if (existing != null && existing.isNotEmpty()) return
        defaultProfiles().forEach { saveProfile(ctx, it) }
    }

    fun resetDefaults(ctx: Context) {
        defaultProfiles().forEach { saveProfile(ctx, it) }
    }

    fun listProfiles(ctx: Context): List<ControlsProfile> {
        ensureDefaults(ctx)
        val files = dir(ctx).listFiles { f -> f.isFile && f.name.endsWith(".icp") }.orEmpty()
        return files.mapNotNull { file ->
            runCatching {
                val fallbackId = file.name.removePrefix("profile-").removeSuffix(".icp").toIntOrNull() ?: 1
                ControlsProfile.fromJson(JSONObject(file.readText()), fallbackId)
            }.getOrNull()
        }.sortedBy { it.id }
    }

    fun getProfile(ctx: Context, id: Int): ControlsProfile? =
        if (id <= 0) null else listProfiles(ctx).firstOrNull { it.id == id }

    fun activeProfile(ctx: Context): ControlsProfile? = getProfile(ctx, activeProfileId(ctx))

    fun saveProfile(ctx: Context, profile: ControlsProfile) {
        File(dir(ctx), "profile-${profile.id}.icp").writeText(profile.toJson().toString(2))
    }

    private fun nextId(ctx: Context): Int = (listProfiles(ctx).maxOfOrNull { it.id } ?: 0) + 1

    fun createProfile(ctx: Context, name: String): ControlsProfile {
        val p = ControlsProfile(nextId(ctx), name.trim().ifEmpty { "Custom" })
        saveProfile(ctx, p)
        return p
    }

    fun duplicateProfile(ctx: Context, source: ControlsProfile): ControlsProfile {
        val copy = source.deepCopy(nextId(ctx), "${source.name} (Copy)")
        saveProfile(ctx, copy)
        return copy
    }

    fun renameProfile(ctx: Context, profile: ControlsProfile, newName: String) {
        val trimmed = newName.trim()
        if (trimmed.isEmpty()) return
        profile.name = trimmed
        saveProfile(ctx, profile)
    }

    fun deleteProfile(ctx: Context, id: Int) {
        File(dir(ctx), "profile-$id.icp").delete()
        if (activeProfileId(ctx) == id) setActiveProfileId(ctx, -1)
    }

    fun importProfile(ctx: Context, jsonText: String): ControlsProfile {
        val obj = JSONObject(jsonText)
        val id = nextId(ctx)
        val parsed = ControlsProfile.fromJson(obj, id)
        val imported = parsed.deepCopy(newId = id, newName = parsed.name)
        saveProfile(ctx, imported)
        return imported
    }

    fun exportProfileJson(profile: ControlsProfile): String = profile.toJson().toString(2)

    // ---- Built-in Winlator-style profiles ----

    private fun btn(
        binding: TouchBinding,
        x: Float,
        y: Float,
        shape: ElementShape = ElementShape.CIRCLE,
        scale: Float = 1.0f,
        toggle: Boolean = false,
        text: String = "",
    ) = ControlElement(
        type = ElementType.BUTTON,
        shape = shape,
        bindings = mutableListOf(binding, TouchBinding.NONE, TouchBinding.NONE, TouchBinding.NONE),
        scale = scale,
        x = x,
        y = y,
        toggleSwitch = toggle,
        text = text,
    )

    private fun pad4(
        type: ElementType,
        up: TouchBinding,
        right: TouchBinding,
        down: TouchBinding,
        left: TouchBinding,
        x: Float,
        y: Float,
        scale: Float = 1.0f,
    ) = ControlElement(
        type = type,
        shape = ElementShape.CIRCLE,
        bindings = mutableListOf(up, right, down, left),
        scale = scale,
        x = x,
        y = y,
    )

    fun defaultProfiles(): List<ControlsProfile> = listOf(
        // 1: Virtual Gamepad (full Xbox 360 / DualShock layout matching Winlator's controls-3.icp)
        ControlsProfile(
            id = 1,
            name = "Virtual Gamepad",
            cursorSpeed = 1.0f,
            elements = mutableListOf(
                // Left stick & D-Pad
                pad4(
                    ElementType.STICK,
                    TouchBinding.GAMEPAD_LEFT_THUMB_UP, TouchBinding.GAMEPAD_LEFT_THUMB_RIGHT,
                    TouchBinding.GAMEPAD_LEFT_THUMB_DOWN, TouchBinding.GAMEPAD_LEFT_THUMB_LEFT,
                    0.12f, 0.72f, 1.05f,
                ),
                pad4(
                    ElementType.D_PAD,
                    TouchBinding.GAMEPAD_DPAD_UP, TouchBinding.GAMEPAD_DPAD_RIGHT,
                    TouchBinding.GAMEPAD_DPAD_DOWN, TouchBinding.GAMEPAD_DPAD_LEFT,
                    0.24f, 0.42f, 0.85f,
                ),
                // Right stick & face buttons (A/B/X/Y diamond)
                pad4(
                    ElementType.STICK,
                    TouchBinding.GAMEPAD_RIGHT_THUMB_UP, TouchBinding.GAMEPAD_RIGHT_THUMB_RIGHT,
                    TouchBinding.GAMEPAD_RIGHT_THUMB_DOWN, TouchBinding.GAMEPAD_RIGHT_THUMB_LEFT,
                    0.74f, 0.46f, 0.95f,
                ),
                btn(TouchBinding.GAMEPAD_BUTTON_X, 0.82f, 0.73f),
                btn(TouchBinding.GAMEPAD_BUTTON_Y, 0.88f, 0.60f),
                btn(TouchBinding.GAMEPAD_BUTTON_A, 0.88f, 0.86f),
                btn(TouchBinding.GAMEPAD_BUTTON_B, 0.94f, 0.73f),
                // Shoulders & Triggers
                btn(TouchBinding.GAMEPAD_BUTTON_L2, 0.08f, 0.23f, ElementShape.ROUND_RECT, 1.0f),
                btn(TouchBinding.GAMEPAD_BUTTON_L1, 0.08f, 0.37f, ElementShape.ROUND_RECT, 1.0f),
                btn(TouchBinding.GAMEPAD_BUTTON_R2, 0.92f, 0.23f, ElementShape.ROUND_RECT, 1.0f),
                btn(TouchBinding.GAMEPAD_BUTTON_R1, 0.92f, 0.37f, ElementShape.ROUND_RECT, 1.0f),
                // Stick clicks & Start/Select
                btn(TouchBinding.GAMEPAD_BUTTON_L3, 0.24f, 0.89f, ElementShape.CIRCLE, 0.8f),
                btn(TouchBinding.GAMEPAD_BUTTON_R3, 0.76f, 0.89f, ElementShape.CIRCLE, 0.8f),
                btn(TouchBinding.GAMEPAD_BUTTON_SELECT, 0.44f, 0.90f, ElementShape.ROUND_RECT, 0.85f),
                btn(TouchBinding.GAMEPAD_BUTTON_START, 0.56f, 0.90f, ElementShape.ROUND_RECT, 0.85f),
            ),
        ),
        // 2: FPS (WASD + Mouse Look)
        ControlsProfile(
            id = 2,
            name = "FPS (WASD + Mouse)",
            cursorSpeed = 1.2f,
            elements = mutableListOf(
                pad4(
                    ElementType.STICK,
                    TouchBinding.KEY_W, TouchBinding.KEY_D, TouchBinding.KEY_S, TouchBinding.KEY_A,
                    0.13f, 0.72f, 1.1f,
                ),
                pad4(
                    ElementType.TRACKPAD,
                    TouchBinding.MOUSE_MOVE_UP, TouchBinding.MOUSE_MOVE_RIGHT,
                    TouchBinding.MOUSE_MOVE_DOWN, TouchBinding.MOUSE_MOVE_LEFT,
                    0.75f, 0.54f, 1.15f,
                ),
                btn(TouchBinding.MOUSE_LEFT_BUTTON, 0.91f, 0.66f, ElementShape.CIRCLE, 1.1f, text = "FIRE"),
                btn(TouchBinding.MOUSE_RIGHT_BUTTON, 0.91f, 0.44f, ElementShape.CIRCLE, 0.95f, text = "AIM"),
                btn(TouchBinding.KEY_SPACE, 0.87f, 0.86f, ElementShape.ROUND_RECT, 0.95f, text = "JUMP"),
                btn(TouchBinding.KEY_R, 0.78f, 0.86f, ElementShape.CIRCLE, 0.85f),
                btn(TouchBinding.KEY_E, 0.69f, 0.86f, ElementShape.CIRCLE, 0.85f),
                btn(TouchBinding.KEY_SHIFT_L, 0.08f, 0.42f, ElementShape.ROUND_RECT, 0.9f, toggle = true, text = "RUN"),
                btn(TouchBinding.KEY_CTRL_L, 0.24f, 0.86f, ElementShape.CIRCLE, 0.85f, toggle = true, text = "CRCH"),
                btn(TouchBinding.KEY_TAB, 0.43f, 0.90f, ElementShape.ROUND_RECT, 0.8f),
                btn(TouchBinding.KEY_ESC, 0.57f, 0.90f, ElementShape.ROUND_RECT, 0.8f),
                ControlElement(
                    type = ElementType.RANGE_BUTTON,
                    shape = ElementShape.RECT,
                    scale = 0.8f,
                    x = 0.50f,
                    y = 0.16f,
                    orientation = 0,
                    range = ElementRange.FROM_0_TO_9,
                    visibleCount = 5,
                ),
            ),
        ),
        // 3: RTS
        ControlsProfile(
            id = 3,
            name = "RTS",
            cursorSpeed = 1.0f,
            elements = mutableListOf(
                pad4(
                    ElementType.D_PAD,
                    TouchBinding.KEY_UP, TouchBinding.KEY_RIGHT, TouchBinding.KEY_DOWN, TouchBinding.KEY_LEFT,
                    0.12f, 0.74f, 1.0f,
                ),
                btn(TouchBinding.MOUSE_LEFT_BUTTON, 0.92f, 0.76f, ElementShape.ROUND_RECT, 1.0f),
                btn(TouchBinding.MOUSE_RIGHT_BUTTON, 0.92f, 0.58f, ElementShape.ROUND_RECT, 1.0f),
                btn(TouchBinding.KEY_SHIFT_L, 0.08f, 0.34f, ElementShape.SQUARE, 0.9f, toggle = true),
                btn(TouchBinding.KEY_CTRL_L, 0.08f, 0.48f, ElementShape.SQUARE, 0.9f, toggle = true),
                btn(TouchBinding.KEY_ALT_L, 0.92f, 0.40f, ElementShape.SQUARE, 0.9f, toggle = true),
                btn(TouchBinding.KEY_ESC, 0.42f, 0.90f, ElementShape.ROUND_RECT, 0.85f),
                btn(TouchBinding.KEY_TAB, 0.52f, 0.90f, ElementShape.ROUND_RECT, 0.85f),
                btn(TouchBinding.KEY_DEL, 0.62f, 0.90f, ElementShape.ROUND_RECT, 0.85f),
                ControlElement(
                    type = ElementType.RANGE_BUTTON,
                    shape = ElementShape.RECT,
                    scale = 0.8f,
                    x = 0.50f,
                    y = 0.16f,
                    orientation = 0,
                    range = ElementRange.FROM_0_TO_9,
                    visibleCount = 6,
                ),
            ),
        ),
        // 4: Template (12 Buttons)
        ControlsProfile(
            id = 4,
            name = "Template (12 Buttons)",
            cursorSpeed = 1.0f,
            elements = mutableListOf(
                pad4(
                    ElementType.D_PAD,
                    TouchBinding.KEY_W, TouchBinding.KEY_D, TouchBinding.KEY_S, TouchBinding.KEY_A,
                    0.13f, 0.68f, 1.0f,
                ),
                btn(TouchBinding.NONE, 0.79f, 0.72f),
                btn(TouchBinding.NONE, 0.86f, 0.58f),
                btn(TouchBinding.NONE, 0.86f, 0.86f),
                btn(TouchBinding.NONE, 0.93f, 0.72f),
                btn(TouchBinding.NONE, 0.08f, 0.25f, ElementShape.ROUND_RECT),
                btn(TouchBinding.NONE, 0.08f, 0.39f, ElementShape.ROUND_RECT),
                btn(TouchBinding.NONE, 0.92f, 0.25f, ElementShape.ROUND_RECT),
                btn(TouchBinding.NONE, 0.92f, 0.39f, ElementShape.ROUND_RECT),
                btn(TouchBinding.NONE, 0.44f, 0.90f, ElementShape.ROUND_RECT, 0.85f),
                btn(TouchBinding.NONE, 0.56f, 0.90f, ElementShape.ROUND_RECT, 0.85f),
                btn(TouchBinding.NONE, 0.25f, 0.88f, ElementShape.CIRCLE, 0.85f),
                btn(TouchBinding.NONE, 0.75f, 0.88f, ElementShape.CIRCLE, 0.85f),
            ),
        ),
    )
}
