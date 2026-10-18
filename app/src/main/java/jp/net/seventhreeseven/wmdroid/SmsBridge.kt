package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Person
import android.app.RemoteInput
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Telephony
import android.telephony.SmsManager
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Text messages between Android and Windows Mobile.
 *
 * Incoming: SmsReceiver (manifest, works with the app closed) puts each message into a first-in
 * first-out queue (one file per OS). Whenever Windows Mobile can take messages the queue is handed
 * over in order: WM 6 / 6.1 / 6.1.4 through the virtual modem as real SMS (+CMT, so WM's own inbox,
 * sound and notification), WM 6.5.3 through the helper (guest/Helper/Sms.cs, WM's messaging API).
 * With "Defer Incoming SMS Messages", messages that arrive while Windows Mobile is not in front
 * stay queued and show as Android notifications with a quick reply; they reach WM when it is
 * opened. Without it, a session kept in memory is woken to take them right away.
 *
 * Outgoing: WM's sends (the modem's AT+CMGS, or the helper's report of WM's Outbox) are sent from
 * the phone with SmsManager; split messages are joined again first.
 */
class SmsBridge(private val ctx: Context, private val guest: GuestBridge, private val handle: () -> Long) {
    private val ui = Handler(Looper.getMainLooper())
    var foreground = false
    /** Asks the emulator to wake a session in the background for a message (false: it cannot). */
    var wake: (() -> Boolean)? = null

    // ---- Lifecycle ----

    fun start() {
        live = this
        if (enabled(ctx)) scanMms(ctx)
        runCatching { ctx.contentResolver.registerContentObserver(android.net.Uri.parse("content://mms"), true, mmsObserver) }
        runCatching {
            if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(sentReceiver, IntentFilter(ACTION_SENT), Context.RECEIVER_NOT_EXPORTED)
            else ctx.registerReceiver(sentReceiver, IntentFilter(ACTION_SENT))
        }
    }

    fun stop() {
        if (live === this) live = null
        runCatching { ctx.contentResolver.unregisterContentObserver(mmsObserver) }
        runCatching { ctx.unregisterReceiver(sentReceiver) }
    }

    fun resume() {
        foreground = true
        cancelNotifications(ctx)
        deliverPending()
    }

    fun pause() { foreground = false }

    // ---- Incoming ----

    /** An MMS the phone's messaging app finished downloading: queued like a text, pictures and all. */
    private val mmsObserver = object : android.database.ContentObserver(ui) {
        override fun onChange(selfChange: Boolean) {
            ui.removeCallbacks(mmsScan)
            ui.postDelayed(mmsScan, 3000)
        }
    }
    private val mmsScan = Runnable { if (enabled(ctx) && scanMms(ctx) > 0) onQueued() }

    private var delivering = false

    /** True while a batch is being filed into Windows Mobile. The card must stay in for that. */
    fun isFiling() = delivering

    /** New messages are queued: deliver now if WM can take them, else wake it or notify. */
    fun onQueued() {
        if (foreground) { deliverPending(); return }
        if (defer(ctx)) { notifyPending(ctx); return }
        if (wake?.invoke() == true) ui.postDelayed({ deliverPending() }, 1500)
        else notifyPending(ctx)
    }

