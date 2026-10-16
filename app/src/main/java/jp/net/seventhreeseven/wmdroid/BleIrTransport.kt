package jp.net.seventhreeseven.wmdroid

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import android.util.Log
import java.io.ByteArrayOutputStream
import java.util.UUID

/**
 * Nearby phones over Bluetooth LE: a room of PocketStylus phones within radio range, standing in
 * for IR ports facing each other (docs/research-irda.md, "Bluetooth LE").
 *
 * Every phone runs a GATT server with the PocketStylus IR service, advertises it with its node id
 * (manufacturer data 0xFFFF, 4 bytes), and scans for the others. Of two phones that see each other,
 * the one with the lower node id connects (GATT client); the link then carries frames both ways:
 * client -> server as writes without response to RX, server -> client as notifications on TX.
 * Frames are split into GATT-sized pieces with a one-byte header: 0x80 first piece, 0x40 last piece,
 * low six bits the message type (1 an IrLAP frame, 2 hello: the sender's node id, 4 bytes LE).
 * A frame from Windows Mobile goes to every linked phone; frames are not forwarded further.
 */
@SuppressLint("MissingPermission")  // InfraredBridge starts this only with the Nearby devices permission
class BleIrTransport(private val ctx: Context, private val ir: InfraredSettings,
                     private val listener: IrTransport.Listener) : IrTransport {
    companion object {
        private const val TAG = "WMDroidIrBle"
        val SERVICE: UUID = UUID.fromString("7e5a1000-2577-4b52-a9c1-504f434b4554")
        val RX: UUID = UUID.fromString("7e5a1001-2577-4b52-a9c1-504f434b4554")  // written by the client
        val TX: UUID = UUID.fromString("7e5a1002-2577-4b52-a9c1-504f434b4554")  // notified by the server
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val MANUFACTURER = 0xFFFF  // "no company": test/internal use
        const val T_FRAME = 1
        const val T_HELLO = 2
    }

    private val manager = ctx.getSystemService(BluetoothManager::class.java)
    private val myNode = ir.nodeId
    private var server: BluetoothGattServer? = null
    private var txChar: BluetoothGattCharacteristic? = null
    @Volatile private var running = false
    private val links = HashMap<String, Link>()  // by device address
    private val retry = android.os.Handler(android.os.Looper.getMainLooper())

    private inner class Link(val device: BluetoothDevice, val client: Boolean) {
        var node = 0
        var gatt: BluetoothGatt? = null
        var rx: BluetoothGattCharacteristic? = null
        var ready = false
        var mtu = 23
        var busy = false
        val queue = ArrayDeque<ByteArray>()
        val reasm = ByteArrayOutputStream()
        var reasmType = 0
        val id get() = if (node != 0) node else device.address.hashCode() or 1
    }

    override fun start() {
        val adapter = manager?.adapter
        if (adapter == null || !adapter.isEnabled) { listener.onState(false, "Nearby: Bluetooth is off"); return }
        if (!InfraredSettings.hasBlePermissions(ctx)) { listener.onState(false, "Nearby: allow Nearby devices in Settings > Infrared"); return }
        running = true
        try {
            openServer()
            advertise(true)
            scan(true)
            report()
        } catch (e: Exception) {
            Log.w(TAG, "start: $e")
            listener.onState(false, "Nearby: ${e.message}")
            stop()
        }
    }

    override fun stop() {
        running = false
        runCatching { advertise(false) }
        runCatching { scan(false) }
        val all = synchronized(this) { links.values.toList().also { links.clear() } }
        for (l in all) {
            runCatching { l.gatt?.disconnect(); l.gatt?.close() }
            runCatching { if (!l.client) server?.cancelConnection(l.device) }
            listener.onPeerGone(l.id)
        }
        runCatching { server?.close() }
        server = null
        listener.onState(false, "Nearby: off")
    }

    override fun send(frame: ByteArray) {
        val targets = synchronized(this) { links.values.filter { it.ready } }
        for (l in targets) enqueue(l, T_FRAME, frame)
    }

    private fun report() {
        val n = synchronized(this) { links.values.count { it.ready } }
        listener.onState(running, "Nearby: " + when (n) { 0 -> "looking for PocketStylus phones"; 1 -> "1 phone in range"; else -> "$n phones in range" })
    }

    // ---- Advertising and scanning ----

    private val advCallback = object : AdvertiseCallback() {
        override fun onStartFailure(errorCode: Int) { Log.w(TAG, "advertise failed $errorCode") }
    }

    private fun advertise(on: Boolean) {
        val adv = manager.adapter.bluetoothLeAdvertiser ?: return
        if (!on) { adv.stopAdvertising(advCallback); return }
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM)
            .setConnectable(true)
            .build()
        val node = byteArrayOf(myNode.toByte(), (myNode shr 8).toByte(), (myNode shr 16).toByte(), (myNode shr 24).toByte())
        val data = AdvertiseData.Builder()
            .addServiceUuid(ParcelUuid(SERVICE))
            .addManufacturerData(MANUFACTURER, node)
            .setIncludeDeviceName(false)
            .build()
        adv.startAdvertising(settings, data, advCallback)
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) = found(result)
        override fun onBatchScanResults(results: MutableList<ScanResult>) = results.forEach { found(it) }
        override fun onScanFailed(errorCode: Int) { Log.w(TAG, "scan failed $errorCode") }
    }

    private fun scan(on: Boolean) {
        val sc = manager.adapter.bluetoothLeScanner ?: return
        if (!on) { sc.stopScan(scanCallback); return }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        sc.startScan(listOf(filter), settings, scanCallback)
    }

    private fun found(r: ScanResult) {
        if (!running) return
        val m = r.scanRecord?.getManufacturerSpecificData(MANUFACTURER) ?: return
        if (m.size < 4) return
        val node = (m[0].toInt() and 255) or ((m[1].toInt() and 255) shl 8) or ((m[2].toInt() and 255) shl 16) or ((m[3].toInt() and 255) shl 24)
        if (node == myNode || myNode > node) return  // the lower id connects; the other waits for it
        synchronized(this) {
            if (links.values.any { it.node == node } || links.containsKey(r.device.address)) return
            val l = Link(r.device, client = true)
            l.node = node
            links[r.device.address] = l
            Log.i(TAG, "connecting to node $node")
            l.gatt = r.device.connectGatt(ctx, false, clientCallback, BluetoothDevice.TRANSPORT_LE)
        }
    }

    // ---- GATT server: links from phones with a lower node id ----

    private fun openServer() {
        val s = manager.openGattServer(ctx, serverCallback) ?: throw IllegalStateException("no GATT server")
        val svc = BluetoothGattService(SERVICE, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        val rx = BluetoothGattCharacteristic(RX,
            BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE or BluetoothGattCharacteristic.PROPERTY_WRITE,
            BluetoothGattCharacteristic.PERMISSION_WRITE)
        val tx = BluetoothGattCharacteristic(TX, BluetoothGattCharacteristic.PROPERTY_NOTIFY, BluetoothGattCharacteristic.PERMISSION_READ)
        tx.addDescriptor(BluetoothGattDescriptor(CCCD, BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE))
        svc.addCharacteristic(rx)
        svc.addCharacteristic(tx)
        s.addService(svc)
        server = s
        txChar = tx
    }

    private val serverCallback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                synchronized(this@BleIrTransport) { if (!links.containsKey(device.address)) links[device.address] = Link(device, client = false) }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                dropLink(device.address)
            }
        }

        override fun onMtuChanged(device: BluetoothDevice, mtu: Int) {
            synchronized(this@BleIrTransport) { links[device.address]?.mtu = mtu }
        }

        override fun onCharacteristicWriteRequest(device: BluetoothDevice, requestId: Int, c: BluetoothGattCharacteristic,
                                                  prepared: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray?) {
            if (responseNeeded) server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
            val l = synchronized(this@BleIrTransport) { links[device.address] } ?: return
            if (c.uuid == RX && value != null) received(l, value)
        }

        override fun onDescriptorWriteRequest(device: BluetoothDevice, requestId: Int, d: BluetoothGattDescriptor,
                                              prepared: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray?) {
            if (responseNeeded) server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
            val l = synchronized(this@BleIrTransport) { links[device.address] } ?: return
            if (d.uuid == CCCD) linkUp(l)  // the client listens for notifications now
        }

        override fun onNotificationSent(device: BluetoothDevice, status: Int) {
            val l = synchronized(this@BleIrTransport) { links[device.address] } ?: return
            synchronized(l) { l.busy = false }
            pump(l)
        }
    }

    // ---- GATT client: links to phones with a higher node id ----

    private val clientCallback = object : BluetoothGattCallback() {
        private fun link(g: BluetoothGatt) = synchronized(this@BleIrTransport) { links[g.device.address] }

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED && running) {
                g.requestMtu(517)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                g.close()
                dropLink(g.device.address)
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            link(g)?.mtu = if (status == BluetoothGatt.GATT_SUCCESS) mtu else 23
            g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)  // 7.5-15 ms intervals
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val l = link(g) ?: return
            val svc = g.getService(SERVICE)
            val rx = svc?.getCharacteristic(RX)
            val tx = svc?.getCharacteristic(TX)
            if (rx == null || tx == null) { g.disconnect(); return }
            l.rx = rx
            g.setCharacteristicNotification(tx, true)
            val cccd = tx.getDescriptor(CCCD) ?: run { g.disconnect(); return }
            if (Build.VERSION.SDK_INT >= 33) g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            else @Suppress("DEPRECATION") run { cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; g.writeDescriptor(cccd) }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            val l = link(g) ?: return
            if (status == BluetoothGatt.GATT_SUCCESS) linkUp(l) else g.disconnect()
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            link(g)?.let { received(it, value) }
        }

        @Deprecated("Android 12 and older")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION") val v = c.value ?: return
            if (Build.VERSION.SDK_INT < 33) link(g)?.let { received(it, v) }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            val l = link(g) ?: return
            synchronized(l) { l.busy = false }
            pump(l)
        }
    }

    // ---- Links ----

    private fun linkUp(l: Link) {
        synchronized(l) { l.ready = true }
        enqueue(l, T_HELLO, byteArrayOf(myNode.toByte(), (myNode shr 8).toByte(), (myNode shr 16).toByte(), (myNode shr 24).toByte()))
        Log.i(TAG, "link up with ${l.device.address} (${if (l.client) "client" else "server"})")
        report()
    }

    private fun dropLink(address: String) {
        val l = synchronized(this) { links.remove(address) } ?: return
        Log.i(TAG, "link down with $address")
        listener.onPeerGone(l.id)
        report()
    }

    private fun received(l: Link, piece: ByteArray) {
        if (piece.isEmpty()) return
        val h = piece[0].toInt() and 255
        var whole: ByteArray? = null
        var type = 0
        synchronized(l) {
            if (h and 0x80 != 0) { l.reasm.reset(); l.reasmType = h and 0x3F }
            l.reasm.write(piece, 1, piece.size - 1)
            if (l.reasm.size() > 8192) { l.reasm.reset(); return }
            if (h and 0x40 != 0) { whole = l.reasm.toByteArray(); type = l.reasmType; l.reasm.reset() }
        }
        val m = whole ?: return
        when (type) {
            T_HELLO -> if (m.size >= 4) {
                l.node = (m[0].toInt() and 255) or ((m[1].toInt() and 255) shl 8) or ((m[2].toInt() and 255) shl 16) or ((m[3].toInt() and 255) shl 24)
                report()
            }
            T_FRAME -> listener.onFrame(l.id, m)
        }
    }

    // Splits a message into GATT-sized pieces behind the link's queue.
    private fun enqueue(l: Link, type: Int, data: ByteArray) {
        synchronized(l) {
            if (l.queue.size > 256) return  // a stuck link drops frames, like a blocked IR beam
            val chunk = (l.mtu - 3 - 1).coerceAtLeast(19)
            var off = 0
            do {
                val n = minOf(chunk, data.size - off)
                var h = type
                if (off == 0) h = h or 0x80
                if (off + n >= data.size) h = h or 0x40
                l.queue.addLast(byteArrayOf(h.toByte()) + data.copyOfRange(off, off + n))
                off += n
            } while (off < data.size)
        }
        pump(l)
    }

    // One GATT operation in flight per link; the callbacks start the next.
    private fun pump(l: Link) {
        val piece: ByteArray
        synchronized(l) {
            if (l.busy || !l.ready || l.queue.isEmpty()) return
            piece = l.queue.removeFirst()
            l.busy = true
        }
        val ok = if (l.client) {
            val g = l.gatt
            val rx = l.rx
            if (g == null || rx == null) false
            else if (Build.VERSION.SDK_INT >= 33)
                g.writeCharacteristic(rx, piece, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) == BluetoothGatt.GATT_SUCCESS
            else @Suppress("DEPRECATION") run {
                rx.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                rx.value = piece
                g.writeCharacteristic(rx)
            }
        } else {
            val s = server
            val tx = txChar
            if (s == null || tx == null) false
            else if (Build.VERSION.SDK_INT >= 33)
                s.notifyCharacteristicChanged(l.device, tx, false, piece) == BluetoothGatt.GATT_SUCCESS
            else @Suppress("DEPRECATION") run {
                tx.value = piece
                s.notifyCharacteristicChanged(l.device, tx, false)
            }
        }
        if (!ok) {
            synchronized(l) { l.busy = false; l.queue.addFirst(piece) }
            if (running) retry.postDelayed({ pump(l) }, 20)  // the Bluetooth stack is busy: try again shortly
        }
    }
}
