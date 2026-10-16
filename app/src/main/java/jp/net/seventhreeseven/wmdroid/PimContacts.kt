package jp.net.seventhreeseven.wmdroid

import android.content.ContentProviderOperation
import android.content.ContentUris
import android.content.Context
import android.net.Uri
import android.provider.ContactsContract
import android.provider.ContactsContract.CommonDataKinds.Email
import android.provider.ContactsContract.CommonDataKinds.Event
import android.provider.ContactsContract.CommonDataKinds.Im
import android.provider.ContactsContract.CommonDataKinds.Nickname
import android.provider.ContactsContract.CommonDataKinds.Note
import android.provider.ContactsContract.CommonDataKinds.Organization
import android.provider.ContactsContract.CommonDataKinds.Phone
import android.provider.ContactsContract.CommonDataKinds.Relation
import android.provider.ContactsContract.CommonDataKinds.StructuredName
import android.provider.ContactsContract.CommonDataKinds.StructuredPostal
import android.provider.ContactsContract.CommonDataKinds.Website
import android.provider.ContactsContract.Data

/**
 * Android contacts as Windows Mobile contact records (PimSync): one record per aggregate contact,
 * keyed "c:<lookup key>", with POOM's field names (guest/Helper/Pim.cs). WM has fixed slots, so
 * Android's rows are placed into them in a fixed order; rows that do not fit stay on Android only
 * and are never touched. Changes made in WM are written back field by field into the rows the
 * slots came from.
 */
class PimContacts(private val ctx: Context) {
    private val cr = ctx.contentResolver

    /** A contact: its record and, per WM field (or field group), the data row it came from. */
    class Item(val key: String, val rec: Map<String, String>, val rows: Map<String, Long>, val rawId: Long)

    private class Row(val id: Long, val rawId: Long, val mime: String, val d: Array<String?>, val primary: Boolean) {
        fun s(i: Int) = d[i - 1].orEmpty()
        fun type() = d[1]?.toIntOrNull() ?: 0
    }

    fun readAll(): Map<String, Item> {
        // Contacts: lookup key and the raw contact the name comes from (new rows go there).
        val contacts = HashMap<Long, Pair<String, Long>>()
        val names = HashMap<Long, String>()
        cr.query(ContactsContract.Contacts.CONTENT_URI, arrayOf(ContactsContract.Contacts._ID, ContactsContract.Contacts.LOOKUP_KEY,
            ContactsContract.Contacts.NAME_RAW_CONTACT_ID, ContactsContract.Contacts.DISPLAY_NAME,
            ContactsContract.Contacts.IN_VISIBLE_GROUP, ContactsContract.Contacts.HAS_PHONE_NUMBER), null, null, null)?.use { c ->
            while (c.moveToNext()) {
                // Contacts in a shown group or with a number (not the thousands of e-mail-only "other contacts").
                if (c.getInt(4) == 0 && c.getInt(5) == 0) continue
                val lookup = c.getString(1) ?: continue
                contacts[c.getLong(0)] = lookup to c.getLong(2)
                names[c.getLong(0)] = c.getString(3).orEmpty()
            }
        } ?: throw IllegalStateException("contacts provider unavailable")
        val rows = HashMap<Long, MutableList<Row>>()
        val proj = arrayOf(Data._ID, Data.CONTACT_ID, Data.RAW_CONTACT_ID, Data.MIMETYPE, Data.IS_SUPER_PRIMARY) +
            (1..10).map { "data$it" }
        cr.query(Data.CONTENT_URI, proj, null, null, "${Data.CONTACT_ID}, ${Data.IS_SUPER_PRIMARY} DESC, ${Data._ID}")?.use { c ->
            while (c.moveToNext()) {
                val cid = c.getLong(1)
                if (cid !in contacts) continue
                val d = Array(10) { c.getString(5 + it) }
                rows.getOrPut(cid) { ArrayList() }.add(Row(c.getLong(0), c.getLong(2), c.getString(3).orEmpty(), d, c.getInt(4) != 0))
            }
        }
        val out = HashMap<String, Item>()
        for ((cid, info) in contacts) {
            val item = build("c:" + info.first, info.second, names[cid].orEmpty(), rows[cid].orEmpty())
            if (item.rec.isNotEmpty()) out[item.key] = item
        }
        return out
    }

