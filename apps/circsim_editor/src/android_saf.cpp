// The SAF bridge's android half (see android_saf.hpp for the contract
// and saf_store.hpp for the data policy). Everything Android about the
// editor's file pickers lives in this TU: the intent launch through
// the CircsimActivity shim, the result callback the shim answers into,
// and the ContentResolver byte moves both directions.

#include "android_saf.hpp"

#ifdef __ANDROID__

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>

#include <jni.h>

#include <ws/core/log.hpp>
#include <ws/platform/errors.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/paths.hpp>

#include <android_native_app_glue.h>

#include "saf_store.hpp"

namespace editor::saf {

namespace {

using ws::core::Status;
using ws::core::make_error;
using ws::platform::PlatformError;

// The shim's request codes (CircsimActivity.java spells the pair).
constexpr jint kRequestOpen = 1;
constexpr jint kRequestSave = 2;

[[nodiscard]] Status saf_fail(PlatformError code, const std::string& what) {
    return ws::core::unexpected<ws::core::Error>(make_error(
        static_cast<std::int32_t>(code), "platform", what));
}

// --- the JNI scaffolding (the window layer's own shape) ---------------------

// One hop's thread state: the worker is not the Java thread, so each
// JNI entry attaches for its span and detaches after.
struct ScopedJavaThread {
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    bool attached = false;

