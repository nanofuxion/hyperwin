package dev.hyperwin.app

import android.content.Context
import android.content.res.ColorStateList
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.widget.TextViewCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.card.MaterialCardView
import com.google.android.material.color.MaterialColors
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.slider.Slider
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import com.google.android.material.R as M

/** Small builders for the Material 3 screens, which are laid out in code like the rest of the app. */
internal object Ui {
    const val MATCH = ViewGroup.LayoutParams.MATCH_PARENT
    const val WRAP = ViewGroup.LayoutParams.WRAP_CONTENT

    fun Context.dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    fun View.themeColor(attr: Int) = MaterialColors.getColor(this, attr)

    fun text(ctx: Context, appearance: Int, colorAttr: Int = M.attr.colorOnSurface, value: CharSequence = "") =
        TextView(ctx).apply {
            val resolved = TypedValue().also { ctx.theme.resolveAttribute(appearance, it, true) }
            TextViewCompat.setTextAppearance(this, resolved.resourceId)
            setTextColor(themeColor(colorAttr))
            text = value
        }

    fun column(ctx: Context, vararg children: View) = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        children.forEach { addView(it, LinearLayout.LayoutParams(MATCH, WRAP)) }
    }

    /** A view with space above it, for stacking in a [column]. */
    fun <T : View> T.spaced(top: Int, width: Int = MATCH): T = apply {
        layoutParams = LinearLayout.LayoutParams(width, WRAP).apply { topMargin = context.dp(top) }
    }

    fun button(ctx: Context, label: String, style: Int = M.attr.materialButtonStyle, icon: Int = 0, onClick: () -> Unit) =
        MaterialButton(ctx, null, style).apply {
            text = label
            if (icon != 0) setIconResource(icon)
            minimumHeight = ctx.dp(52)
            setOnClickListener { onClick() }
        }

    fun iconButton(ctx: Context, icon: Int, description: String, onClick: () -> Unit) =
        MaterialButton(ctx, null, M.attr.materialIconButtonStyle).apply {
            setIconResource(icon)
            contentDescription = description
            tooltipText = description
            setOnClickListener { onClick() }
        }

    /** A titled group of rows in one rounded container, the way the system Settings app groups them. */
    fun section(ctx: Context, title: String, vararg rows: View): LinearLayout {
        val card = MaterialCardView(ctx, null, M.attr.materialCardViewFilledStyle).apply {
            radius = ctx.dp(24).toFloat()
            setCardBackgroundColor(themeColor(M.attr.colorSurfaceContainer))
            addView(column(ctx, *rows).apply { setPadding(0, ctx.dp(4), 0, ctx.dp(4)) })
        }
        return column(ctx,
            text(ctx, M.attr.textAppearanceLabelLarge, M.attr.colorPrimary, title).apply {
                setPadding(ctx.dp(20), ctx.dp(20), ctx.dp(20), ctx.dp(8))
            },
            card,
        )
    }

    /** Title and supporting line on the left, an optional control on the right. */
    fun row(ctx: Context, title: String, summary: String = "", trailing: View? = null, onClick: (() -> Unit)? = null): LinearLayout {
        val summaryView = text(ctx, M.attr.textAppearanceBodyMedium, M.attr.colorOnSurfaceVariant, summary).apply {
            visibility = if (summary.isEmpty()) View.GONE else View.VISIBLE
            tag = "summary"
        }
        return LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = ctx.dp(64)
            setPadding(ctx.dp(20), ctx.dp(10), ctx.dp(16), ctx.dp(10))
            addView(column(ctx, text(ctx, M.attr.textAppearanceBodyLarge, value = title), summaryView),
                LinearLayout.LayoutParams(0, WRAP, 1f).apply { marginEnd = ctx.dp(12) })
            if (trailing != null) addView(trailing, LinearLayout.LayoutParams(WRAP, WRAP))
            if (onClick != null) {
                val ripple = TypedValue().also { ctx.theme.resolveAttribute(android.R.attr.selectableItemBackground, it, true) }
                setBackgroundResource(ripple.resourceId)
                isClickable = true
                isFocusable = true
                setOnClickListener { onClick() }
            }
        }
    }

    fun LinearLayout.setSummary(value: String) {
        findViewWithTag<TextView>("summary")?.apply {
            text = value
            visibility = if (value.isEmpty()) View.GONE else View.VISIBLE
        }
    }

    fun switch(ctx: Context, checked: Boolean, onChange: (Boolean) -> Unit) = MaterialSwitch(ctx).apply {
        isChecked = checked
        setOnCheckedChangeListener { _, on -> onChange(on) }
    }

    /** Row that toggles its switch wherever it is tapped. */
    fun switchRow(ctx: Context, title: String, summary: String, checked: Boolean, onChange: (Boolean) -> Unit): LinearLayout {
        val toggle = switch(ctx, checked, onChange)
        return row(ctx, title, summary, toggle) { toggle.toggle() }.apply {
            contentDescription = "$title. $summary"
        }
    }

    fun chevron(ctx: Context) = ImageView(ctx).apply {
        setImageResource(R.drawable.ic_chevron)
        imageTintList = ColorStateList.valueOf(themeColor(M.attr.colorOnSurfaceVariant))
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    /** Segmented buttons: exactly one of [labels] is chosen. */
    fun segmented(ctx: Context, labels: List<String>, selected: Int, onChange: (Int) -> Unit) =
        MaterialButtonToggleGroup(ctx).apply {
            isSingleSelection = true
            isSelectionRequired = true
            val ids = labels.map { label ->
                MaterialButton(ctx, null, M.attr.materialButtonOutlinedStyle).apply {
                    id = View.generateViewId()
                    text = label
                }.also { addView(it, LinearLayout.LayoutParams(WRAP, WRAP)) }.id
            }
            check(ids[selected.coerceIn(0, ids.lastIndex)])
            addOnButtonCheckedListener { _, id, isChecked -> if (isChecked) onChange(ids.indexOf(id)) }
        }

    fun slider(ctx: Context, from: Float, to: Float, step: Float, value: Float, label: (Float) -> String, onChange: (Float) -> Unit) =
        Slider(ctx).apply {
            valueFrom = from
            valueTo = maxOf(to, from + step)
            stepSize = step
            this.value = (Math.round((value.coerceIn(valueFrom, valueTo) - from) / step) * step + from).coerceIn(valueFrom, valueTo)
            setLabelFormatter { label(it) }
            addOnChangeListener { _, v, _ -> onChange(v) }
        }

    fun field(ctx: Context, hint: String, value: String, inputType: Int): Pair<TextInputLayout, TextInputEditText> {
        val layout = TextInputLayout(ctx, null, M.attr.textInputOutlinedStyle).apply { this.hint = hint }
        val edit = TextInputEditText(layout.context).apply {
            setText(value)
            this.inputType = inputType
            setSingleLine()
        }
        layout.addView(edit, LinearLayout.LayoutParams(MATCH, WRAP))
        return layout to edit
    }

    fun FrameLayout.fill(view: View) = addView(view, FrameLayout.LayoutParams(MATCH, MATCH))
}