    fun read(key: String): Item? = runCatching { readAll()[key] }.getOrNull()

    private fun build(key: String, nameRawId: Long, displayName: String, rows: List<Row>): Item {
        val rec = LinkedHashMap<String, String>()
        val src = HashMap<String, Long>()
        fun put(f: String, v: String?) { if (!v.isNullOrBlank() && f !in rec) rec[f] = v.trim() }
        val name = rows.filter { it.mime == StructuredName.CONTENT_ITEM_TYPE }.let { n -> n.firstOrNull { it.rawId == nameRawId } ?: n.firstOrNull() }
        if (name != null) {
            src[G_NAME] = name.id
            put("FirstName", name.s(2)); put("LastName", name.s(3)); put("Title", name.s(4)); put("MiddleName", name.s(5))
            put("Suffix", name.s(6)); put("YomiFirstName", name.s(7)); put("YomiLastName", name.s(9))
        }
        val org = rows.firstOrNull { it.mime == Organization.CONTENT_ITEM_TYPE }
        if (org != null) {
            src[G_ORG] = org.id
            put("CompanyName", org.s(1)); put("JobTitle", org.s(4)); put("Department", org.s(5))
            put("YomiCompanyName", org.s(8)); put("OfficeLocation", org.s(9))
        }
        if (NAME_FIELDS.none { it in rec } && rec["CompanyName"] == null && displayName.isNotBlank()) put("FirstName", displayName)
        rows.firstOrNull { it.mime == Nickname.CONTENT_ITEM_TYPE }?.let { src["Nickname"] = it.id; put("Nickname", it.s(1)) }
        rows.firstOrNull { it.mime == Note.CONTENT_ITEM_TYPE && it.s(1).isNotBlank() }?.let { src["Body"] = it.id; put("Body", it.s(1)) }
        rows.firstOrNull { it.mime == Website.CONTENT_ITEM_TYPE }?.let { src["WebPage"] = it.id; put("WebPage", it.s(1)) }
        for (r in rows.filter { it.mime == Relation.CONTENT_ITEM_TYPE }) {
            val f = when (r.type()) { Relation.TYPE_SPOUSE, Relation.TYPE_PARTNER -> "Spouse"; Relation.TYPE_CHILD -> "Children"; else -> continue }
            if (f !in src) { src[f] = r.id; put(f, r.s(1)) }
        }
        for (r in rows.filter { it.mime == Event.CONTENT_ITEM_TYPE }) {
            val f = when (r.type()) { Event.TYPE_BIRTHDAY -> "Birthday"; Event.TYPE_ANNIVERSARY -> "Anniversary"; else -> continue }
            val date = wmDate(r.s(1)) ?: continue
            if (f !in src) { src[f] = r.id; put(f, date) }
        }
        for (r in rows.filter { it.mime == StructuredPostal.CONTENT_ITEM_TYPE }) {
            val g = when (r.type()) { StructuredPostal.TYPE_HOME -> "HomeAddress"; StructuredPostal.TYPE_WORK -> "BusinessAddress"; else -> "OtherAddress" }
            if (g in src) continue
            src[g] = r.id
            put(g + "Street", r.s(4).ifBlank { if (r.s(7).isBlank()) r.s(1) else "" }); put(g + "City", r.s(7))
            put(g + "State", r.s(8)); put(g + "PostalCode", r.s(9)); put(g + "Country", r.s(10))
        }
        // Phones into WM's slots: by type first, then whatever is left in a fixed order.
        val phones = rows.filter { it.mime == Phone.CONTENT_ITEM_TYPE && it.s(1).isNotBlank() }
        val placed = HashSet<Long>()
        fun place(slot: String, r: Row): Boolean {
            if (slot in rec) return false
            rec[slot] = r.s(1).trim(); src[slot] = r.id; placed.add(r.id)
            return true
        }
        for (r in phones) {
            val slots = when (r.type()) {
                Phone.TYPE_MOBILE -> listOf("MobileTelephoneNumber")
                Phone.TYPE_HOME -> listOf("HomeTelephoneNumber", "Home2TelephoneNumber")
                Phone.TYPE_WORK -> listOf("BusinessTelephoneNumber", "Business2TelephoneNumber")
                Phone.TYPE_FAX_WORK -> listOf("BusinessFaxNumber")
                Phone.TYPE_FAX_HOME -> listOf("HomeFaxNumber")
                Phone.TYPE_PAGER, Phone.TYPE_WORK_PAGER -> listOf("PagerNumber")
                Phone.TYPE_CAR -> listOf("CarTelephoneNumber")
                Phone.TYPE_RADIO -> listOf("RadioTelephoneNumber")
                Phone.TYPE_COMPANY_MAIN -> listOf("CompanyTelephoneNumber")
                Phone.TYPE_ASSISTANT -> listOf("AssistantTelephoneNumber")
                else -> emptyList()
            }
            slots.firstOrNull { place(it, r) }
        }
        for (r in phones) if (r.id !in placed) PHONE_FALLBACK.firstOrNull { place(it, r) }
        rows.filter { it.mime == Email.CONTENT_ITEM_TYPE && it.s(1).isNotBlank() }.take(3)
            .forEachIndexed { i, r -> rec["Email${i + 1}Address"] = r.s(1).trim(); src["Email${i + 1}Address"] = r.id }
        rows.filter { it.mime == Im.CONTENT_ITEM_TYPE && it.s(1).isNotBlank() }.take(3)
            .forEachIndexed { i, r -> rec["IM${i + 1}Address"] = r.s(1).trim(); src["IM${i + 1}Address"] = r.id }
        val rawId = nameRawId.takeIf { it > 0 } ?: rows.firstOrNull()?.rawId ?: 0L
        return Item(key, rec, src, rawId)
    }

