package com.tapecontrol.app

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.tapecontrol.app.ble.LinkState
import com.tapecontrol.app.ble.LogType
import com.tapecontrol.app.ble.TapeBleClient
import com.tapecontrol.app.ble.TapeStatus
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.time.LocalTime
import java.time.format.DateTimeFormatter

/** Рядок логу на екрані. */
data class LogEntry(val id: Long, val time: String, val msg: String, val type: LogType)

/**
 * ViewModel тримає BLE-клієнт: з'єднання не переривається при повороті екрана.
 */
class TapeViewModel(app: Application) : AndroidViewModel(app) {

    private val _log = MutableStateFlow<List<LogEntry>>(emptyList())
    val log: StateFlow<List<LogEntry>> = _log.asStateFlow()

    private var nextId = 0L
    private val timeFormat = DateTimeFormatter.ofPattern("HH:mm:ss")

    private val client = TapeBleClient(app, viewModelScope) { msg, type -> addLog(msg, type) }

    val link: StateFlow<LinkState> = client.link
    val deviceName: StateFlow<String?> = client.deviceName
    val status: StateFlow<TapeStatus> = client.status

    fun connect() = client.connect()
    fun disconnect() = client.disconnect()
    fun send(cmd: String) = client.send(cmd)
    fun logError(msg: String) = addLog(msg, LogType.ERR)

    private fun addLog(msg: String, type: LogType) {
        val entry = LogEntry(nextId++, LocalTime.now().format(timeFormat), msg, type)
        _log.value = (_log.value + entry).takeLast(MAX_LOG)
    }

    override fun onCleared() {
        client.release()
    }

    private companion object {
        const val MAX_LOG = 100
    }
}
