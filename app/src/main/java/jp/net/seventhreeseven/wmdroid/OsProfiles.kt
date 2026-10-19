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
    /** What a stored Memory of 0 boots. The board's ceiling; a ROM that already has saves keeps its own size (GuestStates). */
    val defaultMemMB: Int = 256,
    /**
     * Smallest Memory setting offered. Every image keeps its storage volume in the first 32 MB of
     * the RAM extension, so 64 MB boots with "Storage memory critically low"; 96 MB works where
     * the CLI checks ran clean (docs/research-de-hardware.md 2.1).
     */
    val minMemMB: Int = 96,
    /**
     * The storage volume survives a cold boot (WM 6.x), so a saved state can move to another memory
     * size. WM 5 clears it on a cold boot: only a screen change carries it, by a warm restart (GuestStates).
     */
    val coldBootKeepsStorage: Boolean = true,
    /**
     * Standard (keypad) editions: the screen sizes offered and the image each needs. The home
     * screen layouts of these images only cover their own size family (e.g. the 6.5.3 QVGA image
     * runs 240x320, 320x240 and 320x320, its WVGA image 480x640 and 480x800), so unlike the
     * Professional images they don't stretch to any phone shape (docs/research-standard.md).
     * Empty = any size from the one image (Professional).
     */
    val screens: List<GuestScreen> = emptyList(),
    /**
     * Tallest screen a Professional release boots at. 0 means the phone shape and the resolution
     * list are unlimited. Original 6.5 has no Titanium layout above 480×800, so the phone shape
     * (852 on a tall cover) and an explicit 854 stay at 800. WM2003 SE's bootloader accepts at
     * most 800 as well.
     */
    val maxHeight: Int = 0,
    /**
     * The PocketStylus helper on Windows Mobile 5 and later is .NET Compact Framework 3.5.
     * Windows Mobile 2003 (CE 4.2) runs the CF 2.0 build instead (cf2): autorun installs
     * NETCFv2.wce4.ARMV4.cab, then starts that helper.
     */
    val helper: Boolean = true,
    /** CE 4.2: ship the CF 2.0 helper and CAB rather than the CF 3.5 ones. */
    val cf2: Boolean = false,
) {
    /** The memory a stored Memory setting boots with: 0 (or a size no longer offered) = the default. */
    fun memFor(setting: Int) = if (setting >= minMemMB) setting.coerceAtMost(OsProfiles.MEM_SIZES.last()) else defaultMemMB

    /** The Memory settings offered for this release. */
    val memSizes get() = OsProfiles.MEM_SIZES.filter { it >= minMemMB }
}

/** A screen size a Standard release runs at, with the image variant that has its home layout. */
data class GuestScreen(val width: Int, val height: Int, val variantId: String, val label: String) {
    val id get() = "${width}x$height"
}

object OsProfiles {
    /**
     * Memory settings (MB). The board has a 64 MB bank plus an extension the OAL probes in 1 MB steps
     * (OEMGetExtensionDRAM) up to 192 MB, which every image's address table maps at 0x94000000: 256 MB
     * is the ceiling (a 320 MB board still shows 256 MB in WM 6.5.3). Each size was booted in the CLI.
     */
    val MEM_SIZES = listOf(96, 128, 160, 192, 224, 256)

