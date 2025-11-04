package com.example.smartglassesremote

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.example.smartglassesremote.databinding.ActivityMainBinding
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.concurrent.thread

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val adapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()

    private val pairedNames = mutableListOf<String>()
    private val pairedDevices = mutableListOf<BluetoothDevice>()
    private lateinit var spinnerAdapter: ArrayAdapter<String>

    private val prefs by lazy { getSharedPreferences("sg_prefs", MODE_PRIVATE) }

    // Time sync (every minute when enabled)
    private val handler = Handler(Looper.getMainLooper())
    private val timeRunnable = object : Runnable {
        override fun run() {
            if (binding.swTimeSync.isChecked && BtClient.isConnected()) {
                pushPhoneTimeToEsp()
                // schedule again at the next minute boundary
                handler.postDelayed(this, 60_000L)
            }
        }
    }

    private val requestEnableBtLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { /* no-op */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        spinnerAdapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, pairedNames)
        binding.spPaired.adapter = spinnerAdapter

        // Restore toggles
        binding.swTimeSync.isChecked = prefs.getBoolean("time_sync", false)
        binding.swForwardNotifications.isChecked = prefs.getBoolean("forward_notifications", false)

        // Buttons
        binding.btnRefresh.setOnClickListener { refreshPaired() }
        binding.btnConnect.setOnClickListener { connectSelected() }
        binding.btnDisconnect.setOnClickListener { disconnectBt() }

        binding.btnSendText.setOnClickListener { sendText() }
        binding.btnClear.setOnClickListener { sendLine("#CLEAR\n") }
        binding.btnClockOn.setOnClickListener { sendLine("#CLOCKON\n") }
        binding.btnClockOff.setOnClickListener { sendLine("#CLOCKOFF\n") }

        binding.swTimeSync.setOnCheckedChangeListener { _, isChecked ->
            prefs.edit().putBoolean("time_sync", isChecked).apply()
            if (isChecked) {
                pushPhoneTimeToEsp()   // push immediately
                handler.removeCallbacks(timeRunnable)
                handler.postDelayed(timeRunnable, 60_000L)
            } else handler.removeCallbacks(timeRunnable)
        }

        binding.swForwardNotifications.setOnCheckedChangeListener { _, isChecked ->
            prefs.edit().putBoolean("forward_notifications", isChecked).apply()
            // no immediate action; NotifyService reads prefs when notifications arrive
        }

        binding.btnOpenNotifAccess.setOnClickListener {
            startActivity(Intent("android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS"))
            toast("Enable SmartGlasses Remote in the list.")
        }

        binding.btnPrev.setOnClickListener { mediaController()?.transportControls?.skipToPrevious() }
        binding.btnPlayPause.setOnClickListener { togglePlayPause() }
        binding.btnNext.setOnClickListener { mediaController()?.transportControls?.skipToNext() }
        binding.btnRefreshNowPlaying.setOnClickListener { refreshNowPlaying() }
        binding.btnSendNowPlaying.setOnClickListener { sendNowPlayingToGlasses() }

        checkPermissionsAndEnableBt()
        refreshPaired()
        refreshNowPlaying()
    }

    // === Permissions + BT ===
    private fun checkPermissionsAndEnableBt() {
        if (adapter == null) {
            toast("This device does not support Bluetooth")
            finish()
            return
        }
        if (!adapter.isEnabled) {
            val intent = Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)
            requestEnableBtLauncher.launch(intent)
        }

        val need = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (!hasPerm(Manifest.permission.BLUETOOTH_CONNECT)) need += Manifest.permission.BLUETOOTH_CONNECT
            if (!hasPerm(Manifest.permission.BLUETOOTH_SCAN)) need += Manifest.permission.BLUETOOTH_SCAN
        } else {
            if (!hasPerm(Manifest.permission.ACCESS_FINE_LOCATION)) need += Manifest.permission.ACCESS_FINE_LOCATION
        }
        if (need.isNotEmpty())
            ActivityCompat.requestPermissions(this, need.toTypedArray(), 42)
    }

    private fun hasPerm(p: String) =
        ContextCompat.checkSelfPermission(this, p) == PackageManager.PERMISSION_GRANTED

    // === Paired devices spinner ===
    @SuppressLint("MissingPermission")
    private fun refreshPaired() {
        pairedNames.clear(); pairedDevices.clear()
        val bonded = adapter?.bondedDevices ?: emptySet()
        if (bonded.isEmpty()) {
            pairedNames += "No paired devices"
        } else bonded.forEach { d ->
            pairedDevices += d
            pairedNames += "${d.name} (${d.address})"
        }
        spinnerAdapter.notifyDataSetChanged()
    }

    @SuppressLint("MissingPermission")
    private fun connectSelected() {
        if (pairedDevices.isEmpty()) {
            toast("Pair your ESP32 in Android Settings first.")
            return
        }
        val idx = binding.spPaired.selectedItemPosition
        if (idx !in pairedDevices.indices) { toast("Pick a device from the list"); return }
        val device = pairedDevices[idx]
        setStatus("Connecting to ${device.name} ...")
        BtClient.connect(this, device) { ok, msg ->
            runOnUiThread {
                setStatus(msg)
                if (ok && binding.swTimeSync.isChecked) {
                    pushPhoneTimeToEsp()
                    handler.removeCallbacks(timeRunnable)
                    handler.postDelayed(timeRunnable, 60_000L)
                }
            }
        }
    }

    private fun disconnectBt() {
        BtClient.disconnect()
        setStatus("Disconnected")
    }

    // === Sending ===
    private fun sendText() {
        val msg = binding.etText.text?.toString()?.trim().orEmpty()
        if (msg.isEmpty()) { toast("Enter text"); return }
        sendLine("#TEXT|$msg\n")
    }

    private fun sendNowPlayingToGlasses() {
        val np = binding.tvNowPlaying.text?.toString()?.trim().orEmpty()
        if (np.isNotEmpty() && np != "(no media)") {
            sendLine("#TEXT|$np\n")
        } else toast("No media info")
    }

    private fun sendLine(cmd: String) {
        if (!BtClient.isConnected()) {
            toast("Not connected")
            return
        }
        thread { BtClient.send(cmd) }
        setStatus("Sent: ${cmd.trim()}")
    }

    // === Time sync ===
    private fun pushPhoneTimeToEsp() {
        val now = Date()
        val hh = SimpleDateFormat("HH", Locale.getDefault()).format(now)
        val mm = SimpleDateFormat("mm", Locale.getDefault()).format(now)
        val dateLabel = SimpleDateFormat("EEE M/d", Locale.getDefault()).format(now)
        if (BtClient.isConnected()) {
            BtClient.send("#SETTIME|$hh|$mm\n")
            BtClient.send("#SETDATE|$dateLabel\n")
            setStatus("Time synced: $hh:$mm  $dateLabel")
        }
    }

    // === Now Playing ===
    private fun mediaController(): MediaController? {
        val cn = ComponentName(this, NotifyService::class.java)
        val msm = getSystemService(Context.MEDIA_SESSION_SERVICE) as MediaSessionManager
        val controllers = try { msm.getActiveSessions(cn) } catch (_: SecurityException) { emptyList() }
        return controllers.firstOrNull()
    }

    private fun refreshNowPlaying() {
        val c = mediaController()
        if (c == null) {
            binding.tvNowPlaying.text = "(no media - enable Notification Access)"
            return
        }
        val meta = c.metadata
        val state = c.playbackState
        val title = meta?.getString(android.media.MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = meta?.getString(android.media.MediaMetadata.METADATA_KEY_ARTIST) ?: ""
        val playing = state?.state == PlaybackState.STATE_PLAYING
        val prefix = if (playing) "▶" else "⏸"
        val line = if (title.isNotBlank())
            "$prefix $title${if (artist.isNotBlank()) " – $artist" else ""}"
        else "(no media)"
        binding.tvNowPlaying.text = line
    }

    private fun togglePlayPause() {
        val c = mediaController() ?: return
        val state = c.playbackState?.state
        if (state == PlaybackState.STATE_PLAYING) c.transportControls.pause()
        else c.transportControls.play()
        refreshNowPlaying()
    }

    // === UI helpers ===
    private fun setStatus(s: String) { binding.tvStatus.text = "Status: $s" }
    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()

    override fun onResume() {
        super.onResume()
        refreshNowPlaying()
    }

    override fun onDestroy() {
        super.onDestroy()
        handler.removeCallbacks(timeRunnable)
        // leave connection as is; user can disconnect manually
    }
}