    explicit ScopedJavaThread(JavaVM* v) : vm(v) {
        if (vm == nullptr) {
            return;
        }
        const jint state =
            vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (state == JNI_OK) {
            return;
        }
        if (state == JNI_EDETACHED &&
            vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached = true;
        }
    }
    ~ScopedJavaThread() {
        if (attached) {
            (void)vm->DetachCurrentThread();
        }
    }
    [[nodiscard]] bool ok() const { return env != nullptr; }
};

// A pending Java exception, if any: described, cleared and logged - a
// failed hop degrades, never aborts.
bool jni_threw(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionDescribe();
    env->ExceptionClear();
    ws::core::error("saf: {}", what);
    return true;
}

[[nodiscard]] std::string jstring_to_std(JNIEnv* env, jstring text) {
    if (text == nullptr) {
        return {};
    }
    const char* bytes = env->GetStringUTFChars(text, nullptr);
    if (bytes == nullptr) {
        return {};
    }
    std::string out{bytes};
    env->ReleaseStringUTFChars(text, bytes);
    return out;
}

// The activity's ContentResolver, or null.
[[nodiscard]] jobject content_resolver(JNIEnv* env, jobject activity) {
    jmethodID const get = env->GetMethodID(
        env->GetObjectClass(activity), "getContentResolver",
        "()Landroid/content/ContentResolver;");
    jobject const resolver =
        get != nullptr ? env->CallObjectMethod(activity, get) : nullptr;
    if (jni_threw(env, "getContentResolver threw")) {
        return nullptr;
    }
    return resolver;
}

// android.net.Uri.parse(uri) - the string URI the map stores, reified
// for the resolver calls.
[[nodiscard]] jobject parse_uri(JNIEnv* env, const std::string& uri) {
    jclass const cls = env->FindClass("android/net/Uri");
    jmethodID const parse = cls != nullptr
                                ? env->GetStaticMethodID(
                                      cls, "parse",
                                      "(Ljava/lang/String;)Landroid/net/Uri;")
                                : nullptr;
    jstring const text = env->NewStringUTF(uri.c_str());
    jobject const parsed =
        parse != nullptr
            ? env->CallStaticObjectMethod(cls, parse, text)
            : nullptr;
    if (jni_threw(env, "Uri.parse threw")) {
        return nullptr;
    }
    return parsed;
}

// A picked document's whole content, through
// ContentResolver.openInputStream + InputStream.read(byte[]). A
// schematic is kilobytes-to-megabytes; the whole-string read is the
// document seam's own shape (the FileSystem's read answers a whole
// string too).
[[nodiscard]] std::optional<std::string> read_uri_to_string(
    JNIEnv* env, jobject activity, const std::string& uri) {
    jobject const resolver = content_resolver(env, activity);
    if (resolver == nullptr) {
        return std::nullopt;
    }
    jobject const parsed = parse_uri(env, uri);
    if (parsed == nullptr) {
        return std::nullopt;
    }
    jmethodID const open_input = env->GetMethodID(
        env->GetObjectClass(resolver), "openInputStream",
        "(Landroid/net/Uri;)Ljava/io/InputStream;");
    jobject const stream =
        open_input != nullptr
            ? env->CallObjectMethod(resolver, open_input, parsed)
            : nullptr;
    if (jni_threw(env, "openInputStream threw (the grant or the provider)")) {
        return std::nullopt;
    }
    if (stream == nullptr) {
        return std::nullopt;
    }
    jmethodID const read = env->GetMethodID(env->GetObjectClass(stream),
                                            "read", "([B)I");
    jbyteArray const buffer =
        read != nullptr ? env->NewByteArray(64 * 1024) : nullptr;
    std::string out;
    bool failed = buffer == nullptr;
    while (!failed) {
        const jint got = env->CallIntMethod(stream, read, buffer);
        if (jni_threw(env, "InputStream.read threw")) {
            failed = true;
            break;
        }
        if (got <= 0) {
            break;  // EOF (0) or the provider's tired answer (-1)
        }
        const std::size_t was = out.size();
        out.resize(was + static_cast<std::size_t>(got));
        env->GetByteArrayRegion(buffer, 0, got,
                                reinterpret_cast<jbyte*>(out.data() + was));
        if (jni_threw(env, "GetByteArrayRegion threw")) {
            failed = true;
        }
    }
    jmethodID const close = env->GetMethodID(env->GetObjectClass(stream),
                                             "close", "()V");
    if (close != nullptr) {
        env->CallVoidMethod(stream, close);
    }
    (void)jni_threw(env, "InputStream.close threw");
    if (failed) {
        return std::nullopt;
    }
    return out;
}

// A saved document's bytes to its URI, through
// ContentResolver.openOutputStream(uri, "wt") - the "wt" truncates, so
// the document's new whole content replaces its old one.
[[nodiscard]] Status push_string_to_uri(JNIEnv* env, jobject activity,
                                        const std::string& uri,
                                        std::string_view bytes) {
    jobject const resolver = content_resolver(env, activity);
    if (resolver == nullptr) {
        return saf_fail(PlatformError::io_failed,
                        "no ContentResolver for the write");
    }
    jobject const parsed = parse_uri(env, uri);
    if (parsed == nullptr) {
        return saf_fail(PlatformError::io_failed, "cannot parse " + uri);
    }
    jmethodID const open_output = env->GetMethodID(
        env->GetObjectClass(resolver), "openOutputStream",
        "(Landroid/net/Uri;Ljava/lang/String;)Ljava/io/OutputStream;");
    jstring const mode = env->NewStringUTF("wt");
    jobject const stream =
        open_output != nullptr
            ? env->CallObjectMethod(resolver, open_output, parsed, mode)
            : nullptr;
    if (jni_threw(env, "openOutputStream threw (the grant or the provider)")) {
        return saf_fail(PlatformError::io_failed,
                        "the provider refused the write to " + uri);
    }
    if (stream == nullptr) {
        return saf_fail(PlatformError::io_failed,
                        "no stream for " + uri);
    }
    jbyteArray const payload =
        env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (payload == nullptr) {
        return saf_fail(PlatformError::io_failed, "cannot stage the bytes");
    }
    env->SetByteArrayRegion(payload, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte*>(bytes.data()));
    jmethodID const write =
        env->GetMethodID(env->GetObjectClass(stream), "write", "([B)V");
    if (write != nullptr) {
        env->CallVoidMethod(stream, write, payload);
    }
    const bool threw =
        write == nullptr || jni_threw(env, "OutputStream.write threw");
    jmethodID const close = env->GetMethodID(env->GetObjectClass(stream),
                                             "close", "()V");
    if (close != nullptr) {
        env->CallVoidMethod(stream, close);
    }
    (void)jni_threw(env, "OutputStream.close threw");
    if (threw) {
        return saf_fail(PlatformError::io_failed,
                        "the write to " + uri + " failed");
    }
    return {};
}

// --- the bridge -------------------------------------------------------------

// The whole bridge state. The fs stack (base + map + the write-through
// wrapper) is heap-built at init because WriteThroughFs's push hook
// captures the activity handle, which init receives.
struct Bridge {
    android_app* app = nullptr;
    std::string sandbox_dir;
    // Set only by a fully successful init (the natives registered): a
    // half-wired bridge - a failed sandbox or registration - serves no
    // pickers, because the shim's answer would never arrive.
    bool ready = false;

