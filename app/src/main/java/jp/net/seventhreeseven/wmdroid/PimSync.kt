package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.database.ContentObserver
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.CalendarContract
import android.provider.ContactsContract
import android.util.Log
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Intent
import android.os.Build
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors

/**
 * Contacts and calendar sync between Android and Windows Mobile, Android winning
 * (docs/research-phone-calls-pim.md, section G).
 *
 * One cycle: the helper dumps WM's contacts and appointments ("pim dump", guest/Helper/Pim.cs);
 * this class compares both sides with what they looked like after the last sync (the map: per key,
 * Android's record and WM's record), applies WM's changes to Android where Android did not change,
 * and has the helper apply everything else ("pim apply"). Rules, per item:
 *  - changed on Android (or on both sides): Android's version goes to WM;
 *  - changed only in WM: WM's changed fields go to Android (single events and contacts only;
 *    occurrences of repeating events always follow Android);
 *  - deleted on Android (or out of the calendar window): deleted in WM;
 *  - deleted in WM: deleted on Android only if Android did not change it, and only a few per cycle
 *    (more look like an accident: they are put back in WM);
 *  - new in WM: matched to an Android item with the same name / subject and time, else created on
 *    Android.
 * If WM's store is not the one synced last (the session was restored from an older snapshot, or WM
 * was reset: its sync generation differs), WM's differences are not trusted at all: Android's state
 * is put back, and only items new in WM are kept.
 */
class PimSync(private val ctx: Context, private val guest: GuestBridge) {
    private val ui = Handler(Looper.getMainLooper())
    private val work = Executors.newSingleThreadExecutor()
    private val contacts = PimContacts(ctx)
    private val calendar = PimCalendar(ctx)

    private enum class Phase { IDLE, DUMPING, PLANNING, APPLYING }
    private var phase = Phase.IDLE
    private var phaseAt = 0L
    private var helperAt = 0L
    private var lastSync = 0L
    private var lastEnd = 0L
    private var dirtyAt = 0L
    private var foreground = false
    private var plan: Plan? = null

    // ---- Lifecycle (EmulatorActivity) ----

    /** The helper (re)started or answered hello: a sync follows shortly. */
    fun onHello() {
        helperAt = SystemClock.uptimeMillis()
    }

    fun resume() {
        foreground = true
        val cr = ctx.contentResolver
        if (has(Manifest.permission.READ_CONTACTS)) runCatching { cr.registerContentObserver(ContactsContract.Contacts.CONTENT_URI, true, observer) }
        if (has(Manifest.permission.READ_CALENDAR)) runCatching { cr.registerContentObserver(CalendarContract.Events.CONTENT_URI, true, observer) }
    }

    fun pause() {
        foreground = false
        runCatching { ctx.contentResolver.unregisterContentObserver(observer) }
    }

    fun stop() {
        pause()
        work.shutdown()
    }

    private val observer = object : ContentObserver(ui) {
        override fun onChange(selfChange: Boolean) {
            // Our own writes during a sync (and just after it) are not news.
            if (phase == Phase.IDLE && SystemClock.uptimeMillis() - lastEnd > 15_000) dirtyAt = SystemClock.uptimeMillis()
        }
    }

    /** Called every second: starts a sync when one is due. */
    fun tick() {
        val now = SystemClock.uptimeMillis()
        if (phase != Phase.IDLE) {
            val limit = if (phase == Phase.DUMPING) 5 * 60_000L else 15 * 60_000L
            if (now - phaseAt > limit) fail("the helper did not answer (${phase.name.lowercase()})")
            return
        }
        if (!foreground || helperAt == 0L || !anyOn(ctx)) return
        val due = requested || (lastSync == 0L && now - helperAt > 20_000) ||
            (dirtyAt != 0L && now - dirtyAt > 10_000) || (lastSync != 0L && now - lastSync > 15 * 60_000L)
        if (!due) return
        requested = false
        dirtyAt = 0L
        start()
    }