    val all: List<OsProfile> = listOf(
        OsProfile(
            "wm653pro", "Windows Mobile 6.5.3 Professional", "Pocket PC phone · touch", true,
            "Windows Mobile 6.5.3 Professional",
            listOf(
                // One image: the board gives Windows Mobile its screen size at boot, so it runs at
                // any resolution (Settings > Device > Screen resolution).
                ImageVariant("fwvga", "FWVGA 480×854", "PPC_USA_GSM_FWVGA_VR.BIN", 480, 854),
                // Optional: the FWVGA image's Start menu sticks (the button stays pressed) when WM
                // runs at 480×800, e.g. under "Resize screen for dock". 480×800 boots this image
                // instead when it is installed (GuestStates.resolve).
                ImageVariant("wvga", "WVGA 480×800", "PPC_USA_GSM_WVGA_VR.BIN", 480, 800),
            ),
            minMemMB = 128,  // at 96 MB only ~3 MB program memory is left with a few programs open
        ),
        OsProfile(
            "wm65pro", "Windows Mobile 6.5 Professional", "Original 6.5 · Pocket PC phone", true,
            "Windows Mobile 6.5 Professional",
            listOf(
                // WVGA first: a full folder import boots this one. Heights above 800 stay at 800
                // (maxHeight). An exact size below uses the image built for it when that file
                // was imported too (GuestStates.resolve).
                ImageVariant("wvga", "WVGA 480×800", "PPC_USA_GSM_WVGA_VR.BIN", 480, 800),
                ImageVariant("vga", "VGA 480×640", "PPC_USA_GSM_VGA_VR.BIN", 480, 640),
                ImageVariant("wqvga", "WQVGA 240×400", "PPC_USA_GSM_WQVGA_VR.BIN", 240, 400),
                ImageVariant("square", "Square 320×320", "PPC_USA_GSM_320x320_VR.BIN", 320, 320),
                ImageVariant("qvga", "QVGA 240×320", "PPC_USA_GSM_VR.BIN", 240, 320),
            ),
            // Titanium, like 6.5.3: 96 MB leaves almost no program memory.
            minMemMB = 128,
            maxHeight = 800,
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
            minMemMB = 128,  // at 96 MB the helper (.NET CF) crashes
            coldBootKeepsStorage = false,
        ),
        OsProfile(
            "wm2003se", "Windows Mobile 2003 Second Edition", "Pocket PC · touch", true,
            "Windows Mobile 2003 SE for Pocket PC (ARMv4)",
            listOf(
                // One image. The board passes the screen size in CE 4.2 BOOT_ARGS, so QVGA, VGA
                // and the other sizes at or under 800 all boot it (checked at 240×320 and 480×640).
                ImageVariant("qvga", "QVGA 240×320", "PPC_2003_SE_WWE_ARMv4.bin", 240, 320),
            ),
            armv5 = false,
            coldBootKeepsStorage = false,
            maxHeight = 800,
            helper = true,
            cf2 = true,
        ),
        // ---- Standard (Smartphone): no touch screen, driven by the keypad skin or the phone's keys ----
        OsProfile(
            "wm653std", "Windows Mobile 6.5.3 Standard", "Smartphone · keypad", false,
            "Windows Mobile 6.5.3 Standard",
            listOf(
                ImageVariant("qvga", "QVGA 240×320", "SP_USA_GSM_QVGA_VR.bin", 240, 320),
                ImageVariant("wvga", "WVGA 480×800", "SP_USA_GSM_WVGA_VR.bin", 480, 800),
            ),
            // The WVGA image's in-call screen draws garbled (any size, also in the CLI): offered,
            // but "auto" stays on the QVGA image when both are installed.
            screens = listOf(
                GuestScreen(240, 320, "qvga", "240×320 (QVGA)"),
                GuestScreen(480, 800, "wvga", "480×800 (WVGA image, sharper; call screen glitches)"),
                GuestScreen(480, 640, "wvga", "480×640 (WVGA image, sharper; call screen glitches)"),
                GuestScreen(320, 320, "qvga", "320×320 (square)"),
                GuestScreen(320, 240, "qvga", "320×240 (landscape)"),
            ),
        ),
        OsProfile(
            "wm65std", "Windows Mobile 6.5 Standard", "Original 6.5 · Smartphone · keypad", false,
            "Windows Mobile 6.5 Standard",
            listOf(
                // The SDK ships these two only: landscape QVGA, and a square image. Each home
                // layout covers its own size (docs/research-standard.md), so they are not stretched.
                ImageVariant("qvga", "Landscape QVGA 320×240", "SP_USA_GSM_QVGA_VR.bin", 320, 240),
                ImageVariant("square", "Square 320×320", "SP_USA_GSM_320x320_VR.bin", 320, 320),
            ),
            screens = listOf(
                GuestScreen(320, 240, "qvga", "320×240 (landscape QVGA)"),
                GuestScreen(320, 320, "square", "320×320 (square)"),
            ),
        ),
        OsProfile(
            "wm614std", "Windows Mobile 6.1.4 Standard", "Latest 6.1 · Smartphone · keypad", false,
            "Windows Mobile 6.1.4 Standard",
            listOf(
                ImageVariant("qvga", "QVGA 240×320", "SP_USA_GSM_QVGA_VR.bin", 240, 320),
            ),
            screens = listOf(
                GuestScreen(240, 320, "qvga", "240×320 (QVGA)"),
                GuestScreen(320, 320, "qvga", "320×320 (square)"),
                GuestScreen(320, 240, "qvga", "320×240 (landscape)"),
            ),
        ),
        OsProfile(
            "wm5sp", "Windows Mobile 5.0 Smartphone", "Original 5.0 · Smartphone · keypad", false,
            "Windows Mobile 5.0 (Smartphone)",
            listOf(
                ImageVariant("qvga", "QVGA 240×320", "SP_USA_GSM_QVGA_VR.bin", 240, 320),
            ),
            minMemMB = 128,  // at 96 MB the helper (.NET CF) crashes
            coldBootKeepsStorage = false,
            screens = listOf(
                GuestScreen(240, 400, "qvga", "240×400 (WQVGA)"),
                GuestScreen(240, 320, "qvga", "240×320 (QVGA)"),
                GuestScreen(320, 240, "qvga", "320×240 (landscape)"),
            ),
        ),
        OsProfile(
            "wm2003sesp", "Windows Mobile 2003 SE Smartphone", "Smartphone · keypad", false,
            "Windows Mobile 2003 SE for Smartphone (ARMv4)",
            listOf(
                ImageVariant("qcif", "176×220", "SP_2003_SE_WWE_ARMv4.bin", 176, 220),
            ),
            armv5 = false,
            coldBootKeepsStorage = false,
            // This image's shell never runs \Storage Card\2577\autorun.exe (no such path in the ROM),
            // so the helper cannot start itself. The Pocket PC 2003 release does.
            helper = false,
            screens = listOf(
                GuestScreen(176, 220, "qcif", "176×220"),
            ),
        ),
    )

    fun byId(id: String?): OsProfile? = all.firstOrNull { it.id == id }

    /**
     * The screen a Standard release boots at: [choice] (Settings.screenRes) when it is one of its
     * sizes with the image installed, else ("auto") the size of the first installed image (the
     * variants' order: QVGA first, whose in-call screen works) whose shape (height / width) is
     * nearest [aspect]. Null for Professional releases.
     */
    fun standardScreen(profile: OsProfile, installed: List<ImageVariant>, choice: String, aspect: Double): GuestScreen? {
        val ok = profile.screens.filter { s -> installed.any { it.id == s.variantId } }
        if (ok.isEmpty()) return null
        ok.firstOrNull { it.id == choice }?.let { return it }
        val preferred = profile.variants.first { v -> ok.any { it.variantId == v.id } }.id
        return ok.filter { it.variantId == preferred }
            .minBy { Math.abs(Math.log(it.height.toDouble() / it.width / aspect)) }
    }
}
