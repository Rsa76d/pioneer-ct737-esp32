package com.tapecontrol.app.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import java.util.UUID

/** UUID-и сервісу й характеристик прошивки ESP32 (такі самі, як у веб-версії). */
object TapeUuids {
    val SERVICE: UUID = UUID.fromString("12345678-1234-1234-1234-123456789abc")
    val COMMAND: UUID = UUID.fromString("12345678-1234-1234-1234-123456789abd")
    val STATUS: UUID = UUID.fromString("12345678-1234-1234-1234-123456789abe")
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
}

const val DEVICE_NAME = "TapeControl"

enum class LinkState { DISCONNECTED, SCANNING, CONNECTING, CONNECTED }

enum class LogType { INFO, OK, ERR }

/**
 * Повністю нативний BLE-клієнт.
 *
 * Усі виклики й колбеки виконуються в головному потоці (connectGatt отримує головний Handler,
 * а scope — це viewModelScope), тому окремої синхронізації не потрібно.
 *
 * Що покращено порівняно з веб-версією:
 *  - автоматичне перепідключення при обриві зв'язку (до 5 спроб);
 *  - черга команд: GATT дозволяє лише одну операцію за раз, швидкі натискання не губляться;
 *  - запит більшого MTU і складання JSON зі шматків — стан не губиться;
 *  - початковий стан читається одразу після підключення;
 *  - таймаути на сканування, підключення й налаштування, коректне закриття GATT;
 *  - скасування підключення, що триває, тією ж кнопкою.
 */
