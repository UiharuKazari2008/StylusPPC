package jp.net.seventhreeseven.wmdroid

/**
 * An HTC HD2 ROM the user owns (RUU_signed.nbh, os.nb or an IMGFS dump), read by the core
 * (core/src/htc/romkit.h, app/src/main/cpp/htcrom_jni.cpp). PocketStylus ships no HTC software.
 */
object HtcRom {
    init {
        System.loadLibrary("wmdroid")
    }

    /** A ROM package: [files] as its manifest lists them. */
    class Package(val name: String, val files: List<String>) {
        val exes get() = files.filter { it.endsWith(".exe", ignoreCase = true) }
        /** "Teeter" for Teeter_2_2_20203823_p0: the name up to its version. */
        val title get() = name.split('_').takeWhile { it.isEmpty() || !it[0].isDigit() }.joinToString(" ").ifEmpty { name }
    }

    /** Opens the ROM on [fd] (the core keeps its own copy of the descriptor); 0 with [err] set on failure. */
    external fun open(fd: Int, err: Array<String?>): Long
    private external fun packages(handle: Long): String
    /** Stages package [name] into [outDir] (see HtcCompat.cs); [have] = the guest's DLL list or null. The log; "error:" lines on failure. */
    external fun stage(handle: Long, name: String, outDir: String, have: String?): String
    external fun close(handle: Long)

    fun list(handle: Long): List<Package> = packages(handle).lineSequence().filter { it.isNotBlank() }.map { line ->
        val (name, files) = line.split('\t', limit = 2).let { it[0] to it.getOrElse(1) { "" } }
        Package(name, files.split('|').filter { it.isNotEmpty() })
    }.toList()
}
