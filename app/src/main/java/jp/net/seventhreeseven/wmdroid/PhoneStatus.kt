package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.telephony.ServiceState
import android.telephony.TelephonyCallback
import android.telephony.TelephonyDisplayInfo
import android.telephony.TelephonyManager
import java.util.concurrent.Executor

/**
 * Reads the phone's radio and Wi-Fi state and expresses it in Windows Mobile 6.5 terms.
 *
 * Data network types are rescaled so the best a modern phone can do maps onto the best icon
 * WM 6.5 has: GSM family 5G (incl. UWB / NR-advanced) -> H, LTE -> 3G, HSPA/UMTS -> E, 2G -> G;
 * CDMA family 5G/LTE/EV-DO -> EV, 1xRTT -> 1X.
 */
class PhoneStatus(private val ctx: Context) {
    data class Snapshot(
        val service: Boolean,     // registered on a network
        val signal: Int,          // 0..4 bars
        val operator: String,
        val roaming: Boolean,
        val data: String,         // WM icon name: H, 3G, E, G, EV, 1X or "" when no data
        val wifiOn: Boolean,
        val wifiConnected: Boolean,
        val wifiSignal: Int,      // 0..4
        val numeric: String = "", // MCC+MNC of the network (the virtual modem's +COPS numeric format)
        val wifiName: String = "", // SSID, empty when Wi-Fi is not the connection or the name is unavailable
    ) {
        /** Line sent to the guest helper. `ssid` is last: the name may contain spaces. */
        fun encode(): String {
            val line = "status svc=${if (service) 1 else 0} sig=$signal roam=${if (roaming) 1 else 0} " +
                "data=$data wifi=${if (wifiOn) 1 else 0} wificonn=${if (wifiConnected) 1 else 0} wifisig=$wifiSignal num=${numeric.ifEmpty { "-" }} op=$operator"
            return if (wifiName.isEmpty()) line else "$line ssid=$wifiName"
        }
    }

    private val tm = ctx.getSystemService(TelephonyManager::class.java)
    private val cm = ctx.getSystemService(ConnectivityManager::class.java)
    private val wm = ctx.applicationContext.getSystemService(WifiManager::class.java)
    @Volatile private var override5g = false
    @Volatile private var signalBars = 0
    private var callback: Any? = null

    fun hasPhonePermission() =
        ctx.checkSelfPermission(Manifest.permission.READ_PHONE_STATE) == PackageManager.PERMISSION_GRANTED

    /** Listens for signal strength and 5G display overrides (Android 12+). */
    fun start() {
        if (Build.VERSION.SDK_INT < 31 || callback != null) return
        val cb = object : TelephonyCallback(), TelephonyCallback.SignalStrengthsListener,
            TelephonyCallback.DisplayInfoListener {
            override fun onSignalStrengthsChanged(s: android.telephony.SignalStrength) { signalBars = bars(s) }
            override fun onDisplayInfoChanged(info: TelephonyDisplayInfo) {
                val o = info.overrideNetworkType
                override5g = o == TelephonyDisplayInfo.OVERRIDE_NETWORK_TYPE_NR_NSA ||
                    o == TelephonyDisplayInfo.OVERRIDE_NETWORK_TYPE_NR_ADVANCED
            }
        }
        runCatching {
            if (hasPhonePermission()) tm.registerTelephonyCallback(Executor { it.run() }, cb)
            else tm.registerTelephonyCallback(Executor { it.run() }, object : TelephonyCallback(), TelephonyCallback.SignalStrengthsListener {
                override fun onSignalStrengthsChanged(s: android.telephony.SignalStrength) { signalBars = bars(s) }
            })
            callback = cb
        }
    }

    fun stop() {
        if (Build.VERSION.SDK_INT >= 31) (callback as? TelephonyCallback)?.let { runCatching { tm.unregisterTelephonyCallback(it) } }
        callback = null
    }

