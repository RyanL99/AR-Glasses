package com.example.smartglassesremote

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.content.Context
import java.io.OutputStream
import java.util.UUID

/**
 * Classic SPP client for the ESP32 glasses.
 */
object BtClient {

    // Standard SPP UUID
    private val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

    private val adapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()

    @Volatile private var socket: BluetoothSocket? = null
    @Volatile private var out: OutputStream? = null

    @Volatile var currentDevice: BluetoothDevice? = null
        private set

    fun isConnected(): Boolean = socket?.isConnected == true

    /** Connect by MAC address string. */
    @SuppressLint("MissingPermission")
    fun connect(
        context: Context,
        macAddress: String,
        onResult: (ok: Boolean, message: String) -> Unit
    ) {
        val bt = adapter
        if (bt == null) {
            onResult(false, "Bluetooth not available")
            return
        }
        val device = try {
            bt.getRemoteDevice(macAddress)
        } catch (t: Throwable) {
            onResult(false, "Invalid MAC: $macAddress")
            return
        }
        connect(context, device, onResult)
    }

    /** Connect by already-picked BluetoothDevice. */
    @SuppressLint("MissingPermission")
    fun connect(
        context: Context,
        device: BluetoothDevice,
        onResult: (ok: Boolean, message: String) -> Unit
    ) {
        if (isConnected()) {
            onResult(true, "Already connected to ${currentDevice?.name ?: "device"}")
            return
        }
        Thread {
            try {
                adapter?.cancelDiscovery()
                val sock = device.createRfcommSocketToServiceRecord(SPP_UUID)
                sock.connect()
                socket = sock
                out = sock.outputStream
                currentDevice = device
                onResult(true, "Connected to ${device.name ?: device.address}")
            } catch (t: Throwable) {
                try { socket?.close() } catch (_: Throwable) {}
                socket = null
                out = null
                currentDevice = null
                onResult(false, "Connect error: ${t.message ?: t.javaClass.simpleName}")
            }
        }.start()
    }

    /** Send a line terminated with '\n'. */
    fun sendLine(line: String) {
        val bytes = (line + "\n").toByteArray(Charsets.UTF_8)
        val o = out ?: return
        try {
            o.write(bytes)
            o.flush()
        } catch (_: Throwable) {
            // ignore; caller can check isConnected()
        }
    }

    /** Alias so older code using BtClient.send(...) still compiles. */
    fun send(line: String) = sendLine(line)

    fun disconnect() {
        try { out?.flush() } catch (_: Throwable) {}
        try { socket?.close() } catch (_: Throwable) {}
        out = null
        socket = null
        currentDevice = null
    }
}