    /** Hands the queue to Windows Mobile, oldest first. */
    fun deliverPending() {
        val h = handle()
        if (h == 0L || delivering || !enabled(ctx)) return
        val q = queue(ctx)
        if (q.isEmpty()) return
        // Pictures from MMS go first: the helper files them in WM's My Pictures\MMS (Sms.cs).
        val withFiles = q.filter { it.files.isNotEmpty() }
        if (withFiles.isNotEmpty()) {
            val dir = guest.boxFile("mms").apply { mkdirs() }
            for (m in withFiles) for (f in m.files) {
                val src = File(f)
                if (src.exists()) runCatching { src.copyTo(File(dir, src.name), overwrite = true); src.delete() }
            }
            guest.send("sms pictures")
        }
        // A live unread text on a modem image still arrives as +CMT. Everything already on the phone
        // (read, or sent) is filed into the inbox or Sent Items, so a history import does not ring once per message.
        val emit = if (NativeBridge.modemActive(h)) q.filter { it.box != "out" && !it.read && it.files.isEmpty() } else emptyList()
        val file = q.filter { m -> emit.none { it.id == m.id } }.take(8)
        for (m in emit) {
            SmsPdu.deliver(m.from, m.text, m.time, (m.id and 0xFF).toInt()).forEach { NativeBridge.modemSms(h, it) }
            Log.i(TAG, "delivered message ${m.id} to Windows Mobile (modem)")
        }
        if (emit.isNotEmpty()) removeFromQueue(ctx, emit.map { it.id }.toSet())
        if (file.isNotEmpty()) {
            val sb = StringBuilder()
            for (m in file) {
                sb.append("#sms\nid=").append(m.id).append("\nfrom=").append(PimSync.escape(m.from))
                    .append("\ntime=").append(m.time).append("\ntext=").append(PimSync.escape(m.text))
                    .append("\nbox=").append(if (m.box == "out") "out" else "in")
                    .append("\nread=").append(if (m.read) "1" else "0").append('\n')
            }
            guest.boxFile("sms-in.txt").writeText(sb.toString())
            delivering = true
            guest.send("sms deliver")
            ui.postDelayed({ delivering = false }, if (file.size > 40) 5 * 60_000L else 30_000L)
        }
    }

    /** A "sms ..." line from the helper, without the prefix. */
    fun onGuest(line: String) {
        val p = line.split(' ')
        when (p[0]) {
            "delivered" -> {
                delivering = false
                removeFromQueue(ctx, p.drop(1).mapNotNull { it.toLongOrNull() }.toSet())
                if (queue(ctx).isNotEmpty()) deliverPending()
            }
            "sent" -> {
                // WM 6.5.3: a message from WM's Outbox, in sms-out-<n>.txt ("to=", "text=").
                val f = guest.boxFile(p.getOrNull(1) ?: return)
                val rec = runCatching { PimSync.decode(f.readText().trim()) }.getOrNull() ?: return
                f.delete()
                send(rec["to"].orEmpty(), rec["text"].orEmpty()) { ok -> guest.send("sms result ${p[1]} ${if (ok) 1 else 0}") }
            }
        }
    }

    // ---- Outgoing (modem) ----

    private class Partial(val to: String, val parts: Int) {
        val text = arrayOfNulls<String>(parts)
        val at = SystemClock.uptimeMillis()
    }
    private val partials = HashMap<String, Partial>()

    /** The modem event "smssend <mr> <pdu>": Windows Mobile sends a message (or one part of it). */
    fun onModemSend(mr: Int, pdu: String) {
        val h = handle()
        val s = SmsPdu.submit(pdu)
        if (s == null || s.to.isEmpty()) { NativeBridge.modemSmsResult(h, mr, false); return }
        if (s.parts <= 1) {
            send(s.to, s.text) { ok -> NativeBridge.modemSmsResult(handle(), mr, ok) }
            return
        }
        partials.entries.removeAll { SystemClock.uptimeMillis() - it.value.at > 120_000 }
        val key = "${s.to}/${s.ref}"
        val p = partials.getOrPut(key) { Partial(s.to, s.parts) }
        if (s.part in 1..p.parts) p.text[s.part - 1] = s.text
        if (p.text.any { it == null }) {
            NativeBridge.modemSmsResult(h, mr, true)  // a middle part: WM sends the next one
            return
        }
        partials.remove(key)
        send(p.to, p.text.joinToString("")) { ok -> NativeBridge.modemSmsResult(handle(), mr, ok) }
    }

    private val sentCallbacks = HashMap<Int, (Boolean) -> Unit>()
    private var nextToken = 1

