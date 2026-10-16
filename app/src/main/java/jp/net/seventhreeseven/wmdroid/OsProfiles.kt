package jp.net.seventhreeseven.wmdroid

/** One ROM image of a Windows Mobile release, built for a particular screen. */
data class ImageVariant(
    val id: String,
    val label: String,
    val fileName: String,
    val width: Int,
    val height: Int,
)

/** A Windows Mobile release the user can pick at first start or in settings. */
data class OsProfile(
    val id: String,
    val title: String,
    val subtitle: String,
    /** Pocket PC / Professional editions have a touch screen; Smartphone / Standard do not. */
    val touch: Boolean,
    /** Folder name in the emulation images archive, used by the import helper. */
    val archiveFolder: String,
    val variants: List<ImageVariant>,
    val armv5: Boolean = true,
    val defaultMemMB: Int = 128,
)

object OsProfiles {
    val all: List<OsProfile> = listOf(
        OsProfile(
            "wm653pro", "Windows Mobile 6.5.3 Professional", "Pocket PC phone · touch", true,
            "Windows Mobile 6.5.3 Professional",
            listOf(
                // One image: the board gives Windows Mobile its screen size at boot, so it runs at
                // any resolution (Settings > Device > Screen resolution).
                ImageVariant("fwvga", "FWVGA 480×854", "PPC_USA_GSM_FWVGA_VR.BIN", 480, 854),
            ),
            defaultMemMB = 256,
        ),
        OsProfile(
            "wm614pro", "Windows Mobile 6.1.4 Professional", "Latest 6.1 · Pocket PC phone", true,
            "Windows Mobile 6.1.4 Professional",
            listOf(
                // One image: the board gives Windows Mobile its screen size at boot, so it runs at
                // any resolution (Settings > Device > Screen resolution).
                ImageVariant("wvga", "WVGA 480×800", "PPC_USA_GSM_480x800.BIN", 480, 800),
            ),
        ),
        OsProfile(
            "wm5ppcphone", "Windows Mobile 5.0 Pocket PC Phone", "Original 5.0 · Pocket PC phone", true,
            "Windows Mobile 5.0 (Pocket PC)",
            listOf(
                // One image: the board gives Windows Mobile its screen size at boot, so it runs at
                // any resolution (Settings > Device > Screen resolution).
                ImageVariant("vga", "VGA 480×640", "PPC_USA_GSM_VGA_VR.BIN", 480, 640),
            ),
            defaultMemMB = 128,
        ),
    )

    fun byId(id: String?): OsProfile? = all.firstOrNull { it.id == id }
}