@SuppressLint("MissingPermission")
class TapeBleClient(
    context: Context,
    private val scope: CoroutineScope,
    private val log: (String, LogType) -> Unit,
) {
    private val appContext = context.applicationContext
    private val adapter: BluetoothAdapter? =
        (appContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
    private val handler = Handler(Looper.getMainLooper())

    private val _link = MutableStateFlow(LinkState.DISCONNECTED)
    val link: StateFlow<LinkState> = _link.asStateFlow()

    private val _deviceName = MutableStateFlow<String?>(null)
    val deviceName: StateFlow<String?> = _deviceName.asStateFlow()

    private val _status = MutableStateFlow(TapeStatus())
    val status: StateFlow<TapeStatus> = _status.asStateFlow()

    private var gatt: BluetoothGatt? = null
    private var cmdChar: BluetoothGattCharacteristic? = null
    private var stsChar: BluetoothGattCharacteristic? = null

    /** Номер поточного GATT-з'єднання: колбеки від старих з'єднань ігноруються. */
    private var session = 0
    private var wantConnection = false
    private var attempt = 0
    private var lastAddress: String? = null
    private var discoveryStarted = false
    private var scanning = false

    private val assembler = StatusAssembler()
    private val commands = Channel<String>(COMMAND_QUEUE_SIZE)
    private var pendingWrite: CompletableDeferred<Int>? = null

    // ---------- таймери ----------

    private val scanTimeout = Runnable {
        if (!scanning) return@Runnable
        stopScan()
        log("Пристрій $DEVICE_NAME не знайдено", LogType.ERR)
        if (attempt > 0) scheduleReconnect() else finishDisconnected()
    }
    private val connectTimeout = Runnable { linkLost("Таймаут підключення") }
    private val discoverFallback = Runnable { gatt?.let { startDiscovery(it) } }
    private val readTimeout = Runnable { gatt?.let { markConnected(it) } }
    private val reconnectRunnable = Runnable { reconnectNow() }

    init {
        // Єдиний «писар»: бере команди по черзі й чекає підтвердження запису
        scope.launch {
            for (cmd in commands) {
                val g = gatt
                val ch = cmdChar
                if (g == null || ch == null || _link.value != LinkState.CONNECTED) continue
                val ok = writeCommand(g, ch, cmd)
                if (ok) {
                    log(cmd.uppercase(), LogType.OK)
                } else if (_link.value == LinkState.CONNECTED) {
                    log("Помилка запису: $cmd", LogType.ERR)
                }
            }
        }
    }

    // ---------- публічне API ----------

    fun connect() {
        if (_link.value != LinkState.DISCONNECTED) return
        val ad = adapter
        if (ad == null || !ad.isEnabled) {
            log("Bluetooth вимкнено", LogType.ERR)
            return
        }
        wantConnection = true
        attempt = 0
        startScan()
    }

    /** Розрив за бажанням користувача (або скасування підключення, що триває). */
    fun disconnect() {
        if (_link.value == LinkState.DISCONNECTED && !wantConnection) return
        val wasConnected = _link.value == LinkState.CONNECTED
        wantConnection = false
        teardown()
        setDisconnectedState()
        if (wasConnected) log("Відключено", LogType.ERR) else log("Скасовано", LogType.INFO)
    }

    fun send(cmd: String) {
        if (_link.value != LinkState.CONNECTED) {
            log("Не підключено!", LogType.ERR)
            return
        }
        if (commands.trySend(cmd).isFailure) log("Черга команд переповнена", LogType.ERR)
    }

    fun release() {
        wantConnection = false
        teardown()
        commands.close()
    }

    // ---------- сканування ----------

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!scanning || !matches(result)) return
            stopScan()
            if (attempt == 0) log("Підключення...", LogType.INFO)
            openGatt(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            if (!scanning) return
            stopScan()
            abort("Помилка сканування (код $errorCode)")
        }
    }

    private fun matches(r: ScanResult): Boolean {
        val name = r.scanRecord?.deviceName ?: runCatching { r.device.name }.getOrNull()
        if (name != null && name.equals(DEVICE_NAME, ignoreCase = true)) return true
        return r.scanRecord?.serviceUuids?.any { it.uuid == TapeUuids.SERVICE } == true
    }

    private fun startScan() {
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            abort("Bluetooth недоступний")
            return
        }
        setLink(LinkState.SCANNING)
        log("Пошук...", LogType.INFO)
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            scanner.startScan(null, settings, scanCallback)
            scanning = true
        } catch (e: Exception) {
            abort("Помилка сканування: ${e.message}")
            return
        }
        handler.postDelayed(scanTimeout, SCAN_TIMEOUT_MS)
    }

    private fun stopScan() {
        handler.removeCallbacks(scanTimeout)
        if (!scanning) return
        scanning = false
        try {
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (e: Exception) {
        }
    }

    // ---------- GATT-з'єднання ----------

    private fun openGatt(device: BluetoothDevice) {
        gatt?.let { releaseGatt(it) }
        gatt = null
        cmdChar = null
        stsChar = null
        discoveryStarted = false
        assembler.reset()

        setLink(LinkState.CONNECTING)
        val id = ++session
        handler.removeCallbacks(connectTimeout)
        handler.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)

        val g = try {
            device.connectGatt(
                appContext,
                false,
                GattCallback(id),
                BluetoothDevice.TRANSPORT_LE,
                BluetoothDevice.PHY_LE_1M_MASK,
                handler,
            )
        } catch (e: Exception) {
            null
        }
        if (g == null) {
            linkLost("Не вдалося почати підключення")
            return
        }
        gatt = g
    }

    private inner class GattCallback(private val id: Int) : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (id != session) {
                runCatching { g.close() }
                return
            }
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                lastAddress = g.device.address
                // Далі — налаштування (MTU → сервіси → сповіщення): даємо йому окремий таймаут
                handler.removeCallbacks(connectTimeout)
                handler.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)
                if (g.requestMtu(REQUEST_MTU)) {
                    // якщо відповіді на MTU не буде — все одно йдемо далі
                    handler.postDelayed(discoverFallback, MTU_FALLBACK_MS)
                } else {
                    startDiscovery(g)
                }
            } else {
                val reason = if (status == BluetoothGatt.GATT_SUCCESS) {
                    "Пристрій розірвав з'єднання"
                } else {
                    "Помилка підключення (код $status)"
                }
                linkLost(reason)
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (id != session) return
            handler.removeCallbacks(discoverFallback)
            startDiscovery(g)
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (id != session) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                linkLost("Помилка пошуку сервісів (код $status)")
                return
            }
            val svc = g.getService(TapeUuids.SERVICE)
            val cmd = svc?.getCharacteristic(TapeUuids.COMMAND)
            val sts = svc?.getCharacteristic(TapeUuids.STATUS)
            if (svc == null || cmd == null || sts == null) {
                // це проблема прошивки, повторювати спроби безглуздо
                abort("Сервіс TapeControl не знайдено на пристрої")
                return
            }
            cmdChar = cmd
            stsChar = sts
            enableNotifications(g, sts)
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            if (id != session || descriptor.uuid != TapeUuids.CCCD) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                abort("Не вдалося увімкнути сповіщення (код $status)")
                return
            }
            afterNotifications(g)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (id != session) return
            pendingWrite?.complete(status)
        }

        // --- читання (нова й стара сигнатури) ---

        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            if (id != session) return
            handleRead(characteristic, value, status)
        }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (id != session || Build.VERSION.SDK_INT >= 33) return
            handleRead(characteristic, characteristic.value, status)
        }

        // --- сповіщення (нова й стара сигнатури) ---

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (id != session || characteristic.uuid != TapeUuids.STATUS) return
            applyStatus(value)
        }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            if (id != session || Build.VERSION.SDK_INT >= 33) return
            if (characteristic.uuid == TapeUuids.STATUS) applyStatus(characteristic.value)
        }
    }

    private fun startDiscovery(g: BluetoothGatt) {
        if (discoveryStarted) return
        discoveryStarted = true
        handler.removeCallbacks(discoverFallback)
        if (!g.discoverServices()) linkLost("Не вдалося почати пошук сервісів")
    }

    private fun enableNotifications(g: BluetoothGatt, sts: BluetoothGattCharacteristic) {
        if (!g.setCharacteristicNotification(sts, true)) {
            abort("Не вдалося увімкнути сповіщення")
            return
        }
        val cccd = sts.getDescriptor(TapeUuids.CCCD)
        if (cccd == null) {
            afterNotifications(g)
            return
        }
        val props = sts.properties
        val useIndicate = (props and BluetoothGattCharacteristic.PROPERTY_NOTIFY) == 0 &&
            (props and BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0
        val value = if (useIndicate) {
            BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
        } else {
            BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        }
        if (!writeDescriptor(g, cccd, value)) abort("Не вдалося увімкнути сповіщення")
    }

    /** Після підписки читаємо поточний стан (якщо дозволено), щоб індикатори були актуальні одразу. */
    private fun afterNotifications(g: BluetoothGatt) {
        val sts = stsChar ?: return
        if ((sts.properties and BluetoothGattCharacteristic.PROPERTY_READ) != 0) {
            handler.postDelayed(readTimeout, READ_TIMEOUT_MS)
            if (!g.readCharacteristic(sts)) {
                handler.removeCallbacks(readTimeout)
                markConnected(g)
            }
        } else {
            markConnected(g)
        }
    }

    private fun handleRead(ch: BluetoothGattCharacteristic, value: ByteArray?, status: Int) {
        if (ch.uuid != TapeUuids.STATUS) return
        handler.removeCallbacks(readTimeout)
        if (status == BluetoothGatt.GATT_SUCCESS) applyStatus(value)
        gatt?.let { markConnected(it) }
    }

    private fun applyStatus(bytes: ByteArray?) {
        assembler.feed(bytes).forEach { _status.value = it }
    }

    private fun markConnected(g: BluetoothGatt) {
        if (_link.value == LinkState.CONNECTED) return
        handler.removeCallbacks(connectTimeout)
        handler.removeCallbacks(readTimeout)
        attempt = 0
        _deviceName.value = runCatching { g.device.name }.getOrNull() ?: DEVICE_NAME
        _link.value = LinkState.CONNECTED
        log("Підключено!", LogType.OK)
    }

    // ---------- запис команд ----------

    private suspend fun writeCommand(
        g: BluetoothGatt,
        ch: BluetoothGattCharacteristic,
        cmd: String,
    ): Boolean {
        val props = ch.properties
        val noResponse = (props and BluetoothGattCharacteristic.PROPERTY_WRITE) == 0 &&
            (props and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0
        val type = if (noResponse) {
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        } else {
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        }
        val deferred = CompletableDeferred<Int>()
        pendingWrite = deferred
        if (!writeCharacteristic(g, ch, cmd.toByteArray(Charsets.UTF_8), type)) {
            pendingWrite = null
            return false
        }
        val status = withTimeoutOrNull(if (noResponse) 500L else WRITE_TIMEOUT_MS) { deferred.await() }
        pendingWrite = null
        if (noResponse) delay(20) // маленька пауза між «безвідповідними» записами
        return status == BluetoothGatt.GATT_SUCCESS || (noResponse && status == null)
    }

    @Suppress("DEPRECATION")
    private fun writeCharacteristic(
        g: BluetoothGatt,
        ch: BluetoothGattCharacteristic,
        value: ByteArray,
        type: Int,
    ): Boolean =
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, value, type) == BluetoothStatusCodes.SUCCESS
        } else {
            ch.writeType = type
            ch.value = value
            g.writeCharacteristic(ch)
        }

    @Suppress("DEPRECATION")
    private fun writeDescriptor(
        g: BluetoothGatt,
        d: BluetoothGattDescriptor,
        value: ByteArray,
    ): Boolean =
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, value) == BluetoothStatusCodes.SUCCESS
        } else {
            d.value = value
            g.writeDescriptor(d)
        }

    // ---------- обрив, перепідключення, прибирання ----------

    /** З'єднання не вдалося або втрачене: або повторюємо спробу, або завершуємо. */
    private fun linkLost(reason: String) {
        val wasConnected = _link.value == LinkState.CONNECTED
        teardown()
        if (!wantConnection) {
            setDisconnectedState()
            return
        }
        log(if (wasConnected) "Зв'язок втрачено" else reason, LogType.ERR)
        scheduleReconnect()
    }

    private fun scheduleReconnect() {
        attempt++
        if (attempt > MAX_ATTEMPTS) {
            log("Не вдалося підключитись. Спробуйте ще раз", LogType.ERR)
            finishDisconnected()
            return
        }
        setLink(LinkState.CONNECTING)
        _status.value = TapeStatus()
        log("Повторне підключення $attempt/$MAX_ATTEMPTS...", LogType.INFO)
        handler.removeCallbacks(reconnectRunnable)
        handler.postDelayed(reconnectRunnable, minOf(1000L * attempt, 5000L))
    }

    private fun reconnectNow() {
        if (!wantConnection) return
        val ad = adapter
        if (ad == null || !ad.isEnabled) {
            abort("Bluetooth вимкнено")
            return
        }
        val addr = lastAddress
        if (addr != null && attempt <= DIRECT_RECONNECT_ATTEMPTS) {
            // Перші спроби — напряму за відомою адресою, без повторного сканування
            try {
                openGatt(ad.getRemoteDevice(addr))
            } catch (e: IllegalArgumentException) {
                startScan()
            }
        } else {
            startScan()
        }
    }

    /** Фатальна помилка: показуємо її й зупиняємось без повторів. */
    private fun abort(msg: String) {
        log(msg, LogType.ERR)
        finishDisconnected()
    }

    private fun finishDisconnected() {
        wantConnection = false
        teardown()
        setDisconnectedState()
    }

    /** Зупиняє сканування й таймери, закриває GATT; усі пізні колбеки стають «чужими». */
    private fun teardown() {
        stopScan()
        handler.removeCallbacks(connectTimeout)
        handler.removeCallbacks(discoverFallback)
        handler.removeCallbacks(readTimeout)
        handler.removeCallbacks(reconnectRunnable)
        session++
        val g = gatt
        gatt = null
        cmdChar = null
        stsChar = null
        discoveryStarted = false
        pendingWrite?.complete(-1)
        pendingWrite = null
        while (commands.tryReceive().isSuccess) { /* спорожнити чергу */ }
        if (g != null) releaseGatt(g)
    }

    /**
     * Спершу просимо розірвати з'єднання й лише потім закриваємо об'єкт:
     * миттєвий close() іноді лишає периферію «підключеною», і ESP32 перестає рекламуватись.
     */
    private fun releaseGatt(g: BluetoothGatt) {
        try {
            g.disconnect()
        } catch (e: Exception) {
        }
        handler.postDelayed({
            try {
                g.close()
            } catch (e: Exception) {
            }
        }, CLOSE_DELAY_MS)
    }

    private fun setLink(state: LinkState) {
        _link.value = state
    }

    private fun setDisconnectedState() {
        _link.value = LinkState.DISCONNECTED
        _deviceName.value = null
        _status.value = TapeStatus()
        assembler.reset()
    }

    private companion object {
        const val SCAN_TIMEOUT_MS = 15_000L
        const val CONNECT_TIMEOUT_MS = 15_000L
        const val MTU_FALLBACK_MS = 2_000L
        const val READ_TIMEOUT_MS = 3_000L
        const val WRITE_TIMEOUT_MS = 3_000L
        const val CLOSE_DELAY_MS = 600L
        const val REQUEST_MTU = 247
        const val MAX_ATTEMPTS = 5
        const val DIRECT_RECONNECT_ATTEMPTS = 3
        const val COMMAND_QUEUE_SIZE = 32
    }
}
