package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.telephony.SubscriptionManager
import android.telephony.TelephonyManager

/**
 * The phone's own number, for Windows Mobile's SIM ("My number"): the user's override in Phone calls
 * settings, else what Android knows. Carriers often leave it off the SIM, so it can be empty.
 */
object PhoneNumber {
    const val PERMISSION = Manifest.permission.READ_PHONE_NUMBERS

    fun hasPermission(ctx: Context) = ctx.checkSelfPermission(PERMISSION) == PackageManager.PERMISSION_GRANTED

    /** Digits and a leading +, or "" when unknown. */
    fun own(ctx: Context): String {
        val override = clean(Settings(ctx).myNumber)
        return if (override.isNotEmpty()) override else clean(fromPhone(ctx))
    }

    @SuppressLint("MissingPermission")
    private fun fromPhone(ctx: Context): String {
        if (!hasPermission(ctx)) return ""
        if (Build.VERSION.SDK_INT >= 33) {
            val number = runCatching {
                val sm = ctx.getSystemService(SubscriptionManager::class.java)
                sm.getPhoneNumber(SubscriptionManager.getDefaultSubscriptionId())
            }.getOrNull()
            if (!number.isNullOrBlank()) return number
        }
        @Suppress("DEPRECATION")
        return runCatching { ctx.getSystemService(TelephonyManager::class.java).line1Number }.getOrNull().orEmpty()
    }

    private fun clean(s: String): String {
        val t = s.trim()
        val digits = t.filter { it.isDigit() }
        return if (digits.isEmpty()) "" else if (t.startsWith("+")) "+$digits" else digits
    }
}