    // The pending-pick slot: the worker fills `open`, the Java thread
    // fills the answer, one lock over both.
    std::mutex mutex;
    std::condition_variable cv;
    bool picker_live = false;
    bool answer_done = false;
    bool answer_ok = false;
    std::string answer_uri;
    std::string answer_name;

    ws::settings::StdFileSystem base;
    SafMap map;
    std::unique_ptr<WriteThroughFs> fs_wrapper;

    [[nodiscard]] jobject activity() const {
        return app != nullptr ? app->activity->clazz : nullptr;
    }
    [[nodiscard]] JavaVM* vm() const {
        return app != nullptr ? app->activity->vm : nullptr;
    }
};

Bridge& bridge() {
    static Bridge instance;
    return instance;
}

// The shim's answer, on the activity's Java thread: fill the slot and
// wake the waiter. The registration (init) is what binds the name.
void native_picker_result(JNIEnv* env, jobject /*activity*/, jint /*code*/,
                          jboolean ok, jstring uri, jstring name) {
    Bridge& b = bridge();
    const std::lock_guard lock{b.mutex};
    b.answer_done = true;
    b.answer_ok = ok == JNI_TRUE;
    b.answer_uri = jstring_to_std(env, uri);
    b.answer_name = jstring_to_std(env, name);
    b.cv.notify_all();
}

// CircsimActivity.launchPicker(code, save, suggested) - the shim hops
// to its UI thread and fires the intent.
[[nodiscard]] Status launch_picker(jint code, bool save,
                                   const std::string& suggested) {
    Bridge& b = bridge();
    ScopedJavaThread java{b.vm()};
    if (!java.ok()) {
        return saf_fail(PlatformError::unavailable,
                        "no JNIEnv for the picker launch");
    }
    JNIEnv* const env = java.env;
    jobject const activity = b.activity();
    jmethodID const launch = env->GetMethodID(
        env->GetObjectClass(activity), "launchPicker",
        "(IZLjava/lang/String;)V");
    if (launch == nullptr) {
        // The manifest's shim did not come along (an old APK, a hand
        // merge) - the honest refusal, named.
        return saf_fail(PlatformError::unavailable,
                        "the activity ships no launchPicker - is the "
                        "CircsimActivity shim in the APK?");
    }
    jstring const name = env->NewStringUTF(suggested.c_str());
    env->CallVoidMethod(activity, launch, code,
                        save ? JNI_TRUE : JNI_FALSE, name);
    if (jni_threw(env, "launchPicker threw")) {
        return saf_fail(PlatformError::io_failed, "the picker launch failed");
    }
    return {};
}

// The blocking pick: launch, then wait in 200 ms ticks - long enough to
// notice the activity dying (destroyRequested - the glue set it on the
// way out, which is the only shutdown this waiter sees), short enough
// that shutdown never waits a meaningful moment for it.
struct PickAnswer {
    bool ok = false;
    bool dismissed = false;
    std::string uri;
    std::string name;
};

[[nodiscard]] ws::core::Result<PickAnswer> run_pick(jint code, bool save,
                                                    const std::string& name) {
    Bridge& b = bridge();
    {
        const std::lock_guard lock{b.mutex};
        if (b.picker_live) {
            return ws::core::unexpected<ws::core::Error>(make_error(
                static_cast<std::int32_t>(PlatformError::invalid_argument),
                "platform", "a picker is already open"));
        }
        b.picker_live = true;
        b.answer_done = false;
        b.answer_ok = false;
        b.answer_uri.clear();
        b.answer_name.clear();
    }
    const Status launched = launch_picker(code, save, name);
    if (!launched) {
        const std::lock_guard lock{b.mutex};
        b.picker_live = false;
        return ws::core::unexpected<ws::core::Error>(launched.error());
    }
    PickAnswer answer;
    {
        std::unique_lock lock{b.mutex};
        while (!b.answer_done) {
            if (b.app->destroyRequested != 0) {
                // The activity is going down; the answer never comes.
                b.picker_live = false;
                answer.dismissed = true;
                return answer;
            }
            b.cv.wait_for(lock, std::chrono::milliseconds(200));
        }
        answer.ok = b.answer_ok;
        answer.uri = b.answer_uri;
        answer.name = b.answer_name;
        b.picker_live = false;
    }
    if (!answer.ok || answer.uri.empty()) {
        answer.dismissed = true;  // the user backed out: a normal answer
    }
    return answer;
}

// --- the dialogs ------------------------------------------------------------

// The SAF-backed FileDialogs: the shell's blocking contract honoured
// on a worker (its own dispatch runs these there).
class SafDialogs final : public ws::platform::FileDialogs {
  public:
    [[nodiscard]] ws::core::Result<std::optional<std::string>> open_file(
        const ws::platform::FileDialogOptions& options) override {
        (void)options;  // the SAF picker filters nothing: every provider
                        // document shows, the factory's .csim gate refuses
        Bridge& b = bridge();
        if (b.app == nullptr) {
            return ws::core::unexpected<ws::core::Error>(make_error(
                static_cast<std::int32_t>(PlatformError::unavailable),
                "platform", "the SAF bridge is not initialized"));
        }
        const ws::core::Result<PickAnswer> picked =
            run_pick(kRequestOpen, false, "");
        if (!picked) {
            return ws::core::unexpected<ws::core::Error>(picked.error());
        }
        if (picked->dismissed) {
            return std::optional<std::string>{};
        }
        // Copy the picked document into the sandbox (a real file every
        // path-shaped thing can read), then remember the URI it
        // mirrors - in that order, so the copy itself never pushes
        // back what it just read.
        const std::string display =
            picked->name.empty() ? "schematic" : picked->name;
        const std::string path = ws::platform::join_path(
            b.sandbox_dir,
            sandbox_name_for(display, picked->uri, "schematic"));
        ScopedJavaThread java{b.vm()};
        if (!java.ok()) {
            return ws::core::unexpected<ws::core::Error>(make_error(
                static_cast<std::int32_t>(PlatformError::io_failed),
                "platform", "no JNIEnv for the copy"));
        }
        const std::optional<std::string> bytes =
            read_uri_to_string(java.env, b.activity(), picked->uri);
        if (!bytes.has_value()) {
            return ws::core::unexpected<ws::core::Error>(make_error(
                static_cast<std::int32_t>(PlatformError::io_failed),
                "platform", "cannot read '" + display + "'"));
        }
        const Status written = b.fs_wrapper->write(path, *bytes);
        if (!written) {
            return ws::core::unexpected<ws::core::Error>(written.error());
        }
        b.fs_wrapper->map(path, picked->uri);
        return path;
    }