    // ---- Writing (Windows Mobile's changes) ----

    /** WM changed fields old -> new on this contact: the matching Android rows follow. */
    fun applyChanges(item: Item, old: Map<String, String>, new: Map<String, String>) {
        val changed = (old.keys + new.keys).filter { old[it].orEmpty() != new[it].orEmpty() }.toSet()
        if (changed.isEmpty()) return
        val ops = ArrayList<ContentProviderOperation>()
        fun v(f: String) = new[f].orEmpty()
        fun row(group: String, mime: String, values: Map<String, Any?>, extra: Map<String, Any?> = emptyMap()) {
            val empty = values.values.all { it == null || it.toString().isBlank() }
            val id = item.rows[group]
            if (id != null) {
                ops += if (empty) ContentProviderOperation.newDelete(ContentUris.withAppendedId(Data.CONTENT_URI, id)).build()
                else ContentProviderOperation.newUpdate(ContentUris.withAppendedId(Data.CONTENT_URI, id)).withValues(cv(values)).build()
            } else if (!empty && item.rawId > 0) {
                ops += ContentProviderOperation.newInsert(Data.CONTENT_URI).withValue(Data.RAW_CONTACT_ID, item.rawId)
                    .withValue(Data.MIMETYPE, mime).withValues(cv(values + extra)).build()
            }
        }
        if (changed.any { it in NAME_FIELDS })
            row(G_NAME, StructuredName.CONTENT_ITEM_TYPE, mapOf(StructuredName.DISPLAY_NAME to null,
                StructuredName.GIVEN_NAME to v("FirstName"), StructuredName.FAMILY_NAME to v("LastName"),
                StructuredName.PREFIX to v("Title"), StructuredName.MIDDLE_NAME to v("MiddleName"), StructuredName.SUFFIX to v("Suffix"),
                StructuredName.PHONETIC_GIVEN_NAME to v("YomiFirstName"), StructuredName.PHONETIC_FAMILY_NAME to v("YomiLastName")))
        if (changed.any { it in ORG_FIELDS })
            row(G_ORG, Organization.CONTENT_ITEM_TYPE, mapOf(Organization.COMPANY to v("CompanyName"),
                Organization.TITLE to v("JobTitle"), Organization.DEPARTMENT to v("Department"),
                Organization.PHONETIC_NAME to v("YomiCompanyName"), Organization.OFFICE_LOCATION to v("OfficeLocation")),
                mapOf(Organization.TYPE to Organization.TYPE_WORK))
        if ("Nickname" in changed) row("Nickname", Nickname.CONTENT_ITEM_TYPE, mapOf(Nickname.NAME to v("Nickname")))
        if ("Body" in changed) row("Body", Note.CONTENT_ITEM_TYPE, mapOf(Note.NOTE to v("Body")))
        if ("WebPage" in changed) row("WebPage", Website.CONTENT_ITEM_TYPE, mapOf(Website.URL to v("WebPage")), mapOf(Website.TYPE to Website.TYPE_HOMEPAGE))
        if ("Spouse" in changed) row("Spouse", Relation.CONTENT_ITEM_TYPE, mapOf(Relation.NAME to v("Spouse")), mapOf(Relation.TYPE to Relation.TYPE_SPOUSE))
        if ("Children" in changed) row("Children", Relation.CONTENT_ITEM_TYPE, mapOf(Relation.NAME to v("Children")), mapOf(Relation.TYPE to Relation.TYPE_CHILD))
        for ((f, type) in listOf("Birthday" to Event.TYPE_BIRTHDAY, "Anniversary" to Event.TYPE_ANNIVERSARY))
            if (f in changed) row(f, Event.CONTENT_ITEM_TYPE, mapOf(Event.START_DATE to androidDate(v(f))), mapOf(Event.TYPE to type))
        for ((g, type) in listOf("HomeAddress" to StructuredPostal.TYPE_HOME, "BusinessAddress" to StructuredPostal.TYPE_WORK,
                "OtherAddress" to StructuredPostal.TYPE_OTHER))
            if (changed.any { it.startsWith(g) })
                row(g, StructuredPostal.CONTENT_ITEM_TYPE, mapOf(StructuredPostal.FORMATTED_ADDRESS to null,
                    StructuredPostal.STREET to v(g + "Street"), StructuredPostal.CITY to v(g + "City"),
                    StructuredPostal.REGION to v(g + "State"), StructuredPostal.POSTCODE to v(g + "PostalCode"),
                    StructuredPostal.COUNTRY to v(g + "Country")), mapOf(StructuredPostal.TYPE to type))
        for ((slot, type) in PHONE_TYPES) if (slot in changed)
            row(slot, Phone.CONTENT_ITEM_TYPE, mapOf(Phone.NUMBER to v(slot)), mapOf(Phone.TYPE to type))
        for (i in 1..3) {
            if ("Email${i}Address" in changed) row("Email${i}Address", Email.CONTENT_ITEM_TYPE, mapOf(Email.ADDRESS to v("Email${i}Address")),
                mapOf(Email.TYPE to Email.TYPE_OTHER))
            if ("IM${i}Address" in changed) row("IM${i}Address", Im.CONTENT_ITEM_TYPE, mapOf(Im.DATA to v("IM${i}Address")),
                mapOf(Im.TYPE to Im.TYPE_OTHER, Im.PROTOCOL to Im.PROTOCOL_CUSTOM, Im.CUSTOM_PROTOCOL to "IM"))
        }
        if (ops.isNotEmpty()) cr.applyBatch(ContactsContract.AUTHORITY, ops)
    }