    fun read(): Snapshot {
        val state: ServiceState? = if (hasPhonePermission()) runCatching { tm.serviceState }.getOrNull() else null
        val inService = state?.state == ServiceState.STATE_IN_SERVICE || (state == null && tm.networkOperatorName.isNotEmpty())
        val bars = if (Build.VERSION.SDK_INT >= 29) runCatching { tm.signalStrength?.let { bars(it) } }.getOrNull() ?: signalBars else signalBars
        val caps = cm.getNetworkCapabilities(cm.activeNetwork)
        val onWifi = caps?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
        val wifiBars = if (onWifi) stableWifi(wifiLevel()) else {
            wifiShown = -1
            wifiPendN = 0
            0
        }
        return Snapshot(
            service = inService,
            signal = if (inService) bars.coerceIn(0, 4) else 0,
            operator = tm.networkOperatorName.ifEmpty { tm.simOperatorName }.replace('\n', ' '),
            roaming = runCatching { tm.isNetworkRoaming }.getOrDefault(false),
            // No data icon without bars: WM would show a connection on a dead antenna.
            data = if (inService && bars > 0 && tm.dataState == TelephonyManager.DATA_CONNECTED) dataIcon() else "",
            wifiOn = runCatching { wm.isWifiEnabled }.getOrDefault(false),
            wifiConnected = onWifi,
            wifiSignal = wifiBars,
            numeric = runCatching { tm.networkOperator }.getOrNull().orEmpty().ifEmpty { runCatching { tm.simOperator }.getOrNull().orEmpty() },
            wifiName = wifiName(),
        )
    }

    /**
     * Bars on the same scale phone status bars use. AOSP's level is stricter than most OEM bars
     * (e.g. 5G at -113 dBm is 0 in AOSP but 2 bars on Samsung), so take the better of it and a
     * reading of the received power.
     */
    private fun bars(s: android.telephony.SignalStrength): Int {
        oemBars(s)?.let { return it }
        var best = s.level
        if (Build.VERSION.SDK_INT < 29) return best  // getCellSignalStrengths: API 29 (callers are 29+ today)
        for (c in s.cellSignalStrengths) {
            val dbm = c.dbm
            if (dbm == Int.MAX_VALUE || dbm == Int.MIN_VALUE || dbm >= 0) continue
            val lteLike = c is android.telephony.CellSignalStrengthNr || c is android.telephony.CellSignalStrengthLte
            val l = if (lteLike) when {
                dbm >= -90 -> 4; dbm >= -100 -> 3; dbm >= -115 -> 2; dbm >= -125 -> 1; else -> 0
            } else when {
                dbm >= -85 -> 4; dbm >= -95 -> 3; dbm >= -105 -> 2; dbm >= -110 -> 1; else -> 0
            }
            if (l > best) best = l
        }
        return best.coerceIn(0, 4)
    }

    /**
     * The bars the phone's own status bar shows, where the OEM exposes them. Samsung puts them in
     * SignalStrength as "SignalBarInfo{ nrLevel=2 }" (also lteLevel, wcdmaLevel, ...); there is no
     * public API for it, so it is read from the description.
     */
    private fun oemBars(s: android.telephony.SignalStrength): Int? {
        val text = s.toString()
        val at = text.indexOf("SignalBarInfo{")
        if (at < 0) return null
        val end = text.indexOf('}', at).let { if (it < 0) text.length else it }
        return Regex("""[A-Za-z]*[Ll]evel=(\d+)""").findAll(text.substring(at, end))
            .mapNotNull { it.groupValues[1].toIntOrNull() }.maxOrNull()?.coerceIn(0, 4)
    }