    private fun send(to: String, text: String, done: (Boolean) -> Unit) {
        if (!enabled(ctx) || ctx.checkSelfPermission(Manifest.permission.SEND_SMS) != PackageManager.PERMISSION_GRANTED || to.isBlank()) {
            done(false); return
        }
        val sms = if (Build.VERSION.SDK_INT >= 31) ctx.getSystemService(SmsManager::class.java) else @Suppress("DEPRECATION") SmsManager.getDefault()
        val parts = sms.divideMessage(text)
        val token = nextToken++
        sentCallbacks[token] = done
        val pi = PendingIntent.getBroadcast(ctx, token, Intent(ACTION_SENT).setPackage(ctx.packageName).putExtra("token", token),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_ONE_SHOT)
        // The last part's result stands for the message.
        val intents = ArrayList<PendingIntent?>(parts.size).apply { repeat(parts.size - 1) { add(null) }; add(pi) }
        try {
            sms.sendMultipartTextMessage(to, null, parts, intents, null)
            Log.i(TAG, "sending a message from Windows Mobile (${parts.size} part(s))")
        } catch (e: RuntimeException) {
            Log.w(TAG, "send failed", e)
            sentCallbacks.remove(token)
            done(false)
        }
        // No answer at all (no service): WM hears "failed" after a minute.
        ui.postDelayed({ sentCallbacks.remove(token)?.invoke(false) }, 60_000)
    }

