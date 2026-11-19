package com.shreknx.game

import android.content.ContentResolver
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import java.io.File

// Папка игры: проверка содержимого, путь из выбора папки системным диалогом (SAF) и копирование оттуда в
// каталог приложения («Импорт») — для копирования разрешение на все файлы не нужно.
object GameFolder {
    // имя без учёта регистра (игра с ПК, раздел телефона различает регистр; vfs.c ищет так же)
    fun child(dir: File, name: String): File? =
        dir.listFiles()?.firstOrNull { it.name.equals(name, ignoreCase = true) }

    // null — всё на месте, иначе — чего не хватает
    fun check(path: String): String? {
        val dir = File(path)
        if (!dir.isDirectory) return L.t("Папка не найдена: $path", "Folder not found: $path")
        if (dir.listFiles() == null) return L.t("Нет доступа к папке (дай разрешение на файлы или используй «Импорт»)", "No access to the folder (grant file access or use “Import”)")
        val sys = child(dir, "System") ?: return L.t("В папке нет System — нужна папка установленной игры с ПК", "No System folder — select the folder of the game installed on a PC")
        for (f in listOf("Core.u", "Engine.u")) if (child(sys, f) == null) return L.t("В System нет $f", "System has no $f")
        if (child(dir, "Maps") == null) return L.t("В папке нет Maps", "No Maps folder")
        return null
    }

    // content://…/tree/primary:Games/Shrek2 -> /storage/emulated/0/Games/Shrek2 (карта памяти — /storage/XXXX-XXXX)
    fun pathOfTree(uri: Uri): String? {
        val id = try { DocumentsContract.getTreeDocumentId(uri) } catch (e: Exception) { return null }
        val colon = id.indexOf(':')
        if (colon < 0) return null
        val vol = id.substring(0, colon)
        val rel = id.substring(colon + 1)
        val base = if (vol.equals("primary", ignoreCase = true)) Environment.getExternalStorageDirectory().absolutePath
                   else "/storage/$vol"
        return if (rel.isEmpty()) base else "$base/$rel"
    }

    class Progress(var files: Int = 0, var bytes: Long = 0, var current: String = "")

    // рекурсивное копирование дерева SAF в dst; cancel() — прервать; onProgress зовётся из потока копирования
    fun import(cr: ContentResolver, tree: Uri, dst: File, cancel: () -> Boolean, onProgress: (Progress) -> Unit) {
        val p = Progress()
        val root = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        copyDir(cr, tree, DocumentsContract.getDocumentId(root), dst, p, cancel, onProgress)
    }

    private fun copyDir(cr: ContentResolver, tree: Uri, docId: String, dst: File, p: Progress,
                        cancel: () -> Boolean, onProgress: (Progress) -> Unit) {
        dst.mkdirs()
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
        val cols = arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                           DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE)
        val entries = ArrayList<Triple<String, String, Boolean>>()
        cr.query(children, cols, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                entries += Triple(c.getString(0), c.getString(1), c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR)
            }
        }
        for ((id, name, isDir) in entries) {
            if (cancel()) return
            val out = File(dst, name)
            if (isDir) {
                copyDir(cr, tree, id, out, p, cancel, onProgress)
                continue
            }
            p.current = name
            onProgress(p)
            val uri = DocumentsContract.buildDocumentUriUsingTree(tree, id)
            val tmp = File(dst, "$name.part")
            cr.openInputStream(uri)?.use { input ->
                tmp.outputStream().use { o ->
                    val buf = ByteArray(1 shl 20)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        o.write(buf, 0, n)
                        p.bytes += n
                        if (cancel()) return
                    }
                }
            }
            tmp.renameTo(out)
            p.files++
        }
    }
}
