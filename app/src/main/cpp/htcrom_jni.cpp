// JNI for installing apps from the user's HTC ROM (HtcRom.kt <-> core/src/htc/romkit.h).
#include <jni.h>
#include <unistd.h>

#include <set>
#include <sstream>
#include <string>

#include "htc/romkit.h"

namespace {

struct Handle {
    int fd;
    htc::Rom rom;
};

std::string str(JNIEnv *env, jstring s) {
    if (!s) return std::string();
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string r(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

}  // namespace

extern "C" {

/** Opens the ROM on [fd] (a copy is kept; the caller may close its own). Returns 0 and [err[0]] on failure. */
JNIEXPORT jlong JNICALL Java_jp_net_seventhreeseven_wmdroid_HtcRom_open(JNIEnv *env, jobject, jint fd, jobjectArray err) {
    auto *h = new Handle{dup(fd), {}};
    std::string e;
    if (h->fd < 0 || !h->rom.open(h->fd, &e)) {
        if (err && env->GetArrayLength(err) > 0) env->SetObjectArrayElement(err, 0, env->NewStringUTF(e.empty() ? "can't open" : e.c_str()));
        if (h->fd >= 0) close(h->fd);
        delete h;
        return 0;
    }
    return reinterpret_cast<jlong>(h);
}

/** One line per package: name, tab, its files separated by '|'. */
JNIEXPORT jstring JNICALL Java_jp_net_seventhreeseven_wmdroid_HtcRom_packages(JNIEnv *env, jobject, jlong hp) {
    auto *h = reinterpret_cast<Handle *>(hp);
    std::ostringstream out;
    if (h)
        for (auto &p : h->rom.packages()) {
            out << p.name << '\t';
            for (size_t i = 0; i < p.files.size(); i++) out << (i ? "|" : "") << p.files[i];
            out << '\n';
        }
    return env->NewStringUTF(out.str().c_str());
}

/** Stages [name] into [outDir]; [have]: the guest's DLL names, one per line, or null. Returns the log ("error: ..." lines on failure). */
JNIEXPORT jstring JNICALL Java_jp_net_seventhreeseven_wmdroid_HtcRom_stage(JNIEnv *env, jobject, jlong hp, jstring name, jstring outDir,
                                                                          jstring have) {
    auto *h = reinterpret_cast<Handle *>(hp);
    if (!h) return env->NewStringUTF("error: no ROM");
    std::set<std::string> names;
    if (have) {
        std::istringstream in(str(env, have));
        for (std::string l; std::getline(in, l);) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            for (char &c : l) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (!l.empty() && l[0] != '#') names.insert(l);
        }
    }
    std::string log;
    if (!h->rom.stage(str(env, name), str(env, outDir), have ? &names : nullptr, &log) && log.find("error:") == std::string::npos)
        log += "error: staging failed\n";
    return env->NewStringUTF(log.c_str());
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_HtcRom_close(JNIEnv *, jobject, jlong hp) {
    auto *h = reinterpret_cast<Handle *>(hp);
    if (!h) return;
    close(h->fd);
    delete h;
}

}  // extern "C"