    private fun start() {
        lastSync = SystemClock.uptimeMillis()
        Log.i(TAG, "sync: asking the helper for its contacts and appointments")
        guest.boxFile("pim-dump.txt").delete()
        setPhase(Phase.DUMPING)
        guest.send("pim dump")
    }

    /** A "pim ..." line from the helper, without the prefix. */
    fun onGuest(line: String) {
        val p = line.split(' ')
        when (p[0]) {
            "dumped" -> if (phase == Phase.DUMPING) {
                setPhase(Phase.PLANNING)
                work.execute {
                    val r = runCatching { planAndApplyAndroid() }
                    ui.post {
                        r.onSuccess { pl ->
                            if (pl == null) { finish("nothing to do"); return@onSuccess }
                            plan = pl
                            setPhase(Phase.APPLYING)
                            guest.send("pim apply")
                        }.onFailure { Log.w(TAG, "sync failed", it); fail(it.message ?: it.javaClass.simpleName) }
                    }
                }
            }
            "applied" -> if (phase == Phase.APPLYING) {
                val pl = plan ?: return
                work.execute {
                    val r = runCatching { commit(pl) }
                    ui.post { r.onSuccess { finish(it) }.onFailure { fail(it.message ?: "result") } }
                }
            }
            "error", "busy" -> if (phase != Phase.IDLE) fail("helper: $line")
        }
    }

    private fun setPhase(p: Phase) { phase = p; phaseAt = SystemClock.uptimeMillis() }

    private fun finish(summary: String) {
        Log.i(TAG, "sync done: $summary")
        status(ctx, "Last sync ${java.text.DateFormat.getTimeInstance(java.text.DateFormat.SHORT).format(java.util.Date())}: $summary")
        plan = null
        lastEnd = SystemClock.uptimeMillis()
        phase = Phase.IDLE
    }

    private fun fail(why: String) {
        Log.w(TAG, "sync failed: $why")
        status(ctx, "Last sync failed: $why")
        plan = null
        lastEnd = SystemClock.uptimeMillis()
        phase = Phase.IDLE
    }

    // ---- The map: both sides as of the last sync ----

    private class Entry(val kind: String, var a: String, var w: String)

    private class SyncMap(var gen: Int, val items: HashMap<String, Entry>)

    private fun mapFile(): File {
        val profile = ctx.getSharedPreferences("wmdroid", Context.MODE_PRIVATE).getString("profile", null) ?: "default"
        return File(File(ctx.filesDir, "pim").apply { mkdirs() }, "$profile.json")
    }

    private fun loadMap(): SyncMap {
        val f = mapFile()
        if (!f.exists()) return SyncMap(0, HashMap())
        val j = JSONObject(f.readText())
        val items = HashMap<String, Entry>()
        val o = j.getJSONObject("items")
        for (k in o.keys()) {
            val e = o.getJSONObject(k)
            items[k] = Entry(e.getString("k"), e.getString("a"), e.getString("w"))
        }
        return SyncMap(j.getInt("gen"), items)
    }

    private fun saveMap(m: SyncMap) {
        val o = JSONObject()
        for ((k, e) in m.items) o.put(k, JSONObject().put("k", e.kind).put("a", e.a).put("w", e.w))
        val f = mapFile()
        val tmp = File(f.path + ".tmp")
        tmp.writeText(JSONObject().put("gen", m.gen).put("items", o).toString())
        tmp.renameTo(f)
    }

    // ---- One cycle ----

    private class WmItem(val kind: String, val key: String?, val oid: String, val rec: Map<String, String>)

    private class Op(val verb: String, val kind: String, val key: String?, val oid: String?, val rec: Map<String, String>?)

    private class Plan(val map: SyncMap, val gen: Int, val ops: List<Op>, val aRec: Map<String, String>, val summary: String)