    private val sentReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val cb = sentCallbacks.remove(i.getIntExtra("token", 0)) ?: return
            cb(resultCode == android.app.Activity.RESULT_OK)
        }
    }

    // ---- Queue, settings and notifications ----

    /** A queued message; files: an MMS's pictures, video and sound (paths in the app's storage). */
    class Msg(val id: Long, val from: String, val text: String, val time: Long, val files: List<String> = emptyList(),
        val box: String = "in", val read: Boolean = false)

    companion object {
        private const val TAG = "WMDroidSms"
        private const val ACTION_SENT = "jp.net.seventhreeseven.wmdroid.SMS_SENT"
        const val CHANNEL = "sms"
        const val KEY_REPLY = "reply"
        /** The running emulator's bridge (null with no session): receivers hand new messages to it. */
        @Volatile var live: SmsBridge? = null

        fun enabled(ctx: Context) = Settings.global(ctx).getBoolean("sms.enabled", false)
        fun setEnabled(ctx: Context, on: Boolean) = Settings.global(ctx).edit().putBoolean("sms.enabled", on).apply()
        fun defer(ctx: Context) = Settings.global(ctx).getBoolean("sms.defer", false)
        fun setDefer(ctx: Context, on: Boolean) = Settings.global(ctx).edit().putBoolean("sms.defer", on).apply()

        fun permissions(): Array<String> = buildList {
            add(Manifest.permission.SEND_SMS); add(Manifest.permission.RECEIVE_SMS); add(Manifest.permission.READ_SMS)
            add(Manifest.permission.RECEIVE_MMS)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }.toTypedArray()

        private fun queueFile(ctx: Context): File {
            val profile = ctx.getSharedPreferences("wmdroid", Context.MODE_PRIVATE).getString("profile", null) ?: "default"
            return File(File(ctx.filesDir, "sms").apply { mkdirs() }, "$profile.json")
        }

        @Synchronized fun queue(ctx: Context): List<Msg> {
            val f = queueFile(ctx)
            if (!f.exists()) return emptyList()
            val a = runCatching { JSONArray(f.readText()) }.getOrElse { JSONArray() }
            return (0 until a.length()).map { a.getJSONObject(it) }.map { o ->
                val f = o.optJSONArray("files")
                Msg(o.getLong("id"), o.getString("from"), o.getString("text"), o.getLong("time"),
                    if (f == null) emptyList() else (0 until f.length()).map { f.getString(it) },
                    o.optString("box", "in"), o.optBoolean("read", false))
            }
        }

        @Synchronized private fun write(ctx: Context, list: List<Msg>) {
            val a = JSONArray()
            for (m in list) a.put(JSONObject().put("id", m.id).put("from", m.from).put("text", m.text).put("time", m.time)
                .put("files", JSONArray(m.files)).put("box", m.box).put("read", m.read))
            val f = queueFile(ctx)
            val tmp = File(f.path + ".tmp")
            tmp.writeText(a.toString())
            tmp.renameTo(f)
        }

        @Synchronized fun enqueue(ctx: Context, from: String, text: String, time: Long, files: List<String> = emptyList()) {
            val q = queue(ctx).toMutableList()
            val id = (q.maxOfOrNull { it.id } ?: (System.currentTimeMillis() / 1000)) + 1
            q += Msg(id, from, text, time, files)
            write(ctx, q.takeLast(500))
        }

        @Synchronized fun removeFromQueue(ctx: Context, ids: Set<Long>) {
            if (ids.isEmpty()) return
            write(ctx, queue(ctx).filter { it.id !in ids })
        }

        /**
         * Copies this phone's existing texts into the queue, once per Windows image.
         * Settings' Load Messages is what calls this. Inbox and sent, oldest first, marked read
         * so Windows Mobile does not notify for each one. A short note for the user, or "permission".
         */
        fun loadHistory(ctx: Context): String {
            if (!enabled(ctx)) return "Turn on text messages in Windows Mobile first"
            if (ctx.checkSelfPermission(Manifest.permission.READ_SMS) != PackageManager.PERMISSION_GRANTED) return "permission"
            if (Settings.prefs(ctx).getBoolean("sms.historyImported", false)) return "Those messages are already in Windows Mobile"
            val n = importHistory(ctx)
            if (n > 0) live?.onQueued()
            return if (n > 0) "Loading $n messages into Windows Mobile" else "No messages on this phone"
        }

        /** The phone's existing texts. [loadHistory] is the only caller. */
        @Synchronized fun importHistory(ctx: Context): Int {
            if (!enabled(ctx) || ctx.checkSelfPermission(Manifest.permission.READ_SMS) != PackageManager.PERMISSION_GRANTED) return 0
            val prefs = Settings.prefs(ctx)
            if (prefs.getBoolean("sms.historyImported", false)) return 0
            val added = ArrayList<Msg>()
            val ok = runCatching {
                ctx.contentResolver.query(Telephony.Sms.CONTENT_URI,
                    arrayOf(Telephony.Sms._ID, Telephony.Sms.ADDRESS, Telephony.Sms.BODY, Telephony.Sms.DATE, Telephony.Sms.TYPE),
                    null, null, "${Telephony.Sms.DATE} ASC")?.use { c ->
                    while (c.moveToNext()) {
                        val type = c.getInt(4)
                        if (type != Telephony.Sms.MESSAGE_TYPE_INBOX && type != Telephony.Sms.MESSAGE_TYPE_SENT) continue
                        added += Msg(c.getLong(0), c.getString(1).orEmpty(), c.getString(2).orEmpty(), c.getLong(3),
                            emptyList(), if (type == Telephony.Sms.MESSAGE_TYPE_SENT) "out" else "in", read = true)
                    }
                }
            }.onFailure { Log.w(TAG, "reading existing messages failed", it) }.isSuccess
            if (!ok) return 0
            val have = queue(ctx).map { it.id }.toSet()
            val q = queue(ctx).toMutableList()
            for (m in added) if (m.id !in have) q += m
            write(ctx, q)
            prefs.edit().putBoolean("sms.historyImported", true).apply()
            return added.count { it.id !in have }
        }

        /**
         * New MMS in the phone's inbox since the last look (the first look only remembers where it is,
         * so turning texts on does not import old picture messages). Returns how many were queued.
         */
        @Synchronized fun scanMms(ctx: Context): Int {
            if (ctx.checkSelfPermission(Manifest.permission.READ_SMS) != PackageManager.PERMISSION_GRANTED) return 0
            val prefs = Settings.prefs(ctx)
            val cr = ctx.contentResolver
            val mms = android.net.Uri.parse("content://mms")
            val last = prefs.getLong("sms.lastMms", -1L)
            if (last < 0) {
                val max = runCatching { cr.query(mms, arrayOf("_id"), null, null, "_id DESC")?.use { if (it.moveToFirst()) it.getLong(0) else 0L } }
                    .getOrNull() ?: 0L
                prefs.edit().putLong("sms.lastMms", max).apply()
                return 0
            }
            var queued = 0
            var newest = last
            runCatching {
                cr.query(mms, arrayOf("_id", "date"), "msg_box=1 AND _id>?", arrayOf(last.toString()), "_id")?.use { c ->
                    while (c.moveToNext()) {
                        val id = c.getLong(0)
                        newest = maxOf(newest, id)
                        val time = c.getLong(1) * 1000
                        val from = cr.query(android.net.Uri.parse("content://mms/$id/addr"), arrayOf("address"), "type=137", null, null)
                            ?.use { if (it.moveToFirst()) it.getString(0) else null } ?: "MMS"
                        val text = StringBuilder()
                        val files = ArrayList<String>()
                        val dir = File(File(ctx.filesDir, "sms"), "att").apply { mkdirs() }
                        cr.query(android.net.Uri.parse("content://mms/part"), arrayOf("_id", "ct", "text", "cl", "name"), "mid=?",
                            arrayOf(id.toString()), null)?.use { p ->
                            while (p.moveToNext()) {
                                val ct = p.getString(1).orEmpty()
                                when {
                                    ct == "text/plain" -> p.getString(2)?.let { if (text.isNotEmpty()) text.append('\n'); text.append(it) }
                                    ct.startsWith("image/") || ct.startsWith("video/") || ct.startsWith("audio/") -> {
                                        val ext = (p.getString(3) ?: p.getString(4)).orEmpty().substringAfterLast('.', "")
                                            .ifEmpty { ct.substringAfter('/').substringBefore(';').replace("jpeg", "jpg") }
                                        val safeFrom = from.filter { it.isLetterOrDigit() || it == '+' }.ifEmpty { "MMS" }
                                        val out = File(dir, "${safeFrom}_${id}_${files.size + 1}.$ext")
                                        runCatching {
                                            cr.openInputStream(android.net.Uri.parse("content://mms/part/${p.getLong(0)}"))?.use { input ->
                                                out.outputStream().use { input.copyTo(it) }
                                            }
                                            files += out.path
                                        }
                                    }
                                }
                            }
                        }
                        val note = when (files.size) {
                            0 -> ""
                            1 -> "[1 picture in My Pictures\\MMS]"
                            else -> "[${files.size} pictures in My Pictures\\MMS]"
                        }
                        val body = listOf(text.toString().trim(), note).filter { it.isNotEmpty() }.joinToString(" ").ifEmpty { "[MMS]" }
                        enqueue(ctx, from, body, time, files)
                        queued++
                    }
                }
            }.onFailure { Log.w(TAG, "reading MMS failed", it) }
            if (newest != last) prefs.edit().putLong("sms.lastMms", newest).apply()
            return queued
        }

        /** Arrived from SmsReceiver. */
        fun received(ctx: Context, from: String, text: String, time: Long) {
            if (!enabled(ctx)) return
            enqueue(ctx, from, text, time)
            val b = live
            if (b != null) Handler(Looper.getMainLooper()).post { b.onQueued() }
            else if (defer(ctx)) notifyPending(ctx)
        }

        private fun channel(ctx: Context): NotificationManager {
            val nm = ctx.getSystemService(NotificationManager::class.java)
            if (nm.getNotificationChannel(CHANNEL) == null)
                nm.createNotificationChannel(NotificationChannel(CHANNEL, "Text messages for Windows Mobile", NotificationManager.IMPORTANCE_HIGH)
                    .apply { description = "Messages waiting for Windows Mobile, with a quick reply" })
            return nm
        }

        /** One notification per sender with the messages Windows Mobile has not taken yet. */
        fun notifyPending(ctx: Context) {
            if (Build.VERSION.SDK_INT >= 33 && ctx.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
            val nm = channel(ctx)
            val open = PendingIntent.getActivity(ctx, 0, Intent(ctx, LauncherActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                PendingIntent.FLAG_IMMUTABLE)
            for ((from, msgs) in queue(ctx).groupBy { it.from }) {
                val style = messagingStyle()
                msgs.forEach { style.addFrom(it.text, it.time, from) }
                val reply = PendingIntent.getBroadcast(ctx, from.hashCode(),
                    Intent(ctx, SmsReplyReceiver::class.java).putExtra("to", from),
                    PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
                val action = Notification.Action.Builder(null, "Reply", reply)
                    .addRemoteInput(RemoteInput.Builder(KEY_REPLY).setLabel("Reply").build()).build()
                val n = Notification.Builder(ctx, CHANNEL)
                    .setSmallIcon(R.drawable.ic_stat_running)
                    .setStyle(style)
                    .setContentIntent(open)
                    .addAction(action)
                    .setCategory(Notification.CATEGORY_MESSAGE)
                    .setGroup("wm.sms")
                    .build()
                nm.notify(TAG_NOTE, from.hashCode(), n)
            }
        }

        fun cancelNotifications(ctx: Context) {
            val nm = ctx.getSystemService(NotificationManager::class.java) ?: return
            for (n in nm.activeNotifications) if (n.tag == TAG_NOTE) nm.cancel(TAG_NOTE, n.id)
        }

        private const val TAG_NOTE = "wm.sms"
    }
}

/** SMS_RECEIVED: every incoming text goes into the queue for Windows Mobile. */
class SmsReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action != Telephony.Sms.Intents.SMS_RECEIVED_ACTION) return
        val parts = Telephony.Sms.Intents.getMessagesFromIntent(intent) ?: return
        if (parts.isEmpty()) return
        val from = parts[0].displayOriginatingAddress ?: parts[0].originatingAddress ?: return
        val text = parts.joinToString("") { it.displayMessageBody ?: it.messageBody ?: "" }
        SmsBridge.received(ctx, from, text, parts[0].timestampMillis.takeIf { it > 0 } ?: System.currentTimeMillis())
    }
}

