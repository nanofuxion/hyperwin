package dev.hyperwin.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.net.Uri
import android.os.StatFs
import android.provider.OpenableColumns
import android.text.InputType
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import java.io.File

/** Builds a machine's system.img from the user's ISO without running Windows Setup. */
object MachinePrep {
    const val PICK_ISO = 4101

    fun interface Progress {
        fun step(percent: Int, message: String)
    }

    private external fun nativePrepare(json: String, isoFd: Int, assets: String, progress: Progress): String

    init { System.loadLibrary("hyperwin") }

    fun prepare(activity: Activity, machine: Machine, progress: Progress): String {
        val uri = Uri.parse(machine.source)
        val assets = stageAssets(activity)
        machine.prepared = false
        machine.save()
        val report = activity.contentResolver.openFileDescriptor(uri, "r").use { pfd ->
            requireNotNull(pfd) { "Could not open ${machine.sourceName}" }
            nativePrepare(machine.json.absolutePath, pfd.fd, assets.absolutePath, progress)
        }
        if (report == "ok") {
            GuestBoot.requireAligned(machine.disk)
            machine.prepared = true
            machine.save()
            Machine.setActive(activity, machine)
            return "${machine.name} is ready. Boot Windows to start it."
        }
        return report
    }

    private fun stageAssets(activity: Activity): File {
        val dir = File(activity.cacheDir, "prep-assets").apply { deleteRecursively(); mkdirs() }
        fun copy(path: String, dest: File) {
            val children = activity.assets.list(path).orEmpty()
            if (children.isEmpty()) {
                dest.parentFile?.mkdirs()
                activity.assets.open(path).use { input -> dest.outputStream().use { input.copyTo(it) } }
            } else children.forEach { copy("$path/$it", File(dest, it)) }
        }
        copy("setup-esp.img", File(dir, "setup-esp.img"))
        if (activity.assets.list("drivers").orEmpty().isNotEmpty()) copy("drivers", File(dir, "drivers"))
        return dir
    }

    /** Settings dialog. The ISO picker result comes back through [onIsoPicked]. */
    class Dialog(private val activity: Activity, private val onPrepare: (Machine) -> Unit) {
        private val machine = Machine.active(activity)?.takeIf { !it.prepared } ?: Machine.create(activity)
        private val isoLabel = TextView(activity)

        fun show() {
            val cores = Runtime.getRuntime().availableProcessors()
            fun field(label: String, value: String, type: Int) = EditText(activity).apply {
                hint = label
                contentDescription = label
                setText(value)
                inputType = type
            }
            val number = InputType.TYPE_CLASS_NUMBER
            val name = field("Machine name", machine.name, InputType.TYPE_CLASS_TEXT)
            val disk = field("Disk size in GB (at least ${Machine.MIN_DISK_GB})", machine.diskSizeGb.toString(), number)
            val memory = field("Memory in MB (at least ${Machine.MIN_MEMORY_MB})", machine.memoryMb.toString(), number)
            val index = field("Windows edition index in the ISO", machine.imageIndex.toString(), number)
            val user = field("Windows user name", machine.username, InputType.TYPE_CLASS_TEXT)
            val password = field("Password (blank for none)", machine.password,
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
            val cpu = Spinner(activity).apply {
                contentDescription = "Processor cores"
                adapter = ArrayAdapter(activity, android.R.layout.simple_spinner_dropdown_item,
                    listOf("1 core", "All $cores cores"))
                setSelection(if (machine.cpuCount > 1) 1 else 0)
            }
            showIso()
            val pick = Button(activity).apply {
                text = "Choose Windows ISO"
                setOnClickListener {
                    activity.startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                        addCategory(Intent.CATEGORY_OPENABLE)
                        type = "*/*"
                        putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/x-iso9660-image", "application/octet-stream"))
                    }, PICK_ISO)
                }
            }
            val freeGb = StatFs(activity.filesDir.path).availableBytes shr 30
            val form = LinearLayout(activity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(48, 16, 48, 0)
                addView(pick)
                addView(isoLabel)
                addView(TextView(activity).apply { text = "Machine name" }); addView(name)
                addView(TextView(activity).apply { text = "Processor cores" }); addView(cpu)
                addView(TextView(activity).apply { text = "Memory (MB)" }); addView(memory)
                addView(TextView(activity).apply { text = "Disk size (GB). $freeGb GB free on this phone." }); addView(disk)
                addView(TextView(activity).apply { text = "Edition index" }); addView(index)
                addView(TextView(activity).apply { text = "User name" }); addView(user)
                addView(TextView(activity).apply { text = "Password" }); addView(password)
            }
            val dialog = AlertDialog.Builder(activity)
                .setTitle("Prepare machine")
                .setView(ScrollView(activity).apply { addView(form) })
                .setPositiveButton("Prepare", null)
                .setNegativeButton("Cancel", null)
                .create()
            dialog.setOnShowListener {
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                    val diskGb = disk.text.toString().toIntOrNull() ?: 0
                    val memoryMb = memory.text.toString().toIntOrNull() ?: 0
                    val problem = when {
                        machine.source.isEmpty() -> "Choose a Windows ISO first."
                        diskGb < Machine.MIN_DISK_GB -> "Disk size must be at least ${Machine.MIN_DISK_GB} GB."
                        memoryMb < Machine.MIN_MEMORY_MB -> "Memory must be at least ${Machine.MIN_MEMORY_MB} MB."
                        user.text.isBlank() -> "Enter a Windows user name."
                        else -> null
                    }
                    if (problem != null) {
                        isoLabel.text = problem
                        isoLabel.announceForAccessibility(problem)
                        return@setOnClickListener
                    }
                    machine.name = name.text.toString().ifBlank { "Windows" }
                    machine.diskSizeGb = diskGb
                    machine.memoryMb = memoryMb
                    machine.cpuCount = if (cpu.selectedItemPosition == 1) cores else 1
                    machine.imageIndex = index.text.toString().toIntOrNull()?.coerceAtLeast(1) ?: 1
                    machine.username = user.text.toString().trim()
                    machine.password = password.text.toString()
                    machine.save()
                    dialog.dismiss()
                    onPrepare(machine)
                }
            }
            dialog.show()
        }

        fun onIsoPicked(uri: Uri) {
            activity.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            machine.source = uri.toString()
            machine.sourceName = activity.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { if (it.moveToFirst()) it.getString(0) else null } ?: uri.lastPathSegment.orEmpty()
            machine.save()
            showIso()
        }

        private fun showIso() {
            isoLabel.text = if (machine.source.isEmpty()) "No ISO chosen yet." else "ISO: ${machine.sourceName}"
        }
    }
}
