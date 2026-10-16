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
                ImageVariant("fwvga", "FWVGA 480×854 (tall)", "PPC_USA_GSM_FWVGA_VR.BIN", 480, 854),
                ImageVariant("wvga", "WVGA 480×800", "PPC_USA_GSM_WVGA_VR.BIN", 480, 800),
                ImageVariant("vga", "VGA 480×640", "PPC_USA_GSM_VGA_VR.BIN", 480, 640),
                ImageVariant("wqvga", "WQVGA 240×400", "PPC_USA_GSM_WQVGA_VR.BIN", 240, 400),
                ImageVariant("qvga", "QVGA 240×320", "PPC_USA_GSM_VR.BIN", 240, 320),
                ImageVariant("vgasq", "Square 480×480", "PPC_USA_GSM_VGA_SQUARE_VR.BIN", 480, 480),
                ImageVariant("sq320", "Square 320×320", "PPC_USA_GSM_QVGA_VR.BIN", 320, 320),
            ),
            defaultMemMB = 256,
        ),
        OsProfile(
            "wm614pro", "Windows Mobile 6.1.4 Professional", "Latest 6.1 · Pocket PC phone", true,
            "Windows Mobile 6.1.4 Professional",
            listOf(
                ImageVariant("wvga", "WVGA 480×800", "PPC_USA_GSM_480x800.BIN", 480, 800),
                ImageVariant("vga", "VGA 480×640", "PPC_USA_GSM_VGA_VR.BIN", 480, 640),
                ImageVariant("wqvga", "WQVGA 240×400", "PPC_USA_GSM_240x400.BIN", 240, 400),
                ImageVariant("qvga", "QVGA 240×320", "PPC_USA_GSM_VR.BIN", 240, 320),
                ImageVariant("sq", "Square 240×240", "PPC_USA_GSM_SQUARE_VR.BIN", 240, 240),
            ),
        ),
        OsProfile(
            "wm5ppcphone", "Windows Mobile 5.0 Pocket PC Phone", "Original 5.0 · Pocket PC phone", true,
            "Windows Mobile 5.0 (Pocket PC)",
            listOf(
                ImageVariant("vga", "VGA 480×640", "PPC_USA_GSM_VGA_VR.BIN", 480, 640),
                ImageVariant("qvga", "QVGA 240×320", "PPC_USA_GSM_VR.BIN", 240, 320),
                ImageVariant("vganp", "VGA 480×640 (no phone)", "PPC_USA_VGA.bin", 480, 640),
                ImageVariant("qvganp", "QVGA 240×320 (no phone)", "PPC_USA.bin", 240, 320),
            ),
            defaultMemMB = 128,
        ),
    )

    fun byId(id: String?): OsProfile? = all.firstOrNull { it.id == id }
}