    /** A contact made in Windows Mobile: a new phone-local contact. Returns its key. */
    fun create(rec: Map<String, String>): String? {
        val ops = arrayListOf(ContentProviderOperation.newInsert(ContactsContract.RawContacts.CONTENT_URI)
            .withValue(ContactsContract.RawContacts.ACCOUNT_TYPE, null).withValue(ContactsContract.RawContacts.ACCOUNT_NAME, null).build())
        val uri = cr.applyBatch(ContactsContract.AUTHORITY, ops).firstOrNull()?.uri ?: return null
        val rawId = ContentUris.parseId(uri)
        // Every field is an insert into the new raw contact (no rows yet).
        applyChanges(Item("", emptyMap(), emptyMap(), rawId), emptyMap(), rec)
        return keyOfRaw(rawId)
    }

    private fun keyOfRaw(rawId: Long): String? {
        val cid = cr.query(ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI, rawId),
            arrayOf(ContactsContract.RawContacts.CONTACT_ID), null, null, null)?.use { if (it.moveToFirst()) it.getLong(0) else null } ?: return null
        return cr.query(ContentUris.withAppendedId(ContactsContract.Contacts.CONTENT_URI, cid),
            arrayOf(ContactsContract.Contacts.LOOKUP_KEY), null, null, null)?.use { if (it.moveToFirst()) "c:" + it.getString(0) else null }
    }

    /** Deleted in Windows Mobile (PimSync guards how many at once). */
    fun delete(key: String): Boolean = runCatching {
        cr.delete(Uri.withAppendedPath(ContactsContract.Contacts.CONTENT_LOOKUP_URI, Uri.encode(key.removePrefix("c:"))), null, null) > 0
    }.getOrDefault(false)

    private fun cv(m: Map<String, Any?>) = android.content.ContentValues().apply {
        for ((k, v) in m) when (v) {
            null -> putNull(k)
            is Int -> put(k, v)
            is Long -> put(k, v)
            else -> put(k, v.toString().ifBlank { null })
        }
    }

    companion object {
        private const val G_NAME = "#name"
        private const val G_ORG = "#org"
        val NAME_FIELDS = setOf("FirstName", "MiddleName", "LastName", "Title", "Suffix", "YomiFirstName", "YomiLastName")
        val ORG_FIELDS = setOf("CompanyName", "YomiCompanyName", "Department", "JobTitle", "OfficeLocation")
        private val PHONE_FALLBACK = listOf("MobileTelephoneNumber", "HomeTelephoneNumber", "BusinessTelephoneNumber",
            "Home2TelephoneNumber", "Business2TelephoneNumber", "CarTelephoneNumber", "RadioTelephoneNumber",
            "CompanyTelephoneNumber", "AssistantTelephoneNumber", "PagerNumber")
        /** WM's phone slots and the Android type a number typed into each gets. */
        private val PHONE_TYPES = listOf("MobileTelephoneNumber" to Phone.TYPE_MOBILE, "HomeTelephoneNumber" to Phone.TYPE_HOME,
            "Home2TelephoneNumber" to Phone.TYPE_HOME, "BusinessTelephoneNumber" to Phone.TYPE_WORK,
            "Business2TelephoneNumber" to Phone.TYPE_WORK, "HomeFaxNumber" to Phone.TYPE_FAX_HOME,
            "BusinessFaxNumber" to Phone.TYPE_FAX_WORK, "PagerNumber" to Phone.TYPE_PAGER, "CarTelephoneNumber" to Phone.TYPE_CAR,
            "RadioTelephoneNumber" to Phone.TYPE_RADIO, "CompanyTelephoneNumber" to Phone.TYPE_COMPANY_MAIN,
            "AssistantTelephoneNumber" to Phone.TYPE_ASSISTANT)

        /** Android's "yyyy-MM-dd" or year-less "--MM-dd" -> WM's "yyyy-MM-dd" (1804 stands for no year: POOM refuses years before 1753). */
        fun wmDate(s: String): String? {
            val t = s.trim()
            Regex("""^(\d{4})-(\d{2})-(\d{2})""").find(t)?.let { return it.value }
            Regex("""^--(\d{2})-(\d{2})""").find(t)?.let { return "1804-${it.groupValues[1]}-${it.groupValues[2]}" }
            return null
        }

        fun androidDate(s: String): String = if (s.startsWith("1804-")) "--" + s.substring(5) else s
    }
}
