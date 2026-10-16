package jp.net.seventhreeseven.wmdroid

import android.content.ContentUris
import android.content.ContentValues
import android.content.Context
import android.provider.CalendarContract
import android.provider.CalendarContract.Events
import android.provider.CalendarContract.Instances
import android.provider.CalendarContract.Reminders
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone

/**
 * Android calendar events as Windows Mobile appointment records (PimSync), for the visible,
 * synced calendars and a window around today. A single event is keyed "e:<event id>" and syncs both
 * ways. Repeating events are sent as their occurrences in the window, keyed "i:<event id>:<begin>",
 * and only one way: Android's series is the truth, and WM changes to an occurrence are put back.
 * Record fields as in guest/Helper/Pim.cs: Start/End are UTC "yyyyMMddTHHmmssZ", or "yyyyMMdd" dates
 * (End exclusive) for all-day events.
 */
class PimCalendar(private val ctx: Context) {
    private val cr = ctx.contentResolver

    class Item(val key: String, val eventId: Long, val rec: Map<String, String>, val writable: Boolean)

    private class Cal(val name: String, val writable: Boolean)

    private fun calendars(): Map<Long, Cal> {
        val out = HashMap<Long, Cal>()
        cr.query(CalendarContract.Calendars.CONTENT_URI, arrayOf(CalendarContract.Calendars._ID,
            CalendarContract.Calendars.CALENDAR_DISPLAY_NAME, CalendarContract.Calendars.CALENDAR_ACCESS_LEVEL),
            "${CalendarContract.Calendars.VISIBLE}=1 AND ${CalendarContract.Calendars.SYNC_EVENTS}=1", null, null)?.use { c ->
            while (c.moveToNext())
                out[c.getLong(0)] = Cal(c.getString(1).orEmpty(), c.getInt(2) >= CalendarContract.Calendars.CAL_ACCESS_CONTRIBUTOR)
        } ?: throw IllegalStateException("calendar provider unavailable")
        return out
    }

    fun readAll(): Map<String, Item> {
        val cals = calendars()
        if (cals.isEmpty()) return emptyMap()
        val now = System.currentTimeMillis()
        val begin = now - PAST_DAYS * DAY
        val end = now + FUTURE_DAYS * DAY
        val uri = Instances.CONTENT_URI.buildUpon().also { ContentUris.appendId(it, begin); ContentUris.appendId(it, end) }.build()
        val proj = arrayOf(Instances.EVENT_ID, Instances.BEGIN, Instances.END, Instances.TITLE, Instances.EVENT_LOCATION,
            Instances.DESCRIPTION, Instances.ALL_DAY, Instances.AVAILABILITY, Instances.ACCESS_LEVEL, Instances.CALENDAR_ID,
            Instances.RRULE, Instances.RDATE, Instances.ORIGINAL_ID, Instances.HAS_ALARM)
        val sel = "${Instances.CALENDAR_ID} IN (${cals.keys.joinToString(",")})"
        class Raw(val eventId: Long, val key: String, val rec: LinkedHashMap<String, String>, val alarm: Boolean, val writable: Boolean)
        val raws = ArrayList<Raw>()
        cr.query(uri, proj, sel, null, null)?.use { c ->
            while (c.moveToNext()) {
                val eventId = c.getLong(0)
                val b = c.getLong(1)
                val e = c.getLong(2)
                val recurring = !c.getString(10).isNullOrEmpty() || !c.getString(11).isNullOrEmpty() || !c.isNull(12)
                val cal = cals[c.getLong(9)] ?: continue
                val allDay = c.getInt(6) != 0
                val rec = LinkedHashMap<String, String>()
                fun put(f: String, v: String?) { if (!v.isNullOrEmpty()) rec[f] = v }
                put("Subject", c.getString(3)?.trim())
                put("Location", c.getString(4)?.trim())
                put("Body", c.getString(5)?.trim())
                if (allDay) { put("AllDay", "1"); put("Start", date(b)); put("End", date(maxOf(e, b + DAY))) }
                else { put("Start", utc(b)); put("End", utc(maxOf(e, b))) }
                put("Busy", when (c.getInt(7)) { Events.AVAILABILITY_FREE -> "0"; Events.AVAILABILITY_TENTATIVE -> "1"; else -> "2" })
                put("Sensitivity", if (c.getInt(8) == Events.ACCESS_PRIVATE || c.getInt(8) == Events.ACCESS_CONFIDENTIAL) "2" else "0")
                put("Categories", cal.name)
                val key = if (recurring) "i:$eventId:$b" else "e:$eventId"
                raws += Raw(eventId, key, rec, c.getInt(13) != 0, cal.writable && !recurring)
            }
        }
        // Reminders: the earliest alert of each event, or none.
        val alarms = HashMap<Long, Int>()
        val withAlarm = raws.filter { it.alarm }.map { it.eventId }.distinct()
        for (chunk in withAlarm.chunked(400)) {
            cr.query(Reminders.CONTENT_URI, arrayOf(Reminders.EVENT_ID, Reminders.MINUTES, Reminders.METHOD),
                "${Reminders.EVENT_ID} IN (${chunk.joinToString(",")})", null, null)?.use { c ->
                while (c.moveToNext()) {
                    val m = c.getInt(2)
                    if (m != Reminders.METHOD_ALERT && m != Reminders.METHOD_DEFAULT) continue
                    val min = c.getInt(1).let { if (it < 0) 15 else it }  // -1: the calendar's default
                    alarms[c.getLong(0)] = maxOf(alarms[c.getLong(0)] ?: -1, min)
                }
            }
        }
        val out = HashMap<String, Item>()
        for (r in raws) {
            r.rec["Reminder"] = (alarms[r.eventId] ?: -1).toString()
            out[r.key] = Item(r.key, r.eventId, r.rec, r.writable)
        }
        return out
    }

