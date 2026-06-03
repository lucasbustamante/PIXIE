package com.example.pixie

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.IOException
import java.util.UUID
import java.util.concurrent.Executors

class MainActivity : FlutterActivity(), EventChannel.StreamHandler {
    private val methodChannelName = "esp32cam_bt/methods"
    private val eventChannelName = "esp32cam_bt/events"
    private val requestCodeBluetoothPermissions = 1001
    private val sppUuid: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

    private val bluetoothAdapter: BluetoothAdapter? by lazy { BluetoothAdapter.getDefaultAdapter() }
    private val mainHandler = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor()

    private var eventSink: EventChannel.EventSink? = null
    private var pendingPermissionResult: MethodChannel.Result? = null
    private var receiverRegistered = false
    private var socket: BluetoothSocket? = null
    private var input: BufferedInputStream? = null
    private var output: BufferedOutputStream? = null
    private var readThread: Thread? = null
    @Volatile private var keepReading = false

    private val discoveryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                BluetoothDevice.ACTION_FOUND -> {
                    val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                    }
                    device?.let { emitDevice(it, false) }
                }
                BluetoothAdapter.ACTION_DISCOVERY_STARTED -> {
                    emitStatus("Localizando dispositivos Bluetooth...", scanning = true)
                }
                BluetoothAdapter.ACTION_DISCOVERY_FINISHED -> {
                    emitStatus("Busca finalizada", scanning = false)
                }
            }
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, methodChannelName)
            .setMethodCallHandler { call, result -> handleMethodCall(call, result) }

        EventChannel(flutterEngine.dartExecutor.binaryMessenger, eventChannelName)
            .setStreamHandler(this)
    }

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        eventSink = events
    }

    override fun onCancel(arguments: Any?) {
        eventSink = null
    }

    private fun handleMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "requestPermissions" -> requestBluetoothPermissions(result)
            "startDiscovery" -> startDiscovery(result)
            "stopDiscovery" -> stopDiscovery(result)
            "connect" -> connect(call.argument<String>("address"), result)
            "sendCommand" -> sendCommand(call.argument<String>("command"), result)
            "disconnect" -> disconnect(result)
            else -> result.notImplemented()
        }
    }

    private fun requestBluetoothPermissions(result: MethodChannel.Result) {
        val permissions = requiredBluetoothPermissions()
        val missingPermissions = permissions.filter { permission ->
            ContextCompat.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED
        }

        if (missingPermissions.isEmpty()) {
            result.success(true)
            return
        }

        pendingPermissionResult = result
        ActivityCompat.requestPermissions(
            this,
            missingPermissions.toTypedArray(),
            requestCodeBluetoothPermissions
        )
    }

    private fun requiredBluetoothPermissions(): Array<String> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            arrayOf(
                Manifest.permission.BLUETOOTH,
                Manifest.permission.BLUETOOTH_ADMIN,
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)

        if (requestCode == requestCodeBluetoothPermissions) {
            val allGranted = grantResults.isNotEmpty() &&
                grantResults.all { it == PackageManager.PERMISSION_GRANTED }
            pendingPermissionResult?.success(allGranted)
            pendingPermissionResult = null
        }
    }

    @SuppressLint("MissingPermission")
    private fun startDiscovery(result: MethodChannel.Result) {
        val adapter = bluetoothAdapter
        if (adapter == null) {
            emitError("Bluetooth nao disponivel neste aparelho")
            result.success(false)
            return
        }

        if (!adapter.isEnabled) {
            emitError("Bluetooth do celular esta desligado")
            result.success(false)
            return
        }

        if (!hasBluetoothPermissions()) {
            emitError("Permissoes Bluetooth nao concedidas")
            result.success(false)
            return
        }

        registerDiscoveryReceiverIfNeeded()

        if (adapter.isDiscovering) {
            adapter.cancelDiscovery()
        }

        // Mostra também dispositivos já pareados. O ESP32-CAM Classic/SPP pode não aparecer
        // em uma nova varredura se já estiver salvo no Bluetooth do Android.
        adapter.bondedDevices?.forEach { emitDevice(it, true) }

        val started = adapter.startDiscovery()
        emitStatus(
            if (started) "Localizando dispositivos Bluetooth..." else "Nao foi possivel iniciar a busca",
            scanning = started
        )
        result.success(started)
    }

    @SuppressLint("MissingPermission")
    private fun stopDiscovery(result: MethodChannel.Result) {
        bluetoothAdapter?.takeIf { it.isDiscovering }?.cancelDiscovery()
        emitStatus("Busca parada", scanning = false)
        result.success(true)
    }

    @SuppressLint("MissingPermission")
    private fun connect(address: String?, result: MethodChannel.Result) {
        if (address.isNullOrBlank()) {
            emitError("Endereco Bluetooth invalido")
            result.success(false)
            return
        }

        val adapter = bluetoothAdapter
        if (adapter == null || !adapter.isEnabled || !hasBluetoothPermissions()) {
            emitError("Bluetooth indisponivel ou sem permissao")
            result.success(false)
            return
        }

        emitStatus("Conectando ao ESP32-CAM...", connected = false)
        adapter.cancelDiscovery()

        executor.execute {
            try {
                closeConnectionOnly()
                val device = adapter.getRemoteDevice(address)
                val newSocket = device.createRfcommSocketToServiceRecord(sppUuid)
                newSocket.connect()
                socket = newSocket
                input = BufferedInputStream(newSocket.inputStream)
                output = BufferedOutputStream(newSocket.outputStream)
                keepReading = true
                startReadThread()

                mainHandler.post {
                    emitStatus("Conectado ao ${safeDeviceName(device)}", connected = true, scanning = false)
                    result.success(true)
                }
            } catch (e: Exception) {
                closeConnectionOnly()
                mainHandler.post {
                    emitError("Falha ao conectar: ${e.message ?: "erro desconhecido"}")
                    result.success(false)
                }
            }
        }
    }

    private fun sendCommand(command: String?, result: MethodChannel.Result) {
        val text = command?.trim().orEmpty()
        val out = output
        if (text.isBlank() || out == null || socket?.isConnected != true) {
            result.success(false)
            return
        }

        executor.execute {
            try {
                out.write((text + "\n").toByteArray(Charsets.UTF_8))
                out.flush()
                mainHandler.post { result.success(true) }
            } catch (e: IOException) {
                mainHandler.post {
                    emitError("Erro ao enviar comando: ${e.message ?: "falha de escrita"}")
                    result.success(false)
                }
            }
        }
    }

    private fun disconnect(result: MethodChannel.Result) {
        closeConnectionOnly()
        emitStatus("Desconectado", connected = false)
        result.success(true)
    }

    private fun startReadThread() {
        readThread = Thread {
            try {
                val stream = input ?: return@Thread
                while (keepReading) {
                    val header = readLine(stream) ?: break
                    if (header.startsWith("IMG ")) {
                        readImagePacket(stream, header)
                    } else if (header.startsWith("MSG ")) {
                        handleMessage(header.removePrefix("MSG ").trim())
                    } else if (header.isNotBlank()) {
                        emitStatus(header)
                    }
                }
            } catch (e: Exception) {
                if (keepReading) {
                    emitError("Conexao perdida: ${e.message ?: "erro de leitura"}")
                }
            } finally {
                closeConnectionOnly()
                emitStatus("Desconectado", connected = false)
            }
        }
        readThread?.start()
    }

    private fun readLine(stream: BufferedInputStream): String? {
        val buffer = StringBuilder()
        while (keepReading) {
            val value = stream.read()
            if (value == -1) return null
            val c = value.toChar()
            if (c == '\n') return buffer.toString().trim()
            if (c != '\r') buffer.append(c)
            if (buffer.length > 160) return buffer.toString().trim()
        }
        return null
    }

    private fun readImagePacket(stream: BufferedInputStream, header: String) {
        val parts = header.split(" ")
        if (parts.size < 3) return

        val kind = parts[1]
        val length = parts[2].toIntOrNull() ?: return
        if (length <= 0 || length > 400_000) {
            emitError("Imagem Bluetooth com tamanho invalido: $length")
            return
        }

        val bytes = ByteArray(length)
        var offset = 0
        while (offset < length && keepReading) {
            val read = stream.read(bytes, offset, length - offset)
            if (read == -1) throw IOException("fim inesperado dos dados da imagem")
            offset += read
        }

        emit(mapOf("type" to "frame", "kind" to kind, "bytes" to bytes))
    }

    private fun handleMessage(message: String) {
        when {
            message.startsWith("SAVED ") -> {
                val path = message.split(" ").getOrNull(1) ?: "microSD"
                emit(mapOf("type" to "saved", "path" to path))
            }
            message.startsWith("ERROR ") -> emitError(message.removePrefix("ERROR "))
            else -> emitStatus(message)
        }
    }

    @SuppressLint("MissingPermission")
    private fun emitDevice(device: BluetoothDevice, bonded: Boolean) {
        if (!hasBluetoothPermissions()) return
        emit(
            mapOf(
                "type" to "device",
                "name" to safeDeviceName(device),
                "address" to device.address,
                "bonded" to (bonded || device.bondState == BluetoothDevice.BOND_BONDED)
            )
        )
    }

    @SuppressLint("MissingPermission")
    private fun safeDeviceName(device: BluetoothDevice): String {
        return try {
            device.name ?: "Dispositivo sem nome"
        } catch (_: SecurityException) {
            "Dispositivo sem nome"
        }
    }

    private fun emitStatus(message: String, connected: Boolean? = null, scanning: Boolean? = null) {
        val event = mutableMapOf<String, Any>("type" to "status", "message" to message)
        connected?.let { event["connected"] = it }
        scanning?.let { event["scanning"] = it }
        emit(event)
    }

    private fun emitError(message: String) {
        emit(mapOf("type" to "error", "message" to message))
    }

    private fun emit(event: Map<String, Any>) {
        mainHandler.post { eventSink?.success(event) }
    }

    private fun hasBluetoothPermissions(): Boolean {
        return requiredBluetoothPermissions().all { permission ->
            ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun registerDiscoveryReceiverIfNeeded() {
        if (receiverRegistered) return
        val filter = IntentFilter().apply {
            addAction(BluetoothDevice.ACTION_FOUND)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_STARTED)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)
        }
        registerReceiver(discoveryReceiver, filter)
        receiverRegistered = true
    }

    private fun closeConnectionOnly() {
        keepReading = false
        try { input?.close() } catch (_: Exception) {}
        try { output?.close() } catch (_: Exception) {}
        try { socket?.close() } catch (_: Exception) {}
        input = null
        output = null
        socket = null
    }

    override fun onDestroy() {
        closeConnectionOnly()
        if (receiverRegistered) {
            try { unregisterReceiver(discoveryReceiver) } catch (_: Exception) {}
            receiverRegistered = false
        }
        executor.shutdownNow()
        super.onDestroy()
    }
}