    private fun planAndApplyAndroid(): Plan? {
        val (dumpGen, wm) = readDump(guest.boxFile("pim-dump.txt"))
        val map = loadMap()
        val trusted = map.gen > 0 && dumpGen == map.gen
        val ops = ArrayList<Op>()
        val aRec = HashMap<String, String>()   // Android's record for keys the WM ops write
        var pulled = 0; var created = 0; var deletedA = 0; var pushed = 0; var deletedW = 0
        if (!trusted && map.gen > 0) Log.i(TAG, "WM's store is not the one synced last (gen $dumpGen, expected ${map.gen}): Android wins")
        val gate = Gate(loadPending(), prompt(ctx))

        for (kind in listOf(KIND_CONTACT, KIND_APPOINTMENT)) {
            val on = if (kind == KIND_CONTACT) contactsOn(ctx) else calendarOn(ctx)
            if (!on) continue
            val android: Map<String, Map<String, String>> = if (kind == KIND_CONTACT) contacts.readAll().mapValues { it.value.rec }
                else calendar.readAll().mapValues { it.value.rec }
            val known = map.items.filterValues { it.kind == kind }
            if (android.isEmpty() && known.size >= 5) {
                // Nothing at all on Android but plenty synced before: a provider hiccup or a revoked
                // permission, not a reason to empty Windows Mobile.
                Log.w(TAG, "$kind: Android returned nothing; skipping")
                continue
            }
            val wmItems = wm.filter { it.kind == kind }
            val wmByKey = HashMap<String, WmItem>()
            for (w in wmItems) {
                val k = w.key ?: continue
                if (k in wmByKey) ops += Op("delete", kind, null, w.oid, null)  // a duplicate (a copied item)
                else wmByKey[k] = w
            }
            val pulls = ArrayList<Triple<String, Map<String, String>, Map<String, String>>>()
            val wmDeleted = ArrayList<String>()
            for (k in android.keys + wmByKey.keys) {
                val a = android[k]
                val w = wmByKey[k]
                val m = map.items[k]
                val aChanged = m == null || encode(a.orEmpty()) != m.a
                val wChanged = m == null || encode(w?.rec.orEmpty()) != m.w
                when {
                    a != null && w != null -> when {
                        aChanged -> { ops += put(kind, k, w.oid, a); aRec[k] = encode(a); pushed++ }
                        wChanged && trusted && pullable(k) -> when (gate.check(k, kind, "changed", w.rec)) {
                            ALLOW -> pulls += Triple(k, decode(m!!.w), w.rec)
                            REJECT -> { ops += put(kind, k, w.oid, a); aRec[k] = encode(a); pushed++ }
                            else -> {}  // waiting for the user's answer: both sides stay as they are
                        }
                        wChanged -> { ops += put(kind, k, w.oid, a); aRec[k] = encode(a); pushed++ }
                    }
                    a != null -> if (m != null && trusted && !aChanged && pullable(k)) wmDeleted += k
                        else { ops += put(kind, k, null, a); aRec[k] = encode(a); pushed++ }
                    w != null -> { ops += Op("delete", kind, k, w.oid, null); deletedW++ }
                }
            }
            // Deleted in WM: a few at a time; more look like an accident and are put back.
            val limit = maxOf(3, known.size / 10)
            if (wmDeleted.size > limit) {
                Log.w(TAG, "$kind: ${wmDeleted.size} deleted in WM (more than $limit): putting them back")
                for (k in wmDeleted) { ops += put(kind, k, null, android[k]!!); aRec[k] = encode(android[k]!!); pushed++ }
            } else for (k in wmDeleted) {
                when (gate.check(k, kind, "deleted", android[k]!!)) {
                    ALLOW -> {
                        val ok = if (kind == KIND_CONTACT) contacts.delete(k) else calendar.delete(k)
                        if (ok) { map.items.remove(k); deletedA++ }
                    }
                    REJECT -> { ops += put(kind, k, null, android[k]!!); aRec[k] = encode(android[k]!!); pushed++ }
                }
            }
            // Changed only in WM: its changed fields go to Android.
            if (pulls.isNotEmpty()) {
                val cItems = if (kind == KIND_CONTACT) contacts.readAll() else null
                val eItems = if (kind == KIND_APPOINTMENT) calendar.readAll() else null
                for ((k, old, new) in pulls) {
                    runCatching {
                        cItems?.get(k)?.let { contacts.applyChanges(it, old, new) }
                        eItems?.get(k)?.let { calendar.applyChanges(it, old, new) }
                    }.onFailure { Log.w(TAG, "could not write $k to Android", it) }
                    pulled++
                }
            }
            // New in WM: matched to an Android item WM does not have yet, or created on Android.
            val unmatched = android.keys.filter { it !in wmByKey }.toMutableSet()
            val newWm = wmItems.filter { it.key == null }
            val created2 = HashMap<String, WmItem>()
            for (w in newWm) {
                val match = unmatched.firstOrNull { same(kind, android[it]!!, w.rec) }
                if (match != null) {
                    unmatched.remove(match)
                    // Already queued as a put of a new WM item: aim it at this one instead.
                    ops.removeAll { it.verb == "put" && it.key == match && it.oid == null }
                    ops += put(kind, match, w.oid, android[match]!!)
                    aRec[match] = encode(android[match]!!)
                    continue
                }
                when (gate.check("new:$kind:${w.oid}", kind, "new", w.rec)) {
                    REJECT -> { ops += Op("delete", kind, null, w.oid, null); deletedW++; continue }
                    HOLD -> continue
                }
                val key = runCatching { if (kind == KIND_CONTACT) contacts.create(w.rec) else calendar.create(w.rec) }
                    .onFailure { Log.w(TAG, "could not add a WM $kind to Android", it) }.getOrNull() ?: continue
                created2[key] = w
                created++
            }
            // Android's records after our writes, for the map.
            if (pulls.isNotEmpty() || created2.isNotEmpty()) {
                val now = if (kind == KIND_CONTACT) contacts.readAll().mapValues { it.value.rec } else calendar.readAll().mapValues { it.value.rec }
                for ((k, _, new) in pulls) now[k]?.let { map.items[k] = Entry(kind, encode(it), encode(new)) }
                for ((k, w) in created2) {
                    val a = now[k] ?: continue
                    ops += Op("setkey", kind, k, w.oid, null)
                    aRec[k] = encode(a)
                }
            }
            // Gone from both sides: forgotten.
            for (k in known.keys) if (k !in android && k !in wmByKey) map.items.remove(k)
        }
        gate.finish()
        val summary = "$pushed to WM, $pulled from WM, $created new from WM, $deletedW removed in WM, $deletedA removed on Android" +
            if (gate.waiting > 0) ", ${gate.waiting} waiting for your OK" else ""
        val gen = maxOf(map.gen, dumpGen) + 1
        if (ops.isEmpty() && trusted) {
            saveMap(map)
            return null
        }
        writeApply(guest.boxFile("pim-apply.txt"), gen, ops)
        Log.i(TAG, "plan: ${ops.size} WM operations; $summary")
        saveMap(map)  // Android-side changes are done whatever happens in WM
        return Plan(map, gen, ops, aRec, summary)
    }