    // ---- Writing (Windows Mobile's changes to single events) ----

    fun applyChanges(item: Item, old: Map<String, String>, new: Map<String, String>) {
        if (!item.writable) return
        val changed = (old.keys + new.keys).filter { old[it].orEmpty() != new[it].orEmpty() }.toSet()
        val v = values(new, changed, existing = true)
        if (v.size() > 0) cr.update(ContentUris.withAppendedId(Events.CONTENT_URI, item.eventId), v, null, null)
        if ("Reminder" in changed) setReminder(item.eventId, new["Reminder"]?.toIntOrNull() ?: -1)
    }

    /** An appointment made in Windows Mobile: a new event in the phone's main calendar. Returns its key. */
    fun create(rec: Map<String, String>): String? {
        val calId = defaultCalendar() ?: return null
        val v = values(rec, rec.keys + setOf("Start", "End", "AllDay"), existing = false)
        v.put(Events.CALENDAR_ID, calId)
        val uri = cr.insert(Events.CONTENT_URI, v) ?: return null
        val id = ContentUris.parseId(uri)
        setReminder(id, rec["Reminder"]?.toIntOrNull() ?: -1)
        return "e:$id"
    }

    fun delete(key: String): Boolean {
        if (!key.startsWith("e:")) return false
        val id = key.removePrefix("e:").toLongOrNull() ?: return false
        return runCatching { cr.delete(ContentUris.withAppendedId(Events.CONTENT_URI, id), null, null) > 0 }.getOrDefault(false)
    }

    private fun values(rec: Map<String, String>, changed: Set<String>, existing: Boolean) = ContentValues().apply {
        if ("Subject" in changed) put(Events.TITLE, rec["Subject"].orEmpty())
        if ("Location" in changed) put(Events.EVENT_LOCATION, rec["Location"].orEmpty())
        if ("Body" in changed) put(Events.DESCRIPTION, rec["Body"].orEmpty())
        if ("Start" in changed || "End" in changed || "AllDay" in changed) {
            val allDay = rec["AllDay"] == "1"
            val s = parse(rec["Start"].orEmpty()) ?: return@apply
            val e = parse(rec["End"].orEmpty()) ?: s
            put(Events.ALL_DAY, if (allDay) 1 else 0)
            put(Events.DTSTART, s)
            put(Events.DTEND, if (allDay && e <= s) s + DAY else e)
            putNull(Events.DURATION)
            put(Events.EVENT_TIMEZONE, if (allDay) "UTC" else TimeZone.getDefault().id)
        }
        if ("Busy" in changed) put(Events.AVAILABILITY, when (rec["Busy"]) { "0" -> Events.AVAILABILITY_FREE; "1" -> Events.AVAILABILITY_TENTATIVE; else -> Events.AVAILABILITY_BUSY })
        if ("Sensitivity" in changed) put(Events.ACCESS_LEVEL, if (rec["Sensitivity"] == "2") Events.ACCESS_PRIVATE else Events.ACCESS_DEFAULT)
        if (!existing && !containsKey(Events.TITLE)) put(Events.TITLE, "")
    }

    private fun setReminder(eventId: Long, minutes: Int) {
        cr.delete(Reminders.CONTENT_URI, "${Reminders.EVENT_ID}=? AND ${Reminders.METHOD} IN (${Reminders.METHOD_ALERT},${Reminders.METHOD_DEFAULT})",
            arrayOf(eventId.toString()))
        if (minutes < 0) return
        cr.insert(Reminders.CONTENT_URI, ContentValues().apply {
            put(Reminders.EVENT_ID, eventId); put(Reminders.MINUTES, minutes); put(Reminders.METHOD, Reminders.METHOD_ALERT)
        })
    }

    /** The phone's main writable calendar (its primary, else the first one the user owns). */
    private fun defaultCalendar(): Long? {
        cr.query(CalendarContract.Calendars.CONTENT_URI, arrayOf(CalendarContract.Calendars._ID, CalendarContract.Calendars.IS_PRIMARY,
            CalendarContract.Calendars.CALENDAR_ACCESS_LEVEL),
            "${CalendarContract.Calendars.VISIBLE}=1 AND ${CalendarContract.Calendars.CALENDAR_ACCESS_LEVEL}>=${CalendarContract.Calendars.CAL_ACCESS_CONTRIBUTOR}",
            null, "${CalendarContract.Calendars.IS_PRIMARY} DESC, ${CalendarContract.Calendars.CALENDAR_ACCESS_LEVEL} DESC, ${CalendarContract.Calendars._ID}")?.use { c ->
            if (c.moveToFirst()) return c.getLong(0)
        }
        return null
    }

    companion object {
        const val PAST_DAYS = 30L
        const val FUTURE_DAYS = 365L
        private const val DAY = 86_400_000L

        private fun fmt(p: String) = SimpleDateFormat(p, Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }
        fun utc(ms: Long): String = fmt("yyyyMMdd'T'HHmmss'Z'").format(ms)
        fun date(ms: Long): String = fmt("yyyyMMdd").format(ms)

        /** "yyyyMMdd" (UTC midnight, as Android keeps all-day events) or "yyyyMMddTHHmmssZ". */
        fun parse(s: String): Long? = runCatching {
            if (s.length >= 15) fmt("yyyyMMdd'T'HHmmss'Z'").parse(s)!!.time else fmt("yyyyMMdd").parse(s)!!.time
        }.getOrNull()
    }
}