    /**
     * The joined network's name. Android quotes it ("My Wifi") and reports "<unknown ssid>" when
     * location permission is missing, which Windows Mobile should not show in place of "Network Card".
     */
    @Suppress("DEPRECATION")
    private fun wifiName(): String {
        val raw = runCatching {
            var fromCaps: String? = null
            if (Build.VERSION.SDK_INT >= 29) {
                for (net in cm.allNetworks) {
                    val caps = cm.getNetworkCapabilities(net) ?: continue
                    if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) continue
                    fromCaps = (caps.transportInfo as? WifiInfo)?.ssid
                    break
                }
            }
            fromCaps?.takeIf { it.isNotBlank() && it != "<unknown ssid>" }
                ?: wm.connectionInfo?.let { if (it.networkId != -1) it.ssid else null }
        }.getOrNull() ?: return ""
        var name = raw.trim()
        if (name.length >= 2 && name.startsWith("\"") && name.endsWith("\"")) name = name.substring(1, name.length - 1)
        name = name.replace('\n', ' ').replace('\r', ' ').trim()
        if (name.isEmpty() || name == "<unknown ssid>" || name == "0x") return ""
        return if (name.length > 32) name.substring(0, 32) else name
    }

    // RSSI sits on a bar boundary and flips 3/4 every read. Report a new bar only after it holds.
    private var wifiShown = -1
    private var wifiPend = -1
    private var wifiPendN = 0

    private fun stableWifi(level: Int): Int {
        if (wifiShown < 0 || level == wifiShown) {
            if (wifiShown < 0) wifiShown = level
            wifiPendN = 0
            return wifiShown
        }
        if (level != wifiPend) { wifiPend = level; wifiPendN = 1 } else wifiPendN++
        if (wifiPendN >= 3) { wifiShown = level; wifiPendN = 0 }
        return wifiShown
    }

    private fun wifiLevel(): Int {
        val rssi = runCatching {
            @Suppress("DEPRECATION")
            wm.connectionInfo.rssi
        }.getOrDefault(-100)
        return if (Build.VERSION.SDK_INT >= 30) wm.calculateSignalLevel(rssi).coerceIn(0, 4)
        else @Suppress("DEPRECATION") WifiManager.calculateSignalLevel(rssi, 5)
    }

    private fun dataIcon(): String {
        val type = if (hasPhonePermission()) runCatching { tm.dataNetworkType }.getOrDefault(TelephonyManager.NETWORK_TYPE_UNKNOWN)
        else TelephonyManager.NETWORK_TYPE_UNKNOWN
        val is5g = type == TelephonyManager.NETWORK_TYPE_NR || override5g
        // Design > Network Indicator Type: the letters of a GSM or a CDMA phone of the time.
        val kind = Settings(ctx).netIndicator
        val cdma = if (kind == "auto") tm.phoneType == TelephonyManager.PHONE_TYPE_CDMA else kind == "cdma"
        if (cdma) {
            return when (type) {
                TelephonyManager.NETWORK_TYPE_1xRTT, TelephonyManager.NETWORK_TYPE_CDMA, TelephonyManager.NETWORK_TYPE_GPRS,
                TelephonyManager.NETWORK_TYPE_EDGE, TelephonyManager.NETWORK_TYPE_GSM -> "1X"
                else -> "EV"  // EV-DO for anything faster
            }
        }
        if (is5g) return "H"
        return when (type) {
            TelephonyManager.NETWORK_TYPE_LTE, TelephonyManager.NETWORK_TYPE_IWLAN -> "3G"
            TelephonyManager.NETWORK_TYPE_HSPAP, TelephonyManager.NETWORK_TYPE_HSPA, TelephonyManager.NETWORK_TYPE_HSDPA,
            TelephonyManager.NETWORK_TYPE_HSUPA, TelephonyManager.NETWORK_TYPE_UMTS, TelephonyManager.NETWORK_TYPE_TD_SCDMA -> "E"
            TelephonyManager.NETWORK_TYPE_EDGE, TelephonyManager.NETWORK_TYPE_GPRS, TelephonyManager.NETWORK_TYPE_GSM -> "G"
            TelephonyManager.NETWORK_TYPE_UNKNOWN -> "3G"  // no permission: assume a modern connection
            else -> "3G"
        }
    }
}