    private fun put(kind: String, key: String, oid: String?, rec: Map<String, String>) = Op("put", kind, key, oid, rec)

    // ---- "Prompt before accepting changes from Device" ----

    /**
     * WM's changes waiting for the user (pim/<profile>-pending.json): items by id (key, or
     * "new:<kind>:<oid>" for an item new in WM) with a label, and the user's answer for all of them
     * ("accept", "reject" or none yet). Answered from the notification (PimDecisionReceiver).
     */
    private inner class Gate(private val pending: JSONObject, private val on: Boolean) {
        private val items = pending.optJSONObject("items") ?: JSONObject()
        private val decision = pending.optString("decision")
        private var added = 0
        var waiting = 0
            private set

        private val seen = HashSet<String>()

        fun check(id: String, kind: String, what: String, rec: Map<String, String>): Int {
            if (!on) return ALLOW
            seen += id
            if (items.has(id)) {
                when (decision) {
                    "accept" -> { items.remove(id); return ALLOW }
                    "reject" -> { items.remove(id); return REJECT }
                }
                waiting++
                return HOLD
            }
            items.put(id, JSONObject().put("kind", kind).put("what", what).put("label", label(kind, rec)))
            added++
            waiting++
            return HOLD
        }

        /** An answer is used up by this cycle; changes that are gone (or new ones) wait for the next. */
        fun finish() {
            if (!on) { pendingFile().delete(); cancelPending(ctx); return }
            for (id in items.keys().asSequence().toList()) if (id !in seen) items.remove(id)
            pendingFile().writeText(JSONObject().put("items", items).put("decision", "").toString())
            if (items.length() == 0) cancelPending(ctx)
            else if (added > 0 || decision.isNotEmpty()) notifyPending(ctx, items)
        }
    }

