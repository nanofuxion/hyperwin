package dev.hyperwin.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.StatFs
import android.provider.OpenableColumns
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import com.google.android.material.button.MaterialButton
import com.google.android.material.progressindicator.LinearProgressIndicator
import dev.hyperwin.app.Ui.dp
import dev.hyperwin.app.Ui.setSummary
import dev.hyperwin.app.Ui.spaced
import dev.hyperwin.app.Ui.themeColor
import com.google.android.material.R as M

/**
 * First-run slides: choose the ISO, size the machine, name the account, then watch the disk being
 * built. [onPrepare] gets the finished settings; progress comes back through [showProgress].
 */
class SetupFlow(
    private val ctx: Context,
    private val pickIso: () -> Unit,
    private val onPrepare: (Machine) -> Unit,
) {
    val machine: Machine = Machine.all(ctx).firstOrNull { !it.prepared } ?: Machine.create(ctx)
    private val hostCores = Runtime.getRuntime().availableProcessors()
    private val freeGb = (StatFs(ctx.filesDir.path).availableBytes shr 30).toInt()
    private var step = 0
    private var working = false

    private val stepLabel = Ui.text(ctx, M.attr.textAppearanceLabelLarge, M.attr.colorPrimary).apply { letterSpacing = 0.12f }
    private val headline = Ui.text(ctx, M.attr.textAppearanceHeadlineMedium)
    private val body = Ui.text(ctx, M.attr.textAppearanceBodyLarge, M.attr.colorOnSurfaceVariant)
    private val problem = Ui.text(ctx, M.attr.textAppearanceBodyMedium, M.attr.colorError).apply {
        accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_ASSERTIVE
    }
    private val page = FrameLayout(ctx)
    private val bar = LinearProgressIndicator(ctx).apply { max = 100; trackCornerRadius = ctx.dp(2) }
    private val back: MaterialButton = Ui.button(ctx, "Back", M.attr.materialButtonOutlinedStyle) { go(step - 1) }
    private val next: MaterialButton = Ui.button(ctx, "Next") { advance() }

    // Page controls that outlive a page change.
    private var isoRow: LinearLayout? = null
    private val edition = Ui.field(ctx, "Windows edition number", machine.imageIndex.toString(), InputType.TYPE_CLASS_NUMBER)
    private val user = Ui.field(ctx, "User name", machine.username, InputType.TYPE_CLASS_TEXT)
    private val password = Ui.field(ctx, "Password (optional)", machine.password,
        InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
    private val progressBar = LinearProgressIndicator(ctx).apply { max = 100; trackCornerRadius = ctx.dp(4); trackThickness = ctx.dp(8) }
    private val progressPercent = Ui.text(ctx, M.attr.textAppearanceDisplayMedium, M.attr.colorPrimary, "0%")
    private val progressMessage = Ui.text(ctx, M.attr.textAppearanceBodyLarge, M.attr.colorOnSurfaceVariant, "Getting ready").apply {
        accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
    }

    private class Page(val title: String, val text: String, val next: String, val content: () -> View)

    private val pages = listOf(
        Page("Windows, on your phone",
            "HyperWin builds a Windows 11 disk from an ISO file you provide, then runs it right here. It takes about fifteen minutes, once.",
            "Get started") { welcome() },
        Page("Choose your Windows ISO",
            "Pick a Windows 11 ARM64 ISO file from this phone. HyperWin only reads it. Nothing is changed.",
            "Next") { isoPage() },
        Page("Give Windows some room",
            "More cores and memory make Windows faster. The disk only uses phone storage as Windows fills it.",
            "Next") { resources() },
        Page("Create your Windows account",
            "This is the account Windows signs in with. You can leave the password empty.",
            "Set up Windows") { account() },
        Page("Setting up Windows",
            "Keep HyperWin open. Windows starts by itself when this is done, and finishes its own setup over a few restarts.",
            "") { progress() },
    )

    val view: View = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(themeColor(M.attr.colorSurface))
        addView(bar, LinearLayout.LayoutParams(Ui.MATCH, Ui.WRAP))
        addView(LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(Ui.column(ctx, stepLabel, headline.spaced(8), body.spaced(12), problem.spaced(12)).apply {
                gravity = Gravity.CENTER_VERTICAL
                setPadding(ctx.dp(32), ctx.dp(16), ctx.dp(24), ctx.dp(16))
            }, LinearLayout.LayoutParams(0, Ui.MATCH, 0.42f))
            addView(ScrollView(ctx).apply {
                isFillViewport = true
                addView(page.apply { setPadding(ctx.dp(8), ctx.dp(16), ctx.dp(32), ctx.dp(16)) })
            }, LinearLayout.LayoutParams(0, Ui.MATCH, 0.58f))
        }, LinearLayout.LayoutParams(Ui.MATCH, 0, 1f))
        addView(LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(ctx.dp(32), ctx.dp(8), ctx.dp(32), ctx.dp(16))
            addView(back, LinearLayout.LayoutParams(Ui.WRAP, Ui.WRAP))
            addView(View(ctx), LinearLayout.LayoutParams(0, 1, 1f))
            addView(next, LinearLayout.LayoutParams(Ui.WRAP, Ui.WRAP))
        }, LinearLayout.LayoutParams(Ui.MATCH, Ui.WRAP))
    }

    init {
        go(0)
    }

    /** True when Back was used inside the flow. */
    fun back(): Boolean {
        if (working || step == 0) return working
        go(step - 1)
        return true
    }

    private fun go(to: Int) {
        step = to.coerceIn(0, pages.lastIndex)
        val p = pages[step]
        stepLabel.text = when (step) {
            0 -> "WELCOME"
            pages.lastIndex -> "ALMOST THERE"
            else -> "STEP $step OF ${pages.size - 2}"
        }
        headline.text = p.title
        body.text = p.text
        problem.text = ""
        page.removeAllViews()
        page.addView(p.content(), FrameLayout.LayoutParams(Ui.MATCH, Ui.WRAP, Gravity.CENTER_VERTICAL))
        bar.setProgressCompat(step * 100 / pages.lastIndex, true)
        back.visibility = if (step in 1 until pages.lastIndex) View.VISIBLE else View.INVISIBLE
        next.visibility = if (step == pages.lastIndex) View.INVISIBLE else View.VISIBLE
        next.text = p.next
        headline.announceForAccessibility(p.title)
    }

    private fun advance() {
        val error = when (step) {
            1 -> if (machine.source.isEmpty()) "Choose a Windows ISO file first." else null
            3 -> if (user.second.text.isNullOrBlank()) "Enter a user name." else null
            else -> null
        }
        if (error != null) {
            problem.text = error
            return
        }
        machine.imageIndex = edition.second.text.toString().toIntOrNull()?.coerceAtLeast(1) ?: 1
        machine.username = user.second.text.toString().trim().ifEmpty { machine.username }
        machine.password = password.second.text.toString()
        machine.save()
        if (step == pages.lastIndex - 1) startPrepare() else go(step + 1)
    }

    private fun startPrepare() {
        working = true
        go(pages.lastIndex)
        showProgress(0, "Getting ready")
        onPrepare(machine)
    }

    /** Jumps straight to the progress slide, for a prepare started from outside the flow. */
    fun showWorking() {
        working = true
        go(pages.lastIndex)
    }

    fun showProgress(percent: Int, message: String) {
        progressBar.setProgressCompat(percent, true)
        progressPercent.text = "$percent%"
        progressMessage.text = message
    }

    /** The build failed: say why and let the user go back and try again. */
    fun showFailure(message: String) {
        working = false
        problem.text = message
        back.visibility = View.VISIBLE
        problem.announceForAccessibility(message)
    }

    fun onIsoPicked(uri: Uri) {
        runCatching { ctx.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
        machine.source = uri.toString()
        machine.sourceName = ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { if (it.moveToFirst()) it.getString(0) else null } ?: uri.lastPathSegment.orEmpty()
        machine.save()
        isoRow?.setSummary(machine.sourceName)
        problem.text = ""
    }

    private fun welcome() = Ui.section(ctx, "What happens next",
        Ui.row(ctx, "Choose your Windows ISO", "A Windows 11 ARM64 image you downloaded."),
        Ui.row(ctx, "Pick cores, memory and disk size", "You can change cores and memory later."),
        Ui.row(ctx, "Create your account", "Then HyperWin does the rest."),
    )

    private fun isoPage(): View {
        (edition.first.parent as? LinearLayout)?.removeView(edition.first)
        edition.first.helperText = "1 is right for most ISO files."
        val row = Ui.row(ctx, "Selected file", machine.sourceName.ifEmpty { "None yet" }).also { isoRow = it }
        return Ui.column(ctx,
            Ui.section(ctx, "ISO file", row),
            Ui.button(ctx, "Choose ISO file", M.attr.materialButtonOutlinedStyle, R.drawable.ic_folder) { pickIso() }.spaced(16, Ui.WRAP),
            edition.first.spaced(24),
        )
    }

    private fun gb(mb: Int) = if (mb % 1024 == 0) "${mb / 1024} GB" else "%.1f GB".format(mb / 1024f)

    private fun resources(): View {
        val memoryValue = Ui.text(ctx, M.attr.textAppearanceBodyLarge, M.attr.colorPrimary, gb(machine.memoryMb))
        // Whole 8 GB steps above the minimum, leaving a little of the phone's free space alone.
        val diskMax = Machine.MIN_DISK_GB + (minOf(256, maxOf(Machine.MIN_DISK_GB + 8, freeGb - 4)) - Machine.MIN_DISK_GB) / 8 * 8
        machine.diskSizeGb = machine.diskSizeGb.coerceIn(Machine.MIN_DISK_GB, diskMax)
        val diskValue = Ui.text(ctx, M.attr.textAppearanceBodyLarge, M.attr.colorPrimary, "${machine.diskSizeGb} GB")
        return Ui.section(ctx, "Machine",
            Ui.row(ctx, "Processor cores", "", Ui.segmented(ctx, listOf("1", "All $hostCores"), if (machine.cpuCount > 1) 1 else 0) {
                machine.cpuCount = if (it == 1) hostCores else 1
            }),
            Ui.row(ctx, "Memory", "", memoryValue).apply { minimumHeight = ctx.dp(40) },
            Ui.slider(ctx, Machine.MIN_MEMORY_MB.toFloat(), HomeScreen.maxMemoryMb(ctx).toFloat(), 512f, machine.memoryMb.toFloat(), { gb(it.toInt()) }) {
                machine.memoryMb = it.toInt()
                memoryValue.text = gb(machine.memoryMb)
            }.apply { contentDescription = "Memory for Windows"; setPadding(ctx.dp(12), 0, ctx.dp(12), 0) },
            Ui.row(ctx, "Disk size", "$freeGb GB free on this phone", diskValue).apply { minimumHeight = ctx.dp(40) },
            Ui.slider(ctx, Machine.MIN_DISK_GB.toFloat(), diskMax.toFloat(), 8f, machine.diskSizeGb.toFloat(), { "${it.toInt()} GB" }) {
                machine.diskSizeGb = it.toInt()
                diskValue.text = "${machine.diskSizeGb} GB"
            }.apply { contentDescription = "Disk size for Windows"; setPadding(ctx.dp(12), 0, ctx.dp(12), ctx.dp(8)) },
        )
    }

    private fun account(): View {
        listOf(user.first, password.first).forEach { (it.parent as? LinearLayout)?.removeView(it) }
        password.first.endIconMode = com.google.android.material.textfield.TextInputLayout.END_ICON_PASSWORD_TOGGLE
        return Ui.column(ctx, user.first, password.first.spaced(16))
    }

    private fun progress(): View {
        listOf(progressPercent, progressBar, progressMessage).forEach { (it.parent as? LinearLayout)?.removeView(it) }
        return Ui.column(ctx, progressPercent, progressBar.spaced(12), progressMessage.spaced(16))
    }
}
