package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.app.admin.DeviceAdminReceiver
import android.app.admin.DevicePolicyManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent

/**
 * Device-admin receiver with the single "lock the screen" policy, so the phone can turn its
 * screen off (and lock) when Windows Mobile turns its display off.
 */
class LockAdmin : DeviceAdminReceiver() {
    companion object {
        private fun component(ctx: Context) = ComponentName(ctx, LockAdmin::class.java)
        private fun dpm(ctx: Context) = ctx.getSystemService(Context.DEVICE_POLICY_SERVICE) as DevicePolicyManager

        fun isActive(ctx: Context) = dpm(ctx).isAdminActive(component(ctx))

        /** Locks the phone; false when the permission has not been granted. */
        fun lock(ctx: Context): Boolean {
            if (!isActive(ctx)) return false
            return runCatching { dpm(ctx).lockNow(); true }.getOrDefault(false)
        }

        /** Asks the user to allow locking (Android's own confirmation screen). */
        fun request(activity: Activity) {
            activity.startActivity(Intent(DevicePolicyManager.ACTION_ADD_DEVICE_ADMIN)
                .putExtra(DevicePolicyManager.EXTRA_DEVICE_ADMIN, component(activity))
                .putExtra(DevicePolicyManager.EXTRA_ADD_EXPLANATION,
                    "Lets PocketStylus turn the phone's screen off when Windows Mobile turns its display off. " +
                        "It can only lock the screen."))
        }
    }
}
