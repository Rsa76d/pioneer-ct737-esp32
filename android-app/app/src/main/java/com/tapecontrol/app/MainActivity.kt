package com.tapecontrol.app

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.getValue
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.tapecontrol.app.ble.LinkState
import com.tapecontrol.app.ui.TapeScreen
import com.tapecontrol.app.ui.TapeTheme

class MainActivity : ComponentActivity() {

    private val vm: TapeViewModel by viewModels()

    private val bleSupported by lazy {
        packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)
    }

    // Результат запиту дозволів Bluetooth
    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
            if (result.values.all { it }) {
                continueConnectFlow()
            } else {
                vm.logError("Немає дозволів Bluetooth. Надайте їх у налаштуваннях застосунку")
            }
        }

    // Результат запиту на увімкнення Bluetooth
    private val enableBtLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == RESULT_OK) {
                continueConnectFlow()
            } else {
                vm.logError("Bluetooth не увімкнено")
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
        )
        super.onCreate(savedInstanceState)

        setContent {
            TapeTheme {
                val link by vm.link.collectAsStateWithLifecycle()
                val deviceName by vm.deviceName.collectAsStateWithLifecycle()
                val status by vm.status.collectAsStateWithLifecycle()
                val log by vm.log.collectAsStateWithLifecycle()

                TapeScreen(
                    link = link,
                    deviceName = deviceName,
                    status = status,
                    log = log,
                    bleSupported = bleSupported,
                    onConnectClick = ::onConnectClick,
                    onCommand = vm::send,
                )
            }
        }
    }

    /** Кнопка CONNECT / CANCEL / DISCONNECT. */
    private fun onConnectClick() {
        if (vm.link.value != LinkState.DISCONNECTED) {
            vm.disconnect()
            return
        }
        if (!bleSupported) {
            vm.logError("Bluetooth LE не підтримується на цьому пристрої")
            return
        }
        continueConnectFlow()
    }

    /** Послідовно перевіряємо: дозволи → Bluetooth увімкнено → (Android ≤ 11) геолокація → підключення. */
    private fun continueConnectFlow() {
        val missing = requiredPermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            permissionLauncher.launch(missing.toTypedArray())
            return
        }

        val adapter = (getSystemService(BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
        if (adapter == null) {
            vm.logError("Bluetooth недоступний на цьому пристрої")
            return
        }
        if (!adapter.isEnabled) {
            enableBtLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            return
        }

        // Android 11 і старіше: без увімкненої геолокації сканування мовчки нічого не знаходить
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            val lm = getSystemService(LOCATION_SERVICE) as? LocationManager
            if (lm != null && !LocationManagerCompat.isLocationEnabled(lm)) {
                vm.logError("Увімкніть геолокацію: Android до 12 вимагає її для пошуку BLE")
                return
            }
        }
        vm.connect()
    }

    private fun requiredPermissions(): List<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            listOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
}