    private fun label(kind: String, r: Map<String, String>): String = if (kind == KIND_CONTACT)
        listOf(r["FirstName"], r["LastName"]).filter { !it.isNullOrBlank() }.joinToString(" ").ifEmpty { r["CompanyName"].orEmpty() }
    else r["Subject"].orEmpty().ifEmpty { "(no subject)" } + (r["Start"]?.let { " " + it.take(8) } ?: "")

    private fun pendingFile(): File = File(mapFile().path.removeSuffix(".json") + "-pending.json")

    private fun loadPending(): JSONObject = runCatching { JSONObject(pendingFile().readText()) }.getOrElse { JSONObject() }

    private fun pullable(key: String) = key.startsWith("c:") || key.startsWith("e:")

    /** A WM item made without the sync is the same as an Android item: name or number / subject and start. */
    private fun same(kind: String, a: Map<String, String>, w: Map<String, String>): Boolean {
        if (kind == KIND_APPOINTMENT) return a["Subject"].orEmpty() == w["Subject"].orEmpty() && a["Start"] == w["Start"]
        fun name(r: Map<String, String>) = listOf(r["FirstName"], r["LastName"]).joinToString(" ") { it.orEmpty() }.trim().lowercase()
            .ifEmpty { r["CompanyName"].orEmpty().trim().lowercase() }
        if (name(a).isNotEmpty() && name(a) == name(w)) return true
        fun numbers(r: Map<String, String>) = r.filterKeys { it.endsWith("TelephoneNumber") || it.endsWith("Number") }
            .values.map { n -> n.filter { it.isDigit() }.takeLast(7) }.filter { it.length >= 7 }.toSet()
        return numbers(a).intersect(numbers(w)).isNotEmpty()
    }

    private fun commit(pl: Plan): String {
        val map = pl.map
        var failed = 0
        val f = guest.boxFile("pim-result.txt")
        var verb = ""; var kind = ""; var ok = false
        var fields = LinkedHashMap<String, String>()
        fun flush() {
            if (verb.isEmpty()) return
            val key = fields.remove("key")
            fields.remove("oid")
            fields.remove("error")
            if (!ok) failed++
            else if (key != null) when (verb) {
                "delete" -> map.items.remove(key)
                "put", "setkey" -> {
                    val a = pl.aRec[key]
                    if (a != null) map.items[key] = Entry(kind, a, encode(fields))
                }
            }
            verb = ""
        }
        f.forEachLine { line ->
            if (line.startsWith("#")) {
                flush()
                val p = line.substring(1).split(' ')
                ok = p[0] == "ok"
                verb = p.getOrElse(1) { "" }
                kind = p.getOrElse(2) { "" }
                fields = LinkedHashMap()
            } else {
                val eq = line.indexOf('=')
                if (eq > 0) fields[line.substring(0, eq)] = unescape(line.substring(eq + 1))
            }
        }
        flush()
        map.gen = pl.gen
        saveMap(map)
        return pl.summary + if (failed > 0) ", $failed failed in WM" else ""
    }