/** A quick reply typed into a message notification: sent from the phone. */
class SmsReplyReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        val to = intent.getStringExtra("to") ?: return
        val text = RemoteInput.getResultsFromIntent(intent)?.getCharSequence(SmsBridge.KEY_REPLY)?.toString() ?: return
        if (text.isBlank() || ctx.checkSelfPermission(Manifest.permission.SEND_SMS) != PackageManager.PERMISSION_GRANTED) return
        val sms = if (Build.VERSION.SDK_INT >= 31) ctx.getSystemService(SmsManager::class.java) else @Suppress("DEPRECATION") SmsManager.getDefault()
        runCatching { sms.sendMultipartTextMessage(to, null, sms.divideMessage(text), null, null) }
            .onFailure { Log.w("WMDroidSms", "quick reply failed", it) }
        // The conversation notification shows the reply and stays until Windows Mobile takes the messages.
        val nm = ctx.getSystemService(NotificationManager::class.java)
        val style = messagingStyle()
        SmsBridge.queue(ctx).filter { it.from == to }.forEach { style.addFrom(it.text, it.time, to) }
        style.addFrom(text, System.currentTimeMillis(), null)
        nm.notify("wm.sms", to.hashCode(), Notification.Builder(ctx, SmsBridge.CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_running).setStyle(style).setOnlyAlertOnce(true).setGroup("wm.sms").build())
    }
}

// Conversation notifications: Person senders are API 28; Android 8.x uses the name-based style.
private fun messagingStyle(): Notification.MessagingStyle =
    if (Build.VERSION.SDK_INT >= 28) Notification.MessagingStyle(Person.Builder().setName("Me").build())
    else @Suppress("DEPRECATION") Notification.MessagingStyle("Me")

/** A message from [from], or from the user when it is null. */
private fun Notification.MessagingStyle.addFrom(text: CharSequence, time: Long, from: String?) {
    if (Build.VERSION.SDK_INT >= 28) addMessage(text, time, from?.let { Person.Builder().setName(it).build() })
    else @Suppress("DEPRECATION") addMessage(text, time, from)
}