    [[nodiscard]] ws::core::Result<std::vector<std::string>> open_files(
        const ws::platform::FileDialogOptions& options) override {
        // The SAF picker answers one document per round; the shell's
        // multi-open is the desktop drop's shape, and one pick per
        // command is the phone's honest answer.
        const ws::core::Result<std::optional<std::string>> picked =
            open_file(options);
        if (!picked) {
            return ws::core::unexpected<ws::core::Error>(picked.error());
        }
        std::vector<std::string> out;
        if (picked->has_value()) {
            out.push_back(**picked);
        }
        return out;
    }

    [[nodiscard]] ws::core::Result<std::optional<std::string>> save_file(
        const ws::platform::FileDialogOptions& options) override {
        Bridge& b = bridge();
        if (b.app == nullptr) {
            return ws::core::unexpected<ws::core::Error>(make_error(
                static_cast<std::int32_t>(PlatformError::unavailable),
                "platform", "the SAF bridge is not initialized"));
        }
        // The suggested name: the options' default_path's last segment
        // when a caller offers one, the plain default otherwise (the
        // shell passes only a title today).
        std::string suggested = "schematic.csim";
        if (!options.default_path.empty()) {
            const std::string leaf =
                ws::platform::name_of(options.default_path);
            if (!leaf.empty()) {
                suggested = leaf;
            }
        }
        const ws::core::Result<PickAnswer> picked =
            run_pick(kRequestSave, true, suggested);
        if (!picked) {
            return ws::core::unexpected<ws::core::Error>(picked.error());
        }
        if (picked->dismissed) {
            return std::optional<std::string>{};
        }
        // No copy: the document's save writes the sandbox file, and the
        // write-through seam pushes those bytes to the URI - the first
        // save lands both places, every later save refreshes both.
        const std::string display = picked->name.empty() ? suggested
                                                         : picked->name;
        const std::string path = ws::platform::join_path(
            b.sandbox_dir,
            sandbox_name_for(display, picked->uri, "schematic"));
        b.fs_wrapper->map(path, picked->uri);
        return path;
    }