    // ---- Files ----

    private fun readDump(f: File): Pair<Int, List<WmItem>> {
        var gen = 0
        val items = ArrayList<WmItem>()
        var kind: String? = null
        var rec = LinkedHashMap<String, String>()
        fun flush() {
            val k = kind ?: return
            val key = rec.remove("key")
            val oid = rec.remove("oid") ?: "0"
            items += WmItem(k, key, oid, rec)
        }
        f.forEachLine { line ->
            when {
                line.startsWith("#") -> { flush(); kind = line.substring(1).trim(); rec = LinkedHashMap() }
                kind == null -> if (line.startsWith("gen=")) gen = line.substring(4).toIntOrNull() ?: 0
                else -> { val eq = line.indexOf('='); if (eq > 0) rec[line.substring(0, eq)] = unescape(line.substring(eq + 1)) }
            }
        }
        flush()
        return gen to items
    }

    private fun writeApply(f: File, gen: Int, ops: List<Op>) {
        val sb = StringBuilder("gen=$gen\n")
        for (op in ops) {
            sb.append('#').append(op.verb).append(' ').append(op.kind).append('\n')
            op.key?.let { sb.append("key=").append(escape(it)).append('\n') }
            op.oid?.let { sb.append("oid=").append(it).append('\n') }
            op.rec?.forEach { (k, v) -> sb.append(k).append('=').append(escape(v)).append('\n') }
        }
        val tmp = File(f.path + ".tmp")
        tmp.writeText(sb.toString())
        f.delete()
        tmp.renameTo(f)
    }

    private fun has(p: String) = ctx.checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED

