package com.tapecontrol.app.ble

import org.json.JSONObject

/** Стан магнітофона, який прилітає від ESP32 у вигляді JSON. */
data class TapeStatus(
    val rew: Boolean = false,
    val stp: Boolean = false,
    val ply: Boolean = false,
    val fwd: Boolean = false,
    val rec: Boolean = false,
    val dlb: Boolean = false,
    val mpx: Boolean = false,
    val hx: Boolean = false,
    val ir: Boolean = false,
)

/**
 * Збирає JSON зі шматків сповіщень.
 *
 * У веб-версії довге повідомлення, розбите на кілька пакетів (малий MTU),
 * мовчки губилось. Тут шматки накопичуються, поки не набереться
 * повний об'єкт {...}, і лише тоді він розбирається.
 */
class StatusAssembler {
    private val buf = StringBuilder()

    fun reset() = buf.setLength(0)

    fun feed(bytes: ByteArray?): List<TapeStatus> {
        if (bytes == null || bytes.isEmpty()) return emptyList()
        val chunk = String(bytes, Charsets.UTF_8)

        // Швидкий шлях: пакет уже є повним JSON
        parse(chunk.trim())?.let {
            buf.setLength(0)
            return listOf(it)
        }

        buf.append(chunk)
        if (buf.length > MAX_BUFFER) { // захист від сміття
            buf.setLength(0)
            return emptyList()
        }

        val result = mutableListOf<TapeStatus>()
        while (true) {
            val start = buf.indexOf("{")
            if (start < 0) {
                buf.setLength(0)
                break
            }
            if (start > 0) buf.delete(0, start)
            val end = findObjectEnd(buf)
            if (end < 0) break // чекаємо наступний шматок
            val json = buf.substring(0, end + 1)
            buf.delete(0, end + 1)
            parse(json)?.let { result.add(it) }
        }
        return result
    }

    private fun findObjectEnd(s: CharSequence): Int {
        var depth = 0
        var inStr = false
        var esc = false
        for (i in 0 until s.length) {
            val c = s[i]
            if (inStr) {
                when {
                    esc -> esc = false
                    c == '\\' -> esc = true
                    c == '"' -> inStr = false
                }
            } else {
                when (c) {
                    '"' -> inStr = true
                    '{' -> depth++
                    '}' -> {
                        depth--
                        if (depth == 0) return i
                    }
                }
            }
        }
        return -1
    }

    private fun parse(json: String): TapeStatus? {
        if (!json.startsWith("{")) return null
        return try {
            val o = JSONObject(json)
            TapeStatus(
                rew = flag(o, "rew"),
                stp = flag(o, "stp"),
                ply = flag(o, "ply"),
                fwd = flag(o, "fwd"),
                rec = flag(o, "rec"),
                dlb = flag(o, "dlb"),
                mpx = flag(o, "mpx"),
                hx = flag(o, "hx"),
                ir = flag(o, "ir"),
            )
        } catch (e: Exception) {
            null
        }
    }

    /** Значення може бути 0/1, true/false або рядком — приймаємо будь-яке. */
    private fun flag(o: JSONObject, key: String): Boolean =
        when (val v = o.opt(key)) {
            is Boolean -> v
            is Number -> v.toDouble() > 0.0
            is String -> v.equals("true", true) || (v.toDoubleOrNull() ?: 0.0) > 0.0
            else -> false
        }

    private companion object {
        const val MAX_BUFFER = 1024
    }
}