    [[nodiscard]] ws::core::Result<std::optional<std::string>> select_folder(
        const ws::platform::FileDialogOptions& /*options*/) override {
        // Nothing in the editor asks for a folder; the refusal is the
        // documented shape until something does.
        return ws::core::unexpected<ws::core::Error>(make_error(
            static_cast<std::int32_t>(PlatformError::unavailable),
            "platform", "select_folder: the SAF port answers open/save"));
    }
};

}  // namespace

// --- the public seam -----------------------------------------------------------

bool init(android_app* app) {
    if (app == nullptr || app->activity == nullptr) {
        ws::core::error("saf: no android_app to wire the bridge to");
        return false;
    }
    Bridge& b = bridge();
    if (b.ready) {
        return true;  // idempotent: a wired bridge stays wired
    }
    b.app = app;

    // The sandbox and the map, under the same app dir the LocationRoots
    // build uses (the locations contract's own tree).
    const char* const internal = app->activity->internalDataPath;
    if (internal == nullptr) {
        ws::core::error("saf: the activity has no internal data path");
        return false;
    }
    const std::string app_dir =
        ws::platform::join_path(internal, "circsim editor");
    b.sandbox_dir = ws::platform::join_path(app_dir, "schematics");
    if (const Status made = ws::platform::ensure_directory(b.sandbox_dir);
        !made) {
        ws::core::error("saf: cannot create the sandbox: {}",
                        made.error().message);
        return false;
    }

    // The write-through stack: the base seam, the persisted map, and
    // the wrapper whose push hook owns the JNI half.
    const std::string map_path =
        ws::platform::join_path(app_dir, "saf-map.txt");
    if (const auto bytes = b.base.read(map_path)) {
        b.map.load(*bytes);
    }
    b.fs_wrapper = std::make_unique<WriteThroughFs>(
        b.base, b.map,
        [](const std::string& uri, std::string_view bytes) {
            Bridge& inner = bridge();
            ScopedJavaThread java{inner.vm()};
            if (!java.ok()) {
                return saf_fail(PlatformError::io_failed,
                                "no JNIEnv for the push");
            }
            return push_string_to_uri(java.env, inner.activity(), uri,
                                      bytes);
        },
        map_path);

    // The natives the shim answers into: bound to the activity's class
    // (a process-lifetime registration), by name - no exported-symbol
    // mangling to keep in step with the package.
    {
        ScopedJavaThread java{b.vm()};
        if (!java.ok()) {
            ws::core::error("saf: no JNIEnv for the native registration");
            return false;
        }
        JNIEnv* const env = java.env;
        jclass const cls = env->GetObjectClass(b.activity());
        const JNINativeMethod methods[] = {
            {"nativePickerResult",
             "(IZLjava/lang/String;Ljava/lang/String;)V",
             reinterpret_cast<void*>(&native_picker_result)},
        };
        if (env->RegisterNatives(cls, methods, 1) != JNI_OK) {
            (void)jni_threw(env, "RegisterNatives(nativePickerResult) "
                                 "failed - is the activity a "
                                 "CircsimActivity?");
            return false;
        }
    }
    b.ready = true;
    return true;
}

ws::platform::FileDialogs* dialogs() {
    static SafDialogs the_dialogs;
    return bridge().ready ? &the_dialogs : nullptr;
}

ws::settings::FileSystem* fs() {
    return bridge().fs_wrapper.get();
}

std::optional<std::string> take_push_error() {
    Bridge& b = bridge();
    return b.fs_wrapper != nullptr ? b.fs_wrapper->take_push_error()
                                   : std::nullopt;
}

}  // namespace editor::saf

#endif  // __ANDROID__