    companion object {
        private const val TAG = "WMDroidPim"
        const val KIND_CONTACT = "contact"
        const val KIND_APPOINTMENT = "appointment"

        /** "Sync now" from the settings page; the running emulator picks it up within a second. */
        @Volatile var requested = false

        fun contactsOn(ctx: Context) = Settings.global(ctx).getBoolean("pim.contacts", false) &&
            ctx.checkSelfPermission(Manifest.permission.WRITE_CONTACTS) == PackageManager.PERMISSION_GRANTED &&
            ctx.checkSelfPermission(Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED
        fun calendarOn(ctx: Context) = Settings.global(ctx).getBoolean("pim.calendar", false) &&
            ctx.checkSelfPermission(Manifest.permission.WRITE_CALENDAR) == PackageManager.PERMISSION_GRANTED &&
            ctx.checkSelfPermission(Manifest.permission.READ_CALENDAR) == PackageManager.PERMISSION_GRANTED
        fun anyOn(ctx: Context) = contactsOn(ctx) || calendarOn(ctx)
        fun setOn(ctx: Context, kind: String, on: Boolean) =
            Settings.global(ctx).edit().putBoolean(if (kind == KIND_CONTACT) "pim.contacts" else "pim.calendar", on).apply()
        fun status(ctx: Context): String = Settings.prefs(ctx).getString("pim.status", null) ?: "Not synced yet."

        const val ALLOW = 0
        const val HOLD = 1
        const val REJECT = 2

        /** "Prompt before accepting changes from Device": WM's changes wait for the user's OK. */
        fun prompt(ctx: Context) = Settings.global(ctx).getBoolean("pim.prompt", false)
        fun setPrompt(ctx: Context, on: Boolean) = Settings.global(ctx).edit().putBoolean("pim.prompt", on).apply()

        private const val CHANNEL = "pim"
        private const val NOTE_ID = 0x7019

        /** Lists WM's waiting changes, with Accept and Reject. */
        fun notifyPending(ctx: Context, items: JSONObject) {
            if (Build.VERSION.SDK_INT >= 33 && ctx.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
            val nm = ctx.getSystemService(NotificationManager::class.java)
            if (nm.getNotificationChannel(CHANNEL) == null)
                nm.createNotificationChannel(NotificationChannel(CHANNEL, "Contacts & calendar changes", NotificationManager.IMPORTANCE_DEFAULT))
            val lines = items.keys().asSequence().map { items.getJSONObject(it) }
                .map { "${it.optString("label")}: ${it.optString("what")} (${if (it.optString("kind") == KIND_CONTACT) "contact" else "calendar"})" }.toList()
            fun action(answer: String, code: Int) = PendingIntent.getBroadcast(ctx, code,
                Intent(ctx, PimDecisionReceiver::class.java).putExtra("answer", answer), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            val style = Notification.InboxStyle().also { st -> lines.take(7).forEach { st.addLine(it) } }
            if (lines.size > 7) style.setSummaryText("and ${lines.size - 7} more")
            val n = Notification.Builder(ctx, CHANNEL)
                .setSmallIcon(R.drawable.ic_stat_running)
                .setContentTitle("Windows Mobile changed ${lines.size} item${if (lines.size == 1) "" else "s"}")
                .setContentText(lines.firstOrNull().orEmpty())
                .setStyle(style)
                .setContentIntent(PendingIntent.getActivity(ctx, 0, Intent(ctx, PimSettingsActivity::class.java), PendingIntent.FLAG_IMMUTABLE))
                .addAction(Notification.Action.Builder(null, "Accept", action("accept", 1)).build())
                .addAction(Notification.Action.Builder(null, "Reject", action("reject", 2)).build())
                .build()
            nm.notify(NOTE_ID, n)
        }

        fun cancelPending(ctx: Context) { ctx.getSystemService(NotificationManager::class.java)?.cancel(NOTE_ID) }

        /** The user's answer: recorded for the waiting changes, and a sync to act on it. */
        fun decide(ctx: Context, answer: String) {
            val profile = ctx.getSharedPreferences("wmdroid", Context.MODE_PRIVATE).getString("profile", null) ?: "default"
            val f = File(File(ctx.filesDir, "pim"), "$profile-pending.json")
            val j = runCatching { JSONObject(f.readText()) }.getOrElse { JSONObject() }
            f.writeText(j.put("decision", answer).toString())
            cancelPending(ctx)
            requested = true
        }
        private fun status(ctx: Context, s: String) = Settings.prefs(ctx).edit().putString("pim.status", s).apply()

        /** A record as text: sorted "field=value" lines (values escaped), for comparing and storing. */
        fun encode(rec: Map<String, String>): String =
            rec.filterValues { it.isNotEmpty() }.toSortedMap().entries.joinToString("\n") { "${it.key}=${escape(it.value)}" }

        fun decode(s: String): Map<String, String> = s.lineSequence().filter { it.isNotEmpty() }.associate {
            val eq = it.indexOf('=')
            it.substring(0, eq) to unescape(it.substring(eq + 1))
        }

        fun escape(v: String) = v.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")

        fun unescape(v: String): String {
            if ('\\' !in v) return v
            val sb = StringBuilder(v.length)
            var i = 0
            while (i < v.length) {
                val c = v[i]
                if (c == '\\' && i + 1 < v.length) {
                    val n = v[++i]
                    sb.append(when (n) { 'n' -> '\n'; 'r' -> '\r'; else -> n })
                } else sb.append(c)
                i++
            }
            return sb.toString()
        }
    }
}

/** Accept / Reject on the "Windows Mobile changed ..." notification (PimSync.decide). */
class PimDecisionReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        val answer = intent.getStringExtra("answer") ?: return
        PimSync.decide(ctx, answer)
    }
}
