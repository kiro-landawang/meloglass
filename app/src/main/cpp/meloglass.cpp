// MeloGlass C++ core: ports meloglass-api/server.py to on-device NDK code.
// Entry: Java_com_meloglass_app_MainActivity_00024Bridge_api
// Transport: JNI -> MainActivity$Http.request (platform TLS stack)
// Storage:   JNI -> MainActivity$Store.read/write (filesDir)
// JSON:      cJSON (vendored)

#include <jni.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include "cJSON.h"

// ---------------------------------------------------------------- utils

static std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char) c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<uint8_t> b64decode(const std::string &in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    int val = 0, valb = -8;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') break;
        int v = b64_val(c);
        if (v < 0) continue;
        val = (val << 6) | v;
        valb += 6;
        if (valb >= 0) {
            out.push_back((char) ((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static int hash33(const std::string &s) {
    long long e = 0;
    for (unsigned char c : s) {
        e += (e << 5) + (long long) c;
    }
    return (int) (e & 2147483647LL);
}

static std::string jstr2(JNIEnv *env, jstring js) {
    if (!js) return "";
    const char *p = env->GetStringUTFChars(js, nullptr);
    std::string s = p ? p : "";
    if (p) env->ReleaseStringUTFChars(js, p);
    return s;
}

// ---- cJSON helpers ----
static std::string Jstr(cJSON *obj, const char *key, const std::string &def = "") {
    if (!obj) return def;
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it || !cJSON_IsString(it)) return def;
    return cJSON_GetStringValue(it) ? std::string(cJSON_GetStringValue(it)) : def;
}

static double Jnum(cJSON *obj, const char *key, double def = 0) {
    if (!obj) return def;
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it || !cJSON_IsNumber(it)) return def;
    return cJSON_GetNumberValue(it) ? cJSON_GetNumberValue(it) : def;
}

static bool Jbool(cJSON *obj, const char *key) {
    if (!obj) return false;
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return it && cJSON_IsTrue(it);
}

static cJSON *Jobj(cJSON *obj, const char *key) {
    if (!obj) return nullptr;
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (it && cJSON_IsObject(it)) ? it : nullptr;
}

static cJSON *Jarr(cJSON *obj, const char *key) {
    if (!obj) return nullptr;
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (it && cJSON_IsArray(it)) ? it : nullptr;
}

static std::string Jprint(cJSON *obj) {
    if (!obj) return "{}";
    char *p = cJSON_PrintUnformatted(obj);
    std::string s = p ? p : "{}";
    if (p) cJSON_free(p);
    return s;
}

// strip jsonp wrapper then parse
static cJSON *loads_maybe(const std::string &text) {
    std::string t = text;
    // trim
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return nullptr;
    t = t.substr(a);
    if (t.rfind("callback", 0) == 0 || t.rfind("jsonCallback", 0) == 0) {
        size_t lp = t.find('(');
        size_t rp = t.rfind(')');
        if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
            t = t.substr(lp + 1, rp - lp - 1);
        }
    }
    return cJSON_Parse(t.c_str());
}

// ---------------------------------------------------------------- JNI transport

static JavaVM *g_vm = nullptr;

jint JNI_OnLoad(JavaVM *vm, void *) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

struct Up {
    int st = 0;
    std::string text;        // decoded utf-8 body
    std::string b64;         // raw body base64 (for binary responses)
    std::string url;         // final URL after redirects
    std::vector<std::string> sc; // set-cookie lines
};

// call MainActivity$Http.request(url, method, headersJson, body)
static Up http_call(JNIEnv *env, const std::string &url, const std::string &method,
                    const std::map<std::string, std::string> &headers,
                    const std::string &body) {
    Up up;
    if (!env) return up;

    cJSON *hh = cJSON_CreateObject();
    for (auto &kv : headers) {
        cJSON_AddStringToObject(hh, kv.first.c_str(), kv.second.c_str());
    }
    std::string headersJson = Jprint(hh);
    cJSON_Delete(hh);

    jclass cls = env->FindClass("com/meloglass/app/MainActivity$Http");
    if (!cls) { env->ExceptionClear(); return up; }
    jmethodID mid = env->GetStaticMethodID(cls, "request",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Ljava/lang/String;");
    if (!mid) { env->ExceptionClear(); env->DeleteLocalRef(cls); return up; }

    jstring ju = env->NewStringUTF(url.c_str());
    jstring jm = env->NewStringUTF(method.c_str());
    jstring jh = env->NewStringUTF(headersJson.c_str());
    jbyteArray jb = nullptr;
    if (!body.empty()) {
        jb = env->NewByteArray((jsize) body.size());
        env->SetByteArrayRegion(jb, 0, (jsize) body.size(), (const jbyte *) body.data());
    }
    jstring jr = (jstring) env->CallStaticObjectMethod(cls, mid, ju, jm, jh, jb);
    env->DeleteLocalRef(ju);
    env->DeleteLocalRef(jm);
    env->DeleteLocalRef(jh);
    if (jb) env->DeleteLocalRef(jb);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cls); return up; }
    if (!jr) { env->DeleteLocalRef(cls); return up; }

    std::string resp = jstr2(env, jr);
    env->DeleteLocalRef(jr);
    env->DeleteLocalRef(cls);

    cJSON *j = cJSON_Parse(resp.c_str());
    if (j) {
        up.st = (int) Jnum(j, "s", 0);
        up.b64 = Jstr(j, "b64");
        up.url = Jstr(j, "url");
        std::vector<uint8_t> raw = b64decode(up.b64);
        up.text.assign(raw.begin(), raw.end());
        cJSON *sc = Jarr(j, "sc");
        if (sc) {
            int n = cJSON_GetArraySize(sc);
            for (int i = 0; i < n; i++) {
                cJSON *it = cJSON_GetArrayItem(sc, i);
                if (it && cJSON_IsString(it) && cJSON_GetStringValue(it)) {
                    up.sc.push_back(cJSON_GetStringValue(it));
                }
            }
        }
        cJSON_Delete(j);
    }
    return up;
}

// ---------------------------------------------------------------- Store (persist)

static std::string store_read(JNIEnv *env, const std::string &name) {
    if (!env) return "";
    jclass cls = env->FindClass("com/meloglass/app/MainActivity$Store");
    if (!cls) { env->ExceptionClear(); return ""; }
    jmethodID mid = env->GetStaticMethodID(cls, "read", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!mid) { env->ExceptionClear(); env->DeleteLocalRef(cls); return ""; }
    jstring jn = env->NewStringUTF(name.c_str());
    jstring jr = (jstring) env->CallStaticObjectMethod(cls, mid, jn);
    env->DeleteLocalRef(jn);
    std::string out;
    if (jr && !env->ExceptionCheck()) out = jstr2(env, jr);
    env->ExceptionClear();
    if (jr) env->DeleteLocalRef(jr);
    env->DeleteLocalRef(cls);
    return out;
}

static void store_write(JNIEnv *env, const std::string &name, const std::string &content) {
    if (!env) return;
    jclass cls = env->FindClass("com/meloglass/app/MainActivity$Store");
    if (!cls) { env->ExceptionClear(); return; }
    jmethodID mid = env->GetStaticMethodID(cls, "write", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!mid) { env->ExceptionClear(); env->DeleteLocalRef(cls); return; }
    jstring jn = env->NewStringUTF(name.c_str());
    jstring jc = env->NewStringUTF(content.c_str());
    env->CallStaticVoidMethod(cls, mid, jn, jc);
    env->ExceptionClear();
    env->DeleteLocalRef(jn);
    env->DeleteLocalRef(jc);
    env->DeleteLocalRef(cls);
}

// ---------------------------------------------------------------- sessions

struct Sess {
    std::string cookie;
    std::string nickname;
    std::string via;
};

static std::map<std::string, Sess> g_sess;
static std::mutex g_sess_mu;
static bool g_sess_loaded = false;

static const char *PLATS[4] = {"qq", "wy", "kg", "qs"};

static const char *plat_hint(const std::string &src) {
    if (src == "wy") return "官方页面登录（账号密码 / 扫码都行）";
    if (src == "qq") return "官方页面登录（QQ / 微信都行）";
    if (src == "kg") return "官方页面登录";
    if (src == "qs") return "汽水官方网页登录（抖音扫码）";
    return "";
}

static const char *src_label(const std::string &src) {
    if (src == "qq") return "QQ音乐";
    if (src == "wy") return "网易云";
    if (src == "kg") return "酷狗";
    if (src == "qs") return "汽水";
    return src.c_str();
}

static void load_sessions(JNIEnv *env) {
    std::lock_guard<std::mutex> lk(g_sess_mu);
    if (g_sess_loaded) return;
    g_sess_loaded = true;
    std::string raw = store_read(env, "mg_sessions.json");
    if (raw.empty()) return;
    cJSON *j = cJSON_Parse(raw.c_str());
    if (!j) return;
    for (int i = 0; i < 4; i++) {
        cJSON *it = Jobj(j, PLATS[i]);
        if (it) {
            Sess s;
            s.cookie = Jstr(it, "cookie");
            s.nickname = Jstr(it, "nickname");
            s.via = Jstr(it, "via");
            if (!s.cookie.empty()) g_sess[PLATS[i]] = s;
        }
    }
    cJSON_Delete(j);
}

static void save_session(JNIEnv *env, const std::string &src, const Sess *rec) {
    {
        std::lock_guard<std::mutex> lk(g_sess_mu);
        if (rec) g_sess[src] = *rec;
        else g_sess.erase(src);
    }
    // persist all
    cJSON *j = cJSON_CreateObject();
    {
        std::lock_guard<std::mutex> lk(g_sess_mu);
        for (auto &kv : g_sess) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "cookie", kv.second.cookie.c_str());
            cJSON_AddStringToObject(it, "nickname", kv.second.nickname.c_str());
            cJSON_AddStringToObject(it, "via", kv.second.via.c_str());
            cJSON_AddItemToObject(j, kv.first.c_str(), it);
        }
    }
    store_write(env, "mg_sessions.json", Jprint(j));
    cJSON_Delete(j);
}

static std::string cookie_of(const std::string &src) {
    std::lock_guard<std::mutex> lk(g_sess_mu);
    auto it = g_sess.find(src);
    return it == g_sess.end() ? "" : it->second.cookie;
}

// ---------------------------------------------------------------- cookie jar

typedef std::map<std::string, std::string> CookieJar;

static void jar_merge(CookieJar &jar, const std::vector<std::string> &sc) {
    for (auto &line : sc) {
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        size_t semi = line.find(';', eq);
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1, semi == std::string::npos ? std::string::npos : semi - eq - 1);
        // trim
        auto tr = [](std::string &x) {
            size_t a = x.find_first_not_of(" \t");
            size_t b = x.find_last_not_of(" \t");
            x = (a == std::string::npos) ? "" : x.substr(a, b - a + 1);
        };
        tr(k);
        tr(v);
        if (!k.empty()) jar[k] = v;
    }
}

static std::string jar_header(const CookieJar &jar) {
    std::string o;
    for (auto &kv : jar) {
        if (!o.empty()) o += "; ";
        o += kv.first + "=" + kv.second;
    }
    return o;
}

// ---------------------------------------------------------------- upstream shortcuts

static Up up_get(JNIEnv *env, const std::string &url, const char *referer,
                 const std::string &srcCookie = "") {
    std::map<std::string, std::string> h;
    if (referer && *referer) h["Referer"] = referer;
    if (!srcCookie.empty()) h["Cookie"] = srcCookie;
    return http_call(env, url, "GET", h, "");
}

static Up up_post(JNIEnv *env, const std::string &url, const char *referer, const char *ctype,
                  const std::string &body, const std::string &srcCookie = "") {
    std::map<std::string, std::string> h;
    if (referer && *referer) h["Referer"] = referer;
    if (ctype && *ctype) h["Content-Type"] = ctype;
    if (!srcCookie.empty()) h["Cookie"] = srcCookie;
    return http_call(env, url, "POST", h, body);
}

static std::string cover_qq(const std::string &albummid) {
    if (albummid.empty()) return "";
    return "https://y.gtimg.cn/music/photo_new/T002R300x300M000" + albummid + ".jpg";
}

// ---------------------------------------------------------------- NetEase

static cJSON *wy_search(JNIEnv *env, const std::string &q) {
    std::string url = "https://music.163.com/api/search/get/web?s=" + url_encode(q) +
                      "&type=1&offset=0&limit=10&csrf_token=";
    Up up = up_get(env, url, "https://music.163.com/", cookie_of("wy"));
    cJSON *j = loads_maybe(up.text);
    cJSON *out = cJSON_CreateArray();
    cJSON *songs = nullptr;
    if (j) {
        cJSON *result = Jobj(j, "result");
        if (result) songs = Jarr(result, "songs");
    }
    if (songs) {
        int n = cJSON_GetArraySize(songs);
        for (int i = 0; i < n; i++) {
            cJSON *s = cJSON_GetArrayItem(songs, i);
            std::string arts;
            cJSON *artistArr = Jarr(s, "artists");
            if (artistArr) {
                int an = cJSON_GetArraySize(artistArr);
                for (int k = 0; k < an; k++) {
                    cJSON *a = cJSON_GetArrayItem(artistArr, k);
                    if (k) arts += " / ";
                    arts += Jstr(a, "name");
                }
            }
            cJSON *al = Jobj(s, "album");
            int fee = (int) Jnum(s, "fee", 0);
            std::string qual = (fee == 1 || fee == 4) ? "需会员" : (fee == 8 ? "VIP可播" : "免费");
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "src", "wy");
            std::string ids;
            cJSON *idit = cJSON_GetObjectItemCaseSensitive(s, "id");
            if (idit && cJSON_IsNumber(idit)) ids = std::to_string((long long) idit->valuedouble);
            cJSON_AddStringToObject(item, "id", ids.c_str());
            cJSON_AddStringToObject(item, "title", Jstr(s, "name").c_str());
            cJSON_AddStringToObject(item, "artist", arts.c_str());
            cJSON_AddStringToObject(item, "album", al ? Jstr(al, "name").c_str() : "");
            cJSON_AddStringToObject(item, "cover", "");
            cJSON_AddStringToObject(item, "quality", qual.c_str());
            cJSON_AddNumberToObject(item, "duration", Jnum(s, "duration", 0) / 1000);
            cJSON_AddBoolToObject(item, "playable", fee != 1);
            cJSON_AddItemToArray(out, item);
        }
    }
    if (j) cJSON_Delete(j);
    return out;
}

static cJSON *wy_playurl(JNIEnv *env, const std::string &sid, int br = 0) {
    std::string urlout, msg;
    int brout = 0;
    // br>0 只试该档;默认先无损 999000（账号有无损权益时生效）再回退 320k
    int tries[2] = {999000, 320000};
    int t0 = 0, t1 = 2;
    if (br == 128000) { tries[0] = 128000; t1 = 1; }
    else if (br == 320000) { tries[0] = 320000; tries[1] = 128000; }
    for (int t = t0; t < t1 && urlout.empty(); t++) {
        int b = tries[t];
        std::string url = "https://music.163.com/api/song/enhance/player/url?id=" + sid +
                          "&ids=[" + sid + "]&br=" + std::to_string(b);
        Up up = up_get(env, url, "https://music.163.com/", cookie_of("wy"));
        cJSON *j = loads_maybe(up.text);
        if (j) {
            cJSON *data = Jarr(j, "data");
            if (data && cJSON_GetArraySize(data) > 0) {
                cJSON *d = cJSON_GetArrayItem(data, 0);
                urlout = Jstr(d, "url");
                brout = (int) Jnum(d, "br", 0);
            }
            cJSON_Delete(j);
        }
    }
    cJSON *res = cJSON_CreateObject();
    if (urlout.empty()) msg = "无播放链（版权/需登录/需会员）";
    cJSON_AddStringToObject(res, "src", "wy");
    cJSON_AddStringToObject(res, "id", sid.c_str());
    if (!urlout.empty()) {
        cJSON_AddStringToObject(res, "url", urlout.c_str());
        std::string q = brout >= 900000 ? "网易云 · 无损 FLAC"
                        : brout >= 320000 ? "网易云 · 320kbps" : "网易云 · " + std::to_string(brout);
        cJSON_AddStringToObject(res, "quality", q.c_str());
    }
    cJSON_AddNumberToObject(res, "br", brout);
    if (msg.size()) cJSON_AddStringToObject(res, "message", msg.c_str());
    return res;
}

static cJSON *wy_lyric(JNIEnv *env, const std::string &sid) {
    Up up = up_get(env, "https://music.163.com/api/song/lyric?id=" + sid + "&lv=1&kv=1&tv=-1",
                   "https://music.163.com/");
    cJSON *j = loads_maybe(up.text);
    cJSON *res = cJSON_CreateObject();
    std::string lrc, tlrc;
    if (j) {
        cJSON *o = Jobj(j, "lrc");
        if (o) lrc = Jstr(o, "lyric");
        o = Jobj(j, "tlyric");
        if (o) tlrc = Jstr(o, "lyric");
        cJSON_Delete(j);
    }
    cJSON_AddStringToObject(res, "src", "wy");
    cJSON_AddStringToObject(res, "id", sid.c_str());
    cJSON_AddStringToObject(res, "lrc", lrc.c_str());
    cJSON_AddStringToObject(res, "tlyric", tlrc.c_str());
    return res;
}

static cJSON *wy_recommend(JNIEnv *env, int limit) {
    std::string url = "https://music.163.com/api/personalized/playlist?limit=" + std::to_string(limit);
    Up up = up_get(env, url, "https://music.163.com/", cookie_of("wy"));
    cJSON *j = loads_maybe(up.text);
    cJSON *out = cJSON_CreateArray();
    if (j) {
        cJSON *result = Jarr(j, "result");
        if (result) {
            int n = cJSON_GetArraySize(result);
            for (int i = 0; i < n; i++) {
                cJSON *it = cJSON_GetArrayItem(result, i);
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "src", "wy");
                std::string ids;
                cJSON *idit = cJSON_GetObjectItemCaseSensitive(it, "id");
                if (idit && cJSON_IsNumber(idit)) ids = std::to_string((long long) idit->valuedouble);
                cJSON_AddStringToObject(item, "id", ids.c_str());
                cJSON_AddStringToObject(item, "title", Jstr(it, "name").c_str());
                cJSON_AddStringToObject(item, "cover", Jstr(it, "picUrl").c_str());
                cJSON_AddNumberToObject(item, "count", Jnum(it, "trackCount", 0));
                cJSON_AddNumberToObject(item, "playCount", Jnum(it, "playCount", 0));
                cJSON_AddItemToArray(out, item);
            }
        }
        cJSON_Delete(j);
    }
    return out;
}

static cJSON *wy_newsong(JNIEnv *env) {
    Up up = up_get(env, "https://music.163.com/api/personalized/newsong", "https://music.163.com/");
    cJSON *j = loads_maybe(up.text);
    cJSON *out = cJSON_CreateArray();
    if (j) {
        cJSON *result = Jarr(j, "result");
        if (result) {
            int n = cJSON_GetArraySize(result);
            for (int i = 0; i < n && out // limit 12
                            && cJSON_GetArraySize(out) < 12; i++) {
                cJSON *it = cJSON_GetArrayItem(result, i);
                cJSON *song = Jobj(it, "song");
                std::string arts;
                cJSON *artistArr = song ? Jarr(song, "artists") : nullptr;
                if (artistArr) {
                    int an = cJSON_GetArraySize(artistArr);
                    for (int k = 0; k < an; k++) {
                        cJSON *a = cJSON_GetArrayItem(artistArr, k);
                        if (k) arts += " / ";
                        arts += Jstr(a, "name");
                    }
                }
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "src", "wy");
                std::string ids;
                cJSON *idit = song ? cJSON_GetObjectItemCaseSensitive(song, "id") : nullptr;
                if (idit && cJSON_IsNumber(idit)) ids = std::to_string((long long) idit->valuedouble);
                if (ids.empty()) {
                    idit = cJSON_GetObjectItemCaseSensitive(it, "id");
                    if (idit && cJSON_IsNumber(idit)) ids = std::to_string((long long) idit->valuedouble);
                }
                cJSON_AddStringToObject(item, "id", ids.c_str());
                std::string title = song ? Jstr(song, "name") : Jstr(it, "name");
                cJSON_AddStringToObject(item, "title", title.c_str());
                cJSON_AddStringToObject(item, "artist", arts.c_str());
                cJSON_AddStringToObject(item, "cover", Jstr(it, "picUrl").c_str());
                cJSON_AddStringToObject(item, "quality", "新歌");
                cJSON_AddItemToArray(out, item);
            }
        }
        cJSON_Delete(j);
    }
    return out;
}

static cJSON *wy_playlist(JNIEnv *env, const std::string &pid) {
    Up up = up_get(env, "https://music.163.com/api/playlist/detail?id=" + pid,
                   "https://music.163.com/");
    cJSON *j = loads_maybe(up.text);
    cJSON *res = cJSON_CreateObject();
    cJSON *tracks = cJSON_CreateArray();
    if (j) {
        cJSON *r = Jobj(j, "result");
        if (r) {
            cJSON_AddStringToObject(res, "title", Jstr(r, "name").c_str());
            cJSON_AddStringToObject(res, "cover", Jstr(r, "coverImgUrl").c_str());
            cJSON *cnt = cJSON_GetObjectItemCaseSensitive(r, "trackCount");
            if (cnt && cJSON_IsNumber(cnt))
                cJSON_AddNumberToObject(res, "count", cnt->valuedouble);
            cJSON *ts = Jarr(r, "tracks");
            if (ts) {
                int n = cJSON_GetArraySize(ts);
                for (int i = 0; i < n && i < 80; i++) {
                    cJSON *t = cJSON_GetArrayItem(ts, i);
                    std::string arts;
                    cJSON *artistArr = Jarr(t, "artists");
                    if (artistArr) {
                        int an = cJSON_GetArraySize(artistArr);
                        for (int k = 0; k < an; k++) {
                            cJSON *a = cJSON_GetArrayItem(artistArr, k);
                            if (k) arts += " / ";
                            arts += Jstr(a, "name");
                        }
                    }
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "src", "wy");
                    cJSON *idit = cJSON_GetObjectItemCaseSensitive(t, "id");
                    std::string ids = (idit && cJSON_IsNumber(idit))
                                      ? std::to_string((long long) idit->valuedouble) : "";
                    cJSON_AddStringToObject(item, "id", ids.c_str());
                    cJSON_AddStringToObject(item, "title", Jstr(t, "name").c_str());
                    cJSON_AddStringToObject(item, "artist", arts.c_str());
                    cJSON_AddStringToObject(item, "quality", "网易云");
                    cJSON_AddItemToArray(tracks, item);
                }
            }
        }
        cJSON_Delete(j);
    }
    cJSON_AddStringToObject(res, "src", "wy");
    cJSON_AddStringToObject(res, "id", pid.c_str());
    cJSON_AddItemToObject(res, "tracks", tracks);
    return res;
}

static cJSON *wy_profile_json(JNIEnv *env, const std::string &cookie);

static cJSON *wy_user_playlists(JNIEnv *env) {
    cJSON *out = cJSON_CreateArray();
    std::string ck = cookie_of("wy");
    if (ck.empty()) return out;
    cJSON *prof = wy_profile_json(env, ck);
    std::string uid;
    cJSON *uidit = prof ? cJSON_GetObjectItemCaseSensitive(prof, "uid") : nullptr;
    if (uidit && cJSON_IsNumber(uidit)) uid = std::to_string((long long) uidit->valuedouble);
    if (uid.empty()) uid = Jstr(prof, "uid");
    if (prof) cJSON_Delete(prof);
    if (uid.empty()) return out;
    std::string url = "https://music.163.com/api/user/playlist?uid=" + uid + "&limit=50&offset=0";
    Up up = up_get(env, url, "https://music.163.com/", ck);
    cJSON *j = loads_maybe(up.text);
    if (j) {
        cJSON *pls = Jarr(j, "playlist");
        if (pls) {
            int n = cJSON_GetArraySize(pls);
            for (int i = 0; i < n; i++) {
                cJSON *it = cJSON_GetArrayItem(pls, i);
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "src", "wy");
                std::string ids;
                cJSON *idit = cJSON_GetObjectItemCaseSensitive(it, "id");
                if (idit && cJSON_IsNumber(idit)) ids = std::to_string((long long) idit->valuedouble);
                cJSON_AddStringToObject(item, "id", ids.c_str());
                cJSON_AddStringToObject(item, "title", Jstr(it, "name").c_str());
                cJSON_AddStringToObject(item, "cover", Jstr(it, "coverImgUrl").c_str());
                cJSON_AddNumberToObject(item, "count", Jnum(it, "trackCount", 0));
                cJSON_AddItemToArray(out, item);
            }
        }
        cJSON_Delete(j);
    }
    return out;
}

// ---------------------------------------------------------------- QQ Music

/** 从 QQ cookie 里解析 uin（uin=o123456 或 uin=123456）。 */
static std::string qq_uin_from_cookie(const std::string &cookie) {
    size_t pos = 0;
    while (pos <= cookie.size()) {
        size_t amp = cookie.find(';', pos);
        std::string pair = cookie.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = pair.substr(0, eq);
            std::string v = pair.substr(eq + 1);
            size_t a = k.find_first_not_of(" \t");
            size_t b = k.find_last_not_of(" \t");
            k = a == std::string::npos ? "" : k.substr(a, b - a + 1);
            a = v.find_first_not_of(" \t");
            b = v.find_last_not_of(" \t");
            v = a == std::string::npos ? "" : v.substr(a, b - a + 1);
            if (k == "uin" && !v.empty()) {
                if (v[0] == 'o') v = v.substr(1);
                return v;
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

/** QQ g_tk：对 qm_keyst（优先）或 p_skey 做 5381 哈希。 */
static std::string qq_gtk(const std::string &cookie) {
    std::string skey;
    size_t pos = 0;
    while (pos <= cookie.size()) {
        size_t amp = cookie.find(';', pos);
        std::string pair = cookie.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = pair.substr(0, eq);
            std::string v = pair.substr(eq + 1);
            size_t a = k.find_first_not_of(" \t");
            size_t b = k.find_last_not_of(" \t");
            k = a == std::string::npos ? "" : k.substr(a, b - a + 1);
            a = v.find_first_not_of(" \t");
            b = v.find_last_not_of(" \t");
            v = a == std::string::npos ? "" : v.substr(a, b - a + 1);
            if (k == "qm_keyst" && v.size() > 5) { skey = v; break; }
            if (k == "p_skey" && v.size() > 5 && skey.empty()) skey = v;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    int h = 5381;
    for (char c : skey) h = h + (h << 5) + (unsigned char) c;
    return std::to_string(h & 0x7fffffff);
}

/** QQ 登录用户的歌单（musicu.fcg playlist.GetUserPlaylist）。 */
static cJSON *qq_user_playlists(JNIEnv *env) {
    cJSON *out = cJSON_CreateArray();
    std::string ck = cookie_of("qq");
    if (ck.empty()) return out;
    std::string uin = qq_uin_from_cookie(ck);
    if (uin.empty()) return out;
    std::string gtk = qq_gtk(ck);
    std::string data = "{\"comm\":{\"ct\":19,\"cv\":1848,\"uin\":" + uin + ",\"format\":\"json\"},"
                       "\"req_1\":{\"module\":\"playlist.GetUserPlaylist\","
                       "\"method\":\"GetUserPlaylist\","
                       "\"param\":{\"uin\":" + uin + ",\"hostUin\":" + uin +
                       ",\"QZFee\":1,\"succ\":3,\"count\":50}}}";
    std::string url = "https://u.y.qq.com/cgi-bin/musicu.fcg?g_tk=" + gtk +
                      "&loginUin=" + uin + "&hostUin=0&format=json&inCharset=utf8&outCharset=utf-8"
                      "&notice=0&platform=yqq.json&needNewCode=0&data=" + url_encode(data);
    Up up = up_get(env, url, "https://y.qq.com/", ck);
    cJSON *j = loads_maybe(up.text);
    if (j) {
        cJSON *list = nullptr;
        cJSON *r1 = Jobj(j, "req_1");
        if (r1) {
            cJSON *d = Jobj(r1, "data");
            if (d) list = Jarr(d, "list");
        }
        if (list) {
            int n = cJSON_GetArraySize(list);
            for (int i = 0; i < n; i++) {
                cJSON *it = cJSON_GetArrayItem(list, i);
                std::string tid = Jstr(it, "tid");
                if (tid.empty()) {
                    cJSON *tidit = cJSON_GetObjectItemCaseSensitive(it, "tid");
                    if (tidit && cJSON_IsNumber(tidit))
                        tid = std::to_string((long long) tidit->valuedouble);
                }
                if (tid.empty()) continue;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "src", "qq");
                cJSON_AddStringToObject(item, "id", tid.c_str());
                cJSON_AddStringToObject(item, "title", Jstr(it, "diss_name").c_str());
                cJSON_AddStringToObject(item, "cover", Jstr(it, "diss_cover").c_str());
                cJSON_AddNumberToObject(item, "count", Jnum(it, "song_cnt", 0));
                cJSON_AddItemToArray(out, item);
            }
        }
        cJSON_Delete(j);
    }
    return out;
}

static cJSON *qq_search(JNIEnv *env, const std::string &q) {
    std::string url = "https://c.y.qq.com/soso/fcgi-bin/search_for_qq_cp?g_tk=5381&uin=0&format=json"
                      "&inCharset=utf-8&outCharset=utf-8&notice=0&platform=h5&needNewCode=1&w=" +
                      url_encode(q) +
                      "&zhidaqu=1&catZhida=1&t=0&flag=1&ie=utf-8&sem=1&aggr=0&perpage=10&n=10&p=1"
                      "&remoteplace=txt.mqq.all";
    Up up = up_get(env, url, "https://y.qq.com/");
    cJSON *j = loads_maybe(up.text);
    cJSON *out = cJSON_CreateArray();
    if (j) {
        cJSON *songs = nullptr;
        cJSON *d = Jobj(j, "data");
        if (d) {
            cJSON *song = Jobj(d, "song");
            if (song) songs = Jarr(song, "list");
        }
        if (songs) {
            int n = cJSON_GetArraySize(songs);
            for (int i = 0; i < n; i++) {
                cJSON *s = cJSON_GetArrayItem(songs, i);
                std::string arts;
                cJSON *singer = Jarr(s, "singer");
                if (singer) {
                    int sn = cJSON_GetArraySize(singer);
                    for (int k = 0; k < sn; k++) {
                        cJSON *a = cJSON_GetArrayItem(singer, k);
                        if (k) arts += " / ";
                        arts += Jstr(a, "name");
                    }
                } else {
                    arts = Jstr(s, "singer");
                }
                int payplay = 0;
                cJSON *pay = Jobj(s, "pay");
                if (pay) payplay = (int) Jnum(pay, "payplay", 0);
                std::string albummid = Jstr(s, "albummid");
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "src", "qq");
                cJSON_AddStringToObject(item, "id", Jstr(s, "songmid").c_str());
                cJSON_AddStringToObject(item, "title", Jstr(s, "songname").c_str());
                cJSON_AddStringToObject(item, "artist", arts.c_str());
                cJSON_AddStringToObject(item, "album", Jstr(s, "albumname").c_str());
                cJSON_AddStringToObject(item, "cover", cover_qq(albummid).c_str());
                cJSON_AddStringToObject(item, "quality", payplay ? "需付费" : "可试听/免费");
                cJSON_AddNumberToObject(item, "duration", Jnum(s, "interval", 0));
                cJSON_AddBoolToObject(item, "playable", true);
                cJSON_AddItemToArray(out, item);
            }
        }
        cJSON_Delete(j);
    }
    return out;
}

static cJSON *qq_playurl(JNIEnv *env, const std::string &songmid, int startTier = 0) {
    std::string ck = cookie_of("qq");
    std::string uin = "0";
    // parse uin from cookie
    size_t pos = 0;
    while (pos <= ck.size()) {
        size_t semi = ck.find(';', pos);
        std::string part = ck.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        // trim
        size_t a = part.find_first_not_of(" \t");
        if (a != std::string::npos) part = part.substr(a);
        std::string low = part;
        for (auto &c : low) c = (char) tolower(c);
        if (low.rfind("uin=", 0) == 0 || low.rfind("wxuin=", 0) == 0) {
            std::string v = part.substr(part.find('=') + 1);
            while (!v.empty() && (v[0] == 'o' || v[0] == 'O')) v = v.substr(1);
            uin = v;
            break;
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }

    // SVIP 权益:从 FLAC 无损往下试（startTier 可由音质设置抬高起点）
    if (startTier < 0 || startTier > 2) startTier = 0;
    const char *tiers[3][2] = {{"F000", ".flac"}, {"M800", ".mp3"}, {"M500", ".mp3"}};
    std::string url, filename, quality;
    for (int t = startTier; t < 3 && url.empty(); t++) {
        cJSON *payload = cJSON_CreateObject();
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "module", "vkey.GetVkeyServer");
        cJSON_AddStringToObject(req, "method", "CgiGetVkey");
        cJSON *param = cJSON_CreateObject();
        cJSON_AddStringToObject(param, "guid", "10000");
        cJSON *mids = cJSON_CreateArray();
        cJSON_AddItemToArray(mids, cJSON_CreateString(songmid.c_str()));
        cJSON_AddItemToObject(param, "songmid", mids);
        cJSON *fnames = cJSON_CreateArray();
        std::string want = std::string(tiers[t][0]) + songmid + tiers[t][1];
        cJSON_AddItemToArray(fnames, cJSON_CreateString(want.c_str()));
        cJSON_AddItemToObject(param, "filename", fnames);
        cJSON *types = cJSON_CreateArray();
        cJSON_AddItemToArray(types, cJSON_CreateNumber(0));
        cJSON_AddItemToObject(param, "songtype", types);
        cJSON_AddStringToObject(param, "uin", uin.c_str());
        cJSON_AddNumberToObject(param, "loginflag", ck.empty() ? 0 : 1);
        cJSON_AddStringToObject(param, "platform", "20");
        cJSON_AddItemToObject(req, "param", param);
        cJSON_AddItemToObject(payload, "req_0", req);
        cJSON *comm = cJSON_CreateObject();
        cJSON_AddStringToObject(comm, "uin", uin.c_str());
        cJSON_AddStringToObject(comm, "format", "json");
        cJSON_AddNumberToObject(comm, "ct", 24);
        cJSON_AddNumberToObject(comm, "cv", 0);
        cJSON_AddItemToObject(payload, "comm", comm);
        std::string body = Jprint(payload);
        cJSON_Delete(payload);

        Up up = up_post(env, "https://u.y.qq.com/cgi-bin/musicu.fcg", "https://y.qq.com/",
                        "application/json", body, ck);
        cJSON *j = loads_maybe(up.text);
        if (j) {
            cJSON *r0 = Jobj(j, "req_0");
            if (r0) {
                cJSON *data = Jobj(r0, "data");
                if (data) {
                    cJSON *mi = Jarr(data, "midurlinfo");
                    if (mi && cJSON_GetArraySize(mi) > 0) {
                        cJSON *info = cJSON_GetArrayItem(mi, 0);
                        std::string purl = Jstr(info, "purl");
                        std::string fn = Jstr(info, "filename");
                        if (!purl.empty()) {
                            std::string sip;
                            cJSON *sipArr = Jarr(data, "sip");
                            if (sipArr && cJSON_GetArraySize(sipArr) > 0) {
                                cJSON *s0 = cJSON_GetArrayItem(sipArr, 0);
                                if (s0 && cJSON_IsString(s0) && cJSON_GetStringValue(s0))
                                    sip = cJSON_GetStringValue(s0);
                            }
                            url = sip + purl;
                            filename = fn;
                            quality = t == 0 ? "QQ SVIP · 无损 FLAC"
                                             : t == 1 ? "QQ · 320kbps" : "QQ · 128kbps";
                        }
                    }
                }
            }
            cJSON_Delete(j);
        }
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "src", "qq");
    cJSON_AddStringToObject(res, "id", songmid.c_str());
    if (!url.empty()) cJSON_AddStringToObject(res, "url", url.c_str());
    if (filename.size()) cJSON_AddStringToObject(res, "filename", filename.c_str());
    if (quality.size()) cJSON_AddStringToObject(res, "quality", quality.c_str());
    if (url.empty()) cJSON_AddStringToObject(res, "message", "无 vkey（需登录 Cookie 或该曲需付费）");
    return res;
}

static cJSON *qq_lyric(JNIEnv *env, const std::string &songmid) {
    Up up = up_get(env,
                   "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=" + songmid +
                   "&format=json&nobase64=1", "https://y.qq.com/");
    cJSON *j = loads_maybe(up.text);
    cJSON *res = cJSON_CreateObject();
    std::string lrc, trans;
    if (j) {
        lrc = Jstr(j, "lyric");
        trans = Jstr(j, "trans");
        cJSON_Delete(j);
    }
    cJSON_AddStringToObject(res, "src", "qq");
    cJSON_AddStringToObject(res, "id", songmid.c_str());
    cJSON_AddStringToObject(res, "lrc", lrc.c_str());
    cJSON_AddStringToObject(res, "tlyric", trans.c_str());
    return res;
}

static cJSON *qq_playlist(JNIEnv *env, const std::string &pid) {
    Up up = up_get(env,
                   "https://c.y.qq.com/v8/fcg-bin/fcg_v8_playlist_cp.fcg?id=" + pid +
                   "&format=json&newsong=1",
                   ("https://y.qq.com/n/ryqq/playlist/" + pid).c_str());
    cJSON *j = loads_maybe(up.text);
    cJSON *res = cJSON_CreateObject();
    cJSON *tracks = cJSON_CreateArray();
    if (j) {
        cJSON *cd = nullptr;
        cJSON *data = Jobj(j, "data");
        if (data) {
            cJSON *cdl = Jarr(data, "cdlist");
            if (cdl && cJSON_GetArraySize(cdl) > 0) cd = cJSON_GetArrayItem(cdl, 0);
        }
        if (cd) {
            cJSON_AddStringToObject(res, "title", Jstr(cd, "dissname").c_str());
            cJSON_AddStringToObject(res, "cover", Jstr(cd, "logo").c_str());
            double cnt = Jnum(cd, "songnum", 0);
            if (cnt == 0) cnt = Jnum(cd, "cur_song_num", 0);
            cJSON_AddNumberToObject(res, "count", cnt);
            cJSON *sl = Jarr(cd, "songlist");
            if (sl) {
                int n = cJSON_GetArraySize(sl);
                for (int i = 0; i < n && i < 80; i++) {
                    cJSON *s = cJSON_GetArrayItem(sl, i);
                    std::string title = Jstr(s, "songname");
                    if (title.empty()) title = Jstr(s, "name");
                    std::string mid = Jstr(s, "songmid");
                    if (mid.empty()) mid = Jstr(s, "mid");
                    std::string arts;
                    cJSON *singer = Jarr(s, "singer");
                    if (singer) {
                        int sn = cJSON_GetArraySize(singer);
                        for (int k = 0; k < sn; k++) {
                            cJSON *a = cJSON_GetArrayItem(singer, k);
                            if (k) arts += " / ";
                            std::string nm = Jstr(a, "name");
                            if (nm.empty()) nm = Jstr(a, "title");
                            arts += nm;
                        }
                    }
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "src", "qq");
                    cJSON_AddStringToObject(item, "id", mid.c_str());
                    cJSON_AddStringToObject(item, "title", title.c_str());
                    cJSON_AddStringToObject(item, "artist", arts.c_str());
                    cJSON_AddStringToObject(item, "quality", "QQ音乐");
                    cJSON_AddItemToArray(tracks, item);
                }
            }
        }
        cJSON_Delete(j);
    }
    cJSON_AddStringToObject(res, "src", "qq");
    cJSON_AddStringToObject(res, "id", pid.c_str());
    cJSON_AddItemToObject(res, "tracks", tracks);
    return res;
}

// ---------------------------------------------------------------- Kugou

static cJSON *kg_search(JNIEnv *env, const std::string &q) {
    std::string url = "https://songsearch.kugou.com/song_search_v2?keyword=" + url_encode(q) +
                      "&page=1&pagesize=10&userid=0&clientver=20000&platform=WebFilter&filter=2"
                      "&iscorrection=1&privilege_filter=0";
    Up up = up_get(env, url, "https://www.kugou.com/");
    cJSON *j = loads_maybe(up.text);
    cJSON *out = cJSON_CreateArray();
    if (j) {
        cJSON *lists = nullptr;
        cJSON *d = Jobj(j, "data");
        if (d) lists = Jarr(d, "lists");
        if (lists) {
            int n = cJSON_GetArraySize(lists);
            for (int i = 0; i < n; i++) {
                cJSON *it = cJSON_GetArrayItem(lists, i);
                int priv = (int) Jnum(it, "Privilege", -1);
                cJSON *price = cJSON_GetObjectItemCaseSensitive(it, "Price");
                bool free = priv == 0 || priv == 8 || !price || cJSON_IsNull(price);
                std::string name = Jstr(it, "FileName");
                if (name.empty()) name = Jstr(it, "SongName");
                std::string singer = Jstr(it, "SingerName");
                std::string title = Jstr(it, "SongName");
                if (title.empty()) title = name;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "src", "kg");
                cJSON_AddStringToObject(item, "id", Jstr(it, "FileHash").c_str());
                cJSON_AddStringToObject(item, "album_id", Jstr(it, "AlbumID").c_str());
                cJSON_AddStringToObject(item, "title", title.c_str());
                cJSON_AddStringToObject(item, "artist", singer.c_str());
                cJSON_AddStringToObject(item, "album", Jstr(it, "AlbumName").c_str());
                cJSON_AddStringToObject(item, "cover", "");
                cJSON_AddStringToObject(item, "quality", free ? "免费" : "需付费");
                cJSON_AddNumberToObject(item, "duration", Jnum(it, "Duration", 0));
                cJSON_AddBoolToObject(item, "playable", true);
                cJSON_AddNumberToObject(item, "privilege", priv);
                cJSON_AddItemToArray(out, item);
            }
        }
        cJSON_Delete(j);
    }
    return out;
}

static cJSON *kg_playurl(JNIEnv *env, const std::string &filehash) {
    Up up = up_get(env,
                   "https://m.kugou.com/app/i/getSongInfo.php?cmd=playInfo&hash=" + filehash,
                   "https://m.kugou.com/");
    cJSON *j = loads_maybe(up.text);
    cJSON *res = cJSON_CreateObject();
    std::string url, msg;
    if (j) {
        url = Jstr(j, "url");
        if (url.empty()) msg = Jstr(j, "error");
        std::string img = Jstr(j, "imgUrl");
        size_t p = img.find("{size}");
        if (p != std::string::npos) img.replace(p, 6, "400");
        cJSON_AddStringToObject(res, "cover", img.c_str());
        cJSON_AddStringToObject(res, "title", Jstr(j, "songName").c_str());
        cJSON_AddStringToObject(res, "artist", Jstr(j, "singerName").c_str());
        cJSON_AddNumberToObject(res, "br", Jnum(j, "bitRate", 0));
        cJSON_Delete(j);
    }
    if (url.empty() && msg.empty()) msg = "无播放链";
    cJSON_AddStringToObject(res, "src", "kg");
    cJSON_AddStringToObject(res, "id", filehash.c_str());
    if (!url.empty()) cJSON_AddStringToObject(res, "url", url.c_str());
    if (!msg.empty()) cJSON_AddStringToObject(res, "message", msg.c_str());
    return res;
}

// ---------------------------------------------------------------- Qishui / Soda

static cJSON *qs_seo_json(JNIEnv *env, const std::string &track_id) {
    Up up = up_get(env,
                   "https://beta-luna.douyin.com/luna/h5/seo_track?track_id=" + url_encode(track_id) +
                   "&device_platform=web", "https://www.qishui.com/");
    return loads_maybe(up.text); // caller deletes
}

static cJSON *qs_playurl(JNIEnv *env, const std::string &track_id) {
    cJSON *j = qs_seo_json(env, track_id);
    cJSON *res = cJSON_CreateObject();
    std::string play_url, title, arts, lrc, quality = "SEO 公开源";
    double duration = 0;
    if (j) {
        cJSON *seo = nullptr;
        cJSON *st = Jobj(j, "seo_track");
        if (st) seo = Jobj(st, "track");
        if (seo) {
            title = Jstr(seo, "name");
            cJSON *artistArr = Jarr(seo, "artists");
            if (artistArr) {
                int an = cJSON_GetArraySize(artistArr);
                for (int k = 0; k < an; k++) {
                    cJSON *a = cJSON_GetArrayItem(artistArr, k);
                    if (k) arts += " / ";
                    arts += Jstr(a, "name");
                }
            }
            duration = Jnum(seo, "duration", 0);
        }
        cJSON *tp = Jobj(j, "track_player");
        if (tp) {
            std::string upi = Jstr(tp, "url_player_info");
            if (upi.rfind("http", 0) == 0) {
                Up up2 = up_get(env, upi, "https://www.qishui.com/");
                cJSON *info = loads_maybe(up2.text);
                if (info) {
                    cJSON *result = Jobj(info, "Result");
                    cJSON *data = result ? Jobj(result, "Data") : nullptr;
                    cJSON *pl = data ? Jobj(data, "PlayInfoList") : nullptr;
                    if (pl && cJSON_IsArray(pl) && cJSON_GetArraySize(pl) > 0) {
                        cJSON *p0 = cJSON_GetArrayItem(pl, 0);
                        play_url = Jstr(p0, "MainPlayUrl");
                        double dur = Jnum(p0, "Duration", 0);
                        if (dur > 0) duration = dur;
                        std::string q = Jstr(p0, "Quality");
                        if (!q.empty()) quality = q;
                        if (dur > 0 && dur <= 35) quality = "试听 30s（VIP 全曲需官方客户端签名）";
                    }
                    cJSON_Delete(info);
                }
            }
        }
        cJSON *lyr = Jobj(j, "lyric");
        if (lyr) lrc = Jstr(lyr, "content");
        cJSON_Delete(j);
    }
    cJSON_AddStringToObject(res, "src", "qs");
    cJSON_AddStringToObject(res, "id", track_id.c_str());
    if (!play_url.empty()) cJSON_AddStringToObject(res, "url", play_url.c_str());
    cJSON_AddStringToObject(res, "title", title.c_str());
    cJSON_AddStringToObject(res, "artist", arts.c_str());
    cJSON_AddNumberToObject(res, "duration", duration);
    cJSON_AddStringToObject(res, "quality", quality.c_str());
    cJSON_AddStringToObject(res, "lrc", lrc.c_str());
    if (play_url.empty()) cJSON_AddStringToObject(res, "message", "无播放链");
    return res;
}

static cJSON *qs_playlist(JNIEnv *env, const std::string &pid) {
    Up up = up_get(env,
                   "https://beta-luna.douyin.com/luna/playlist/detail?playlist_id=" + url_encode(pid),
                   "https://www.qishui.com/");
    cJSON *j = loads_maybe(up.text);
    cJSON *res = cJSON_CreateObject();
    cJSON *tracks = cJSON_CreateArray();
    if (j) {
        cJSON *pl = Jobj(j, "playlist");
        if (pl) {
            cJSON_AddStringToObject(res, "title", Jstr(pl, "title").c_str());
            double cnt = Jnum(pl, "count_tracks", 0);
            cJSON_AddNumberToObject(res, "count", cnt);
            cJSON *uc = Jobj(pl, "url_cover");
            if (uc) {
                cJSON *urls = Jarr(uc, "urls");
                if (urls && cJSON_GetArraySize(urls) > 0) {
                    cJSON *u0 = cJSON_GetArrayItem(urls, 0);
                    if (u0 && cJSON_IsString(u0) && cJSON_GetStringValue(u0))
                        cJSON_AddStringToObject(res, "cover", cJSON_GetStringValue(u0));
                }
            }
        }
        cJSON *mrs = Jarr(j, "media_resources");
        if (mrs) {
            int n = cJSON_GetArraySize(mrs);
            for (int i = 0; i < n; i++) {
                cJSON *mr = cJSON_GetArrayItem(mrs, i);
                cJSON *tw = nullptr;
                cJSON *entity = Jobj(mr, "entity");
                if (entity) {
                    cJSON *twr = Jobj(entity, "track_wrapper");
                    if (twr) tw = Jobj(twr, "track");
                }
                if (!tw) continue;
                std::string arts;
                cJSON *artistArr = Jarr(tw, "artists");
                if (artistArr) {
                    int an = cJSON_GetArraySize(artistArr);
                    for (int k = 0; k < an; k++) {
                        cJSON *a = cJSON_GetArrayItem(artistArr, k);
                        if (k) arts += " / ";
                        arts += Jstr(a, "name");
                    }
                }
                std::string ids;
                cJSON *idit = cJSON_GetObjectItemCaseSensitive(tw, "id");
                if (idit && cJSON_IsNumber(idit)) ids = std::to_string((long long) idit->valuedouble);
                if (ids.empty()) ids = Jstr(mr, "id");
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "src", "qs");
                cJSON_AddStringToObject(item, "id", ids.c_str());
                cJSON_AddStringToObject(item, "title", Jstr(tw, "name").c_str());
                cJSON_AddStringToObject(item, "artist", arts.c_str());
                cJSON_AddStringToObject(item, "quality", "汽水");
                cJSON_AddItemToArray(tracks, item);
            }
        }
        cJSON_Delete(j);
    }
    cJSON_AddStringToObject(res, "src", "qs");
    cJSON_AddStringToObject(res, "id", pid.c_str());
    cJSON_AddItemToObject(res, "tracks", tracks);
    return res;
}

static cJSON *qs_search_note(const std::string &q) {
    std::string ckk = cookie_of("qs");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "src", "qs");
    cJSON_AddStringToObject(item, "id", "");
    std::string title = "汽水搜索「" + q + "」需要 PC 签名头 X-Helios / X-Medusa";
    std::string artist = ckk.empty()
                         ? "先扫码登录抖音；全曲搜索还要汽水 PC 签名"
                         : "已登录抖音，但仍缺客户端签名，搜不到曲库";
    cJSON_AddStringToObject(item, "title", title.c_str());
    cJSON_AddStringToObject(item, "artist", artist.c_str());
    cJSON_AddStringToObject(item, "album", "");
    cJSON_AddStringToObject(item, "cover", "");
    cJSON_AddStringToObject(item, "quality", "需官方客户端签名");
    cJSON_AddBoolToObject(item, "playable", false);
    cJSON_AddStringToObject(item, "hint", "playlist");
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, item);
    return arr;
}

// ---------------------------------------------------------------- login: QR

struct QrJob {
    std::string src;
    std::string id;
    std::string url;
    std::string extra;      // qq: xlogin url
    CookieJar jar;
};

static std::map<std::string, QrJob> g_jobs;
static std::mutex g_jobs_mu;
static unsigned long long g_job_seq = 0;

static std::string rand_id() {
    static const char *hexd = "0123456789abcdef";
    std::string o;
    srand((unsigned) (time(nullptr) * 1000 + (++g_job_seq)));
    for (int i = 0; i < 32; i++) o += hexd[rand() % 16];
    return o;
}

// create: returns cJSON public payload (caller deletes)
static cJSON *login_create(JNIEnv *env, const std::string &src) {
    load_sessions(env);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "src", src.c_str());
    cJSON_AddBoolToObject(out, "allow_cookie", true);
    cJSON_AddStringToObject(out, "hint", plat_hint(src));

    if (src == "kg") {
        cJSON_AddStringToObject(out, "mode", "cookie");
        return out;
    }

    QrJob job;
    job.src = src;

    if (src == "wy") {
        CookieJar jar;
        Up up = http_call(env, "https://music.163.com/api/login/qrcode/unikey?type=1", "GET",
                          std::map<std::string, std::string>{{"Referer", "https://music.163.com/"}}, "");
        jar_merge(jar, up.sc);
        cJSON *j = loads_maybe(up.text);
        if (!j) { cJSON_Delete(out); return nullptr; }
        std::string unikey = Jstr(j, "unikey");
        cJSON_Delete(j);
        if (unikey.empty()) { cJSON_Delete(out); return nullptr; }
        job.id = unikey;
        job.url = "https://music.163.com/login?codekey=" + unikey;
        job.jar = jar;
        cJSON_AddStringToObject(out, "mode", "qr");
        cJSON_AddStringToObject(out, "job", unikey.c_str());
        cJSON_AddStringToObject(out, "url", job.url.c_str());
        // qrimg rendered on JS side (local qrcode lib)
    } else if (src == "qq") {
        std::string xlogin =
                "https://xui.ptlogin2.qq.com/cgi-bin/xlogin?appid=716027609&daid=383&style=33"
                "&s_url=https%3A%2F%2Fy.qq.com%2F&pt_3rd_aid=100497308";
        CookieJar jar;
        Up up0 = http_call(env, xlogin, "GET",
                           std::map<std::string, std::string>{{"Referer", "https://y.qq.com/"}}, "");
        jar_merge(jar, up0.sc);
        long long t = (long long) time(nullptr) * 1000 + (++g_job_seq);
        std::string qrUrl = "https://ssl.ptlogin2.qq.com/ptqrshow?appid=716027609&e=2&l=M&s=3&d=72"
                            "&v=4&t=" + std::to_string(t) + "&daid=383&pt_3rd_aid=100497308";
        Up up1 = http_call(env, qrUrl, "GET",
                           std::map<std::string, std::string>{{"Referer", xlogin}}, "");
        jar_merge(jar, up1.sc);
        if (up1.st != 200 || up1.b64.size() < 100) { cJSON_Delete(out); return nullptr; }
        job.id = rand_id();
        job.extra = xlogin;
        job.jar = jar;
        std::string img = "data:image/png;base64," + up1.b64;
        cJSON_AddStringToObject(out, "mode", "qr");
        cJSON_AddStringToObject(out, "job", job.id.c_str());
        cJSON_AddStringToObject(out, "qrimg", img.c_str());
        cJSON_AddStringToObject(out, "hint",
                                (std::string(plat_hint("qq")) +
                                 " · 当前环境 ptqrlogin 可能 403，扫完若一直等待请改贴 Cookie").c_str());
    } else if (src == "qs") {
        CookieJar jar;
        std::string url = "https://api.qishui.com/passport/web/get_qrcode/?passport_jssdk_version=2.4.13"
                          "&passport_jssdk_type=normal&is_from_ttaccountsdk=1&aid=386088"
                          "&next=https%3A%2F%2Fapi.qishui.com&need_logo=false&need_short_url=false"
                          "&is_new_login=1";
        Up up = http_call(env, url, "GET",
                          std::map<std::string, std::string>{
                                  {"Referer", "https://www.qishui.com/"},
                                  {"Origin",   "https://www.qishui.com/"}}, "");
        jar_merge(jar, up.sc);
        cJSON *j = loads_maybe(up.text);
        if (!j) { cJSON_Delete(out); return nullptr; }
        cJSON *d = Jobj(j, "data");
        std::string token = d ? Jstr(d, "token") : "";
        std::string qr = d ? Jstr(d, "qrcode") : "";
        cJSON_Delete(j);
        if (token.empty() || qr.empty()) { cJSON_Delete(out); return nullptr; }
        if (qr.rfind("data:", 0) != 0) qr = "data:image/png;base64," + qr;
        job.id = token;
        job.jar = jar;
        cJSON_AddStringToObject(out, "mode", "qr");
        cJSON_AddStringToObject(out, "job", token.c_str());
        cJSON_AddStringToObject(out, "qrimg", qr.c_str());
    } else {
        cJSON_Delete(out);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        g_jobs[job.id] = job;
    }
    return out;
}

static cJSON *wy_profile_json(JNIEnv *env, const std::string &cookie) {
    // returns {nickname, uid, avatar} (caller deletes)
    Up up = up_get(env, "https://music.163.com/api/nuser/account/get",
                   "https://music.163.com/", cookie);
    cJSON *j = loads_maybe(up.text);
    cJSON *res = cJSON_CreateObject();
    std::string nick, avatar;
    if (j) {
        cJSON *prof = Jobj(j, "profile");
        if (prof) {
            nick = Jstr(prof, "nickname");
            avatar = Jstr(prof, "avatarUrl");
            cJSON *idit = cJSON_GetObjectItemCaseSensitive(prof, "userId");
            if (idit && cJSON_IsNumber(idit))
                cJSON_AddNumberToObject(res, "uid", idit->valuedouble);
        }
        cJSON_Delete(j);
    }
    cJSON_AddStringToObject(res, "nickname", nick.c_str());
    cJSON_AddStringToObject(res, "avatar", avatar.c_str());
    return res;
}

// poll: returns cJSON {status, message[, nickname]}
static cJSON *login_poll(JNIEnv *env, const std::string &src, const std::string &jobId) {
    QrJob job;
    {
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        auto it = g_jobs.find(jobId);
        if (it == g_jobs.end()) {
            cJSON *res = cJSON_CreateObject();
            cJSON_AddStringToObject(res, "status", "expired");
            cJSON_AddStringToObject(res, "message", "会话不存在，请重新连接");
            return res;
        }
        job = it->second;
    }

    cJSON *res = cJSON_CreateObject();

    if (src == "wy") {
        std::map<std::string, std::string> h = {{"Referer", "https://music.163.com/"}};
        std::string ck = jar_header(job.jar);
        if (!ck.empty()) h["Cookie"] = ck;
        Up up = http_call(env,
                          "https://music.163.com/api/login/qrcode/client/login?type=1&key=" +
                          url_encode(job.id), "GET", h, "");
        jar_merge(job.jar, up.sc);
        cJSON *j = loads_maybe(up.text);
        int code = j ? (int) Jnum(j, "code", 0) : 0;
        if (code == 801) {
            cJSON_AddStringToObject(res, "status", "wait");
            cJSON_AddStringToObject(res, "message", "等待扫码");
        } else if (code == 802) {
            cJSON_AddStringToObject(res, "status", "scanned");
            cJSON_AddStringToObject(res, "message", "已扫码，请在手机上确认");
        } else if (code == 800) {
            cJSON_AddStringToObject(res, "status", "expired");
            cJSON_AddStringToObject(res, "message", "二维码过期，请重新连接");
        } else if (code == 803) {
            std::string cookie = jar_header(job.jar);
            std::string extraCookie = j ? Jstr(j, "cookie") : "";
            if (extraCookie.size() && cookie.find("MUSIC_U") == std::string::npos) {
                if (cookie.size()) cookie += "; ";
                cookie += extraCookie;
            }
            cJSON *prof = wy_profile_json(env, cookie);
            std::string nick = Jstr(prof, "nickname");
            cJSON_Delete(prof);
            if (nick.empty()) nick = "网易云用户";
            Sess s;
            s.cookie = cookie;
            s.nickname = nick;
            s.via = "qr";
            save_session(env, "wy", &s);
            cJSON_AddStringToObject(res, "status", "ok");
            cJSON_AddStringToObject(res, "message", "登录成功");
            cJSON_AddStringToObject(res, "nickname", nick.c_str());
        } else {
            cJSON_AddStringToObject(res, "status", "wait");
            std::string msg = j ? Jstr(j, "message") : "";
            if (msg.empty()) msg = "code " + std::to_string(code);
            cJSON_AddStringToObject(res, "message", msg.c_str());
        }
        if (j) cJSON_Delete(j);
    } else if (src == "qq") {
        std::string qrsig = job.jar.count("qrsig") ? job.jar["qrsig"] : "";
        if (qrsig.empty()) {
            cJSON_AddStringToObject(res, "status", "expired");
            cJSON_AddStringToObject(res, "message", "二维码会话丢失");
        } else {
            int tok = hash33(qrsig);
            std::string xlogin = job.extra.empty()
                                 ? "https://xui.ptlogin2.qq.com/" : job.extra;
            std::string url = "https://ssl.ptlogin2.qq.com/ptqrlogin?u1=" +
                              url_encode("https://y.qq.com/") +
                              "&ptqrtoken=" + std::to_string(tok) +
                              "&ptredirect=0&h=1&t=1&g=1&from_ui=1&ptlang=2052&action=0-0-" +
                              std::to_string((long long) time(nullptr) * 1000) +
                              "&js_ver=25040111&js_type=1&login_sig=" +
                              (job.jar.count("pt_login_sig") ? url_encode(job.jar["pt_login_sig"]) : "") +
                              "&pt_uistyle=40&aid=716027609&daid=383&pt_3rd_aid=100497308";
            std::map<std::string, std::string> h = {{"Referer", xlogin}};
            std::string ck = jar_header(job.jar);
            if (!ck.empty()) h["Cookie"] = ck;
            Up up = http_call(env, url, "GET", h, "");
            jar_merge(job.jar, up.sc);
            std::string text = up.text;
            if (up.st == 403 || text.empty()) {
                cJSON_AddStringToObject(res, "status", "blocked");
                cJSON_AddStringToObject(res, "message", "QQ 轮询被 403 拦截，请改用 Cookie 粘贴");
            } else if (text.find("ptuiCB('66'") != std::string::npos ||
                       text.find("ptuiCB(\"66\"") != std::string::npos) {
                cJSON_AddStringToObject(res, "status", "wait");
                cJSON_AddStringToObject(res, "message", "等待扫码");
            } else if (text.find("ptuiCB('67'") != std::string::npos ||
                       text.find("ptuiCB(\"67\"") != std::string::npos) {
                cJSON_AddStringToObject(res, "status", "scanned");
                cJSON_AddStringToObject(res, "message", "已扫码，请确认");
            } else if (text.find("ptuiCB('65'") != std::string::npos ||
                       text.find("ptuiCB(\"65\"") != std::string::npos) {
                cJSON_AddStringToObject(res, "status", "expired");
                cJSON_AddStringToObject(res, "message", "二维码过期");
            } else if (text.find("ptuiCB('0'") != std::string::npos ||
                       text.find("ptuiCB(\"0\"") != std::string::npos) {
                std::string cookie = jar_header(job.jar);
                Sess s;
                s.cookie = cookie;
                s.nickname = "QQ用户";
                s.via = "qr";
                save_session(env, "qq", &s);
                cJSON_AddStringToObject(res, "status", "ok");
                cJSON_AddStringToObject(res, "message", "登录成功（若无 qm_keyst 仍可能播不了会员曲）");
                cJSON_AddStringToObject(res, "nickname", "QQ用户");
            } else {
                cJSON_AddStringToObject(res, "status", "wait");
                std::string msg = text.substr(0, text.find_first_of("\r\n"));
                if (msg.size() > 80) msg = msg.substr(0, 80);
                cJSON_AddStringToObject(res, "message", msg.c_str());
            }
        }
    } else if (src == "qs") {
        std::map<std::string, std::string> h = {
                {"Referer",      "https://www.qishui.com/"},
                {"Origin",       "https://www.qishui.com"},
                {"Content-Type", "application/x-www-form-urlencoded"}};
        std::string ck = jar_header(job.jar);
        if (!ck.empty()) h["Cookie"] = ck;
        std::string body = "need_logo=false&need_short_url=false&is_frontier=true&token=" +
                           url_encode(job.id) +
                           "&is_new_login=1&next=https%3A%2F%2Fapi.qishui.com";
        std::string url = "https://api.qishui.com/passport/web/check_qrconnect/?"
                          "passport_jssdk_version=2.4.13&passport_jssdk_type=normal"
                          "&is_from_ttaccountsdk=1&aid=386088&iid=27960026095955";
        Up up = http_call(env, url, "POST", h, body);
        jar_merge(job.jar, up.sc);
        cJSON *j = loads_maybe(up.text);
        std::string status;
        std::string sessionid;
        if (j) {
            cJSON *d = Jobj(j, "data");
            if (d) {
                status = Jstr(d, "status");
                sessionid = Jstr(d, "sessionid");
            }
            cJSON_Delete(j);
        }
        if (status == "expired" || status == "4") {
            cJSON_AddStringToObject(res, "status", "expired");
            cJSON_AddStringToObject(res, "message", "二维码过期");
        } else if (status == "scanned" || status == "2") {
            cJSON_AddStringToObject(res, "status", "scanned");
            cJSON_AddStringToObject(res, "message", "已扫码，请在抖音确认");
        } else if (status == "confirmed" || status == "3" || status == "ok" || status == "success" ||
                   !sessionid.empty() || job.jar.count("sessionid")) {
            std::string cookie = jar_header(job.jar);
            std::string sid = sessionid.empty() && job.jar.count("sessionid")
                              ? job.jar["sessionid"] : sessionid;
            if (!sid.empty() && cookie.find("sessionid=") == std::string::npos) {
                if (cookie.size()) cookie += "; ";
                cookie += "sessionid=" + sid;
            }
            Sess s;
            s.cookie = cookie;
            s.nickname = "汽水/抖音用户";
            s.via = "qr";
            save_session(env, "qs", &s);
            cJSON_AddStringToObject(res, "status", "ok");
            cJSON_AddStringToObject(res, "message", "登录成功");
            cJSON_AddStringToObject(res, "nickname", "汽水/抖音用户");
        } else {
            cJSON_AddStringToObject(res, "status", "wait");
            cJSON_AddStringToObject(res, "message",
                                    status.empty() ? "等待抖音扫码" : status.c_str());
        }
    } else {
        cJSON_AddStringToObject(res, "status", "expired");
        cJSON_AddStringToObject(res, "message", "不支持");
    }

    // persist updated jar back to job store
    {
        std::lock_guard<std::mutex> lk(g_jobs_mu);
        auto it = g_jobs.find(jobId);
        if (it != g_jobs.end()) it->second.jar = job.jar;
    }
    return res;
}

// ---------------------------------------------------------------- dispatch

static std::string fail_json(const std::string &msg, int code = 1) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "code", code);
    cJSON_AddStringToObject(j, "message", msg.c_str());
    cJSON_AddItemToObject(j, "data", cJSON_CreateArray());
    std::string s = Jprint(j);
    cJSON_Delete(j);
    return s;
}

static std::string url_decode(const std::string &s) {
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hv(s[i + 1]), l = hv(s[i + 2]);
            if (h >= 0 && l >= 0) {
                o += (char) ((h << 4) | l);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') { o += ' '; continue; }
        o += s[i];
    }
    return o;
}

static std::map<std::string, std::string> parse_query(const std::string &qs) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos <= qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string pair = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

static std::string qget(const std::map<std::string, std::string> &q, const char *k,
                        const std::string &def = "") {
    auto it = q.find(k);
    return it == q.end() ? def : it->second;
}

static std::string merge_key(cJSON *item) {
    std::string t = Jstr(item, "title");
    for (auto &c : t) c = (char) tolower((unsigned char) c);
    std::string a = Jstr(item, "artist");
    size_t slash = a.find('/');
    if (slash != std::string::npos) a = a.substr(0, slash);
    for (auto &c : a) c = (char) tolower((unsigned char) c);
    return t + "|" + a;
}

// ---------------------------------------------- local playlists (import/merge)

/** 从分享文本里抽第一个 http(s) URL。 */
static std::string extract_url(const std::string &text) {
    size_t p = text.find("http");
    while (p != std::string::npos) {
        if (text.compare(p, 7, "http://") == 0 || text.compare(p, 8, "https://") == 0) {
            size_t e = p;
            while (e < text.size() && text[e] != ' ' && text[e] != '\t' &&
                   text[e] != '\r' && text[e] != '\n' && text[e] != '"')
                e++;
            return text.substr(p, e - p);
        }
        p = text.find("http", p + 4);
    }
    return "";
}

/** host 抽取。 */
static std::string url_host(const std::string &url) {
    size_t hs = url.find("://");
    if (hs == std::string::npos) return "";
    std::string rest = url.substr(hs + 3);
    size_t slash = rest.find('/');
    return slash == std::string::npos ? rest : rest.substr(0, slash);
}

/** query 参数。 */
static std::string url_query(const std::string &url, const std::string &key) {
    size_t qm = url.find('?');
    if (qm == std::string::npos) return "";
    std::string qs = url.substr(qm + 1);
    size_t pos = 0;
    while (pos <= qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string pair = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return pair.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

/** token 扫描：before 之后的连续 [A-Za-z0-9]+。 */
static std::string token_after(const std::string &s, const std::string &before) {
    size_t p = s.find(before);
    if (p == std::string::npos) return "";
    p += before.size();
    std::string out;
    while (p < s.size() && (isalnum((unsigned char) s[p]) || s[p] == '_')) out += s[p++];
    return out;
}

/** HTML 里 key 后面的连续数字（specialid=123 / "specialid":123）。 */
static std::string html_num_field(const std::string &html, const std::string &key) {
    size_t p = html.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    while (p < html.size() && (html[p] == '"' || html[p] == '\'' || html[p] == ':' ||
                               html[p] == '=' || html[p] == ' ')) p++;
    std::string num;
    while (p < html.size() && isdigit((unsigned char) html[p])) num += html[p++];
    return num;
}

/** HTML JSON 字符串字段（简易 \uXXXX 转义）。 */
static std::string html_json_str(const std::string &html, const std::string &key) {
    size_t p = html.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    while (p < html.size() && (html[p] == ':' || html[p] == ' ')) p++;
    if (p >= html.size() || html[p] != '"') return "";
    p++;
    std::string out;
    while (p < html.size() && html[p] != '"') {
        if (html[p] == '\\' && p + 1 < html.size()) {
            char c = html[p + 1];
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else if (c == 'u' && p + 5 < html.size()) {
                int code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = html[p + 2 + i];
                    code = code * 16 + (isdigit((unsigned char) h) ? h - '0'
                            : (tolower((unsigned char) h) - 'a' + 10));
                }
                if (code < 0x80) out += (char) code;
                else if (code < 0x800) {
                    out += (char) (0xC0 | (code >> 6));
                    out += (char) (0x80 | (code & 0x3F));
                } else {
                    out += (char) (0xE0 | (code >> 12));
                    out += (char) (0x80 | ((code >> 6) & 0x3F));
                    out += (char) (0x80 | (code & 0x3F));
                }
                p += 4;
            } else out += c;
            p += 2;
        } else out += html[p++];
    }
    return out;
}

/** 酷狗歌单页：special/single/<hash>.html → specialid → m.kugou plist 分页。 */
static cJSON *kg_playlist_special(JNIEnv *env, const std::string &hash) {
    cJSON *res = cJSON_CreateObject();
    cJSON *tracks = cJSON_CreateArray();
    Up up = up_get(env, "https://www.kugou.com/yy/special/single/" + hash + ".html",
                   "https://www.kugou.com/");
    std::string title = html_json_str(up.text, "\"specialname\"");
    if (title.empty()) title = html_json_str(up.text, "\"specialname\":");
    if (title.empty()) title = html_json_str(up.text, "<title>");
    std::string sid = html_num_field(up.text, "\"specialid\"");
    if (sid.empty()) sid = html_num_field(up.text, "specialid=");
    if (sid.empty()) {
        cJSON_AddStringToObject(res, "error", "酷狗歌单页解析失败（没找到 specialid）");
        cJSON_AddItemToObject(res, "tracks", tracks);
        return res;
    }
    if (!title.empty()) cJSON_AddStringToObject(res, "title", title.c_str());
    for (int page = 1; page <= 5; page++) {
        std::string u = "https://m.kugou.com/plist/index&json=true&specialid=" + sid +
                        "&page=" + std::to_string(page) + "&pagesize=100";
        Up pu = up_get(env, u, "https://m.kugou.com/");
        cJSON *j = loads_maybe(pu.text);
        if (!j) break;
        cJSON *songs = Jarr(j, "songs");
        int got = 0;
        if (songs) {
            int n = cJSON_GetArraySize(songs);
            for (int i = 0; i < n; i++) {
                cJSON *s = cJSON_GetArrayItem(songs, i);
                std::string fn = Jstr(s, "filename");   // "歌手 - 歌名"
                std::string fh = Jstr(s, "hash");
                if (fh.empty()) continue;
                std::string artist, name = fn;
                size_t dash = fn.find(" - ");
                if (dash != std::string::npos) {
                    artist = fn.substr(0, dash);
                    name = fn.substr(dash + 3);
                }
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "src", "kg");
                cJSON_AddStringToObject(item, "id", fh.c_str());
                cJSON_AddStringToObject(item, "title", name.c_str());
                cJSON_AddStringToObject(item, "artist", artist.c_str());
                cJSON_AddStringToObject(item, "quality", "酷狗");
                cJSON_AddItemToArray(tracks, item);
                got++;
            }
        }
        cJSON_Delete(j);
        if (got < 100) break;
    }
    cJSON_AddItemToObject(res, "tracks", tracks);
    return res;
}

/** 解析四平台歌单分享链接 → {src,title,tracks[]} 或 {error}。 */
static cJSON *import_parse(JNIEnv *env, const std::string &raw) {
    std::string url = extract_url(raw);
    cJSON *res = cJSON_CreateObject();
    if (url.empty()) {
        cJSON_AddStringToObject(res, "error", "没识别到链接，把歌单分享口令整段粘贴进来");
        return res;
    }
    std::string host = url_host(url);
    // 短链先展开拿最终 URL
    if (host == "t1.kugou.com" || host == "t.kugou.com" || host == "qishui.douyin.com" ||
        host == "v.douyin.com" || host == "c6.y.qq.com") {
        Up up = up_get(env, url, "");
        if (!up.url.empty()) { url = up.url; host = url_host(url); }
    }

    cJSON *data = nullptr;
    std::string src;
    if (url.find("music.163.com") != std::string::npos ||
        url.find("163.com") != std::string::npos) {
        src = "wy";
        std::string pid = url_query(url, "id");
        if (pid.empty()) pid = token_after(url, "/playlist/");
        if (pid.empty()) pid = token_after(url, "playlist%3Fid%3D");
        if (pid.empty()) {
            data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "error", "网易云链接里没找到歌单 id");
        } else {
            data = wy_playlist(env, pid);
        }
    } else if (url.find("qq.com") != std::string::npos) {
        src = "qq";
        std::string pid = url_query(url, "id");
        if (pid.empty()) pid = token_after(url, "/playlist/");
        if (pid.empty()) pid = token_after(url, "taoge.html%3Fid%3D");
        if (pid.empty()) {
            data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "error", "QQ 链接里没找到歌单 id");
        } else {
            data = qq_playlist(env, pid);
        }
    } else if (url.find("kugou.com") != std::string::npos) {
        src = "kg";
        std::string h = token_after(url, "/special/single/");
        if (h.empty()) h = token_after(url, "special/single%2F");
        if (h.empty()) {
            data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "error",
                "酷狗链接没识别出歌单页；在酷狗 APP 打开歌单 → 分享 → 复制链接（含 /special/single/）再试");
        } else {
            data = kg_playlist_special(env, h);
        }
    } else if (url.find("douyin.com") != std::string::npos) {
        src = "qs";
        std::string pid = token_after(url, "/playlist/");
        if (pid.empty()) pid = url_query(url, "playlist_id");
        if (pid.empty()) {
            data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "error",
                "汽水链接没识别出歌单 id；在汽水/抖音网页版歌单页地址栏复制链接（含 /playlist/数字）再试");
        } else {
            data = qs_playlist(env, pid);
        }
    } else {
        cJSON_AddStringToObject(res, "error",
            "没认出这个链接的平台（支持网易云 / QQ / 酷狗 / 汽水的歌单分享链接）");
        return res;
    }
    if (!data) {
        cJSON_AddStringToObject(res, "error", "解析失败");
        return res;
    }
    cJSON_AddStringToObject(res, "src", src.c_str());
    if (cJSON_GetObjectItemCaseSensitive(data, "error")) {
        cJSON_AddItemToObject(res, "error", cJSON_DetachItemFromObject(data, "error"));
    } else {
        cJSON *title = cJSON_DetachItemFromObject(data, "title");
        if (title) cJSON_AddItemToObject(res, "title", title);
        cJSON *cover = cJSON_DetachItemFromObject(data, "cover");
        if (cover) cJSON_AddItemToObject(res, "cover", cover);
        cJSON *tracks = cJSON_DetachItemFromObject(data, "tracks");
        if (tracks) cJSON_AddItemToObject(res, "tracks", tracks);
        else cJSON_AddItemToObject(res, "tracks", cJSON_CreateArray());
    }
    cJSON_Delete(data);
    return res;
}

/** 本地歌单持久化：mg_local_pls.json = 数组 [{id,title,cover,tracks:[...]}] */
static cJSON *localpls_load(JNIEnv *env) {
    std::string raw = store_read(env, "mg_local_pls.json");
    cJSON *arr = raw.empty() ? nullptr : cJSON_Parse(raw.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return cJSON_CreateArray();
    }
    return arr;
}

static void localpls_save(JNIEnv *env, cJSON *arr) {
    store_write(env, "mg_local_pls.json", Jprint(arr));
}

/** 按找 id；没有就生成 local_<ms>。返回 id 字符串。 */
static std::string localpls_upsert(JNIEnv *env, const char *id, const char *title,
                                   const char *cover, cJSON *tracks) {
    cJSON *arr = localpls_load(env);
    std::string outId = id && *id ? id : "";
    if (outId.empty()) {
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        outId = "local_" + std::to_string(ms);
    }
    cJSON *hit = nullptr;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        if (Jstr(it, "id") == outId) { hit = it; break; }
    }
    cJSON *rec;
    if (hit) {
        rec = hit;
    } else {
        rec = cJSON_CreateObject();
        cJSON_AddStringToObject(rec, "id", outId.c_str());
        cJSON_AddItemToArray(arr, rec);
    }
    if (title && *title) cJSON_ReplaceItemInObjectCaseSensitive(rec, "title", cJSON_CreateString(title));
    else if (!cJSON_GetObjectItemCaseSensitive(rec, "title"))
        cJSON_AddStringToObject(rec, "title", "导入的歌单");
    if (cover && *cover) {
        if (cJSON_GetObjectItemCaseSensitive(rec, "cover"))
            cJSON_ReplaceItemInObjectCaseSensitive(rec, "cover", cJSON_CreateString(cover));
        else cJSON_AddStringToObject(rec, "cover", cover);
    }
    if (tracks) {
        if (cJSON_GetObjectItemCaseSensitive(rec, "tracks"))
            cJSON_ReplaceItemInObjectCaseSensitive(rec, "tracks", tracks);
        else cJSON_AddItemToObject(rec, "tracks", tracks);
    } else if (!cJSON_GetObjectItemCaseSensitive(rec, "tracks")) {
        cJSON_AddItemToObject(rec, "tracks", cJSON_CreateArray());
    }
    localpls_save(env, arr);
    cJSON_Delete(arr);
    return outId;
}

static std::string dispatch(JNIEnv *env, const std::string &method, const std::string &path,
                            const std::string &body) {
    load_sessions(env);

    std::string p = path;
    std::string qsStr;
    size_t qm = path.find('?');
    if (qm != std::string::npos) {
        p = path.substr(0, qm);
        qsStr = path.substr(qm + 1);
    }
    auto q = parse_query(qsStr);

    // ---- health
    if (p == "/api/health") {
        return "{\"code\":0,\"data\":{\"ok\":true}}";
    }

    // ---- login status
    if (p == "/api/login/status") {
        cJSON *data = cJSON_CreateObject();
        for (int i = 0; i < 4; i++) {
            std::string src = PLATS[i];
            cJSON *it = cJSON_CreateObject();
            std::lock_guard<std::mutex> lk(g_sess_mu);
            auto found = g_sess.find(src);
            cJSON_AddBoolToObject(it, "logged", found != g_sess.end());
            cJSON_AddStringToObject(it, "nickname",
                                    found == g_sess.end() ? "" : found->second.nickname.c_str());
            cJSON_AddStringToObject(it, "via",
                                    found == g_sess.end() ? "" : found->second.via.c_str());
            cJSON_AddStringToObject(it, "hint", plat_hint(src));
            cJSON_AddItemToObject(data, src.c_str(), it);
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- login qr
    if (p == "/api/login/qr") {
        std::string src = qget(q, "src");
        if (src != "qq" && src != "wy" && src != "kg" && src != "qs") {
            return fail_json("src must be qq|wy|kg|qs");
        }
        cJSON *data = login_create(env, src);
        if (!data) return fail_json("二维码拉取失败");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- login poll
    if (p == "/api/login/poll") {
        std::string src = qget(q, "src");
        std::string jobId = qget(q, "job");
        if (src.empty() || jobId.empty()) return fail_json("missing src/job");
        cJSON *data = login_poll(env, src, jobId);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- search
    if (p == "/api/search") {
        std::string query = qget(q, "q");
        if (query.empty()) query = qget(q, "keyword");
        std::string src = qget(q, "src", "all");
        if (query.empty()) return fail_json("missing q");

        cJSON *merged = cJSON_CreateArray();
        if (src == "qq" || src == "all") {
            cJSON *r = qq_search(env, query);
            int n = cJSON_GetArraySize(r);
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(merged, cJSON_Duplicate(cJSON_GetArrayItem(r, i), 1));
            cJSON_Delete(r);
        }
        if (src == "wy" || src == "all") {
            cJSON *r = wy_search(env, query);
            int n = cJSON_GetArraySize(r);
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(merged, cJSON_Duplicate(cJSON_GetArrayItem(r, i), 1));
            cJSON_Delete(r);
        }
        if (src == "kg" || src == "all") {
            cJSON *r = kg_search(env, query);
            int n = cJSON_GetArraySize(r);
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(merged, cJSON_Duplicate(cJSON_GetArrayItem(r, i), 1));
            cJSON_Delete(r);
        }
        if (src == "qs" || src == "all") {
            cJSON *r = qs_search_note(query);
            int n = cJSON_GetArraySize(r);
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(merged, cJSON_Duplicate(cJSON_GetArrayItem(r, i), 1));
            cJSON_Delete(r);
        }
        // dedupe
        cJSON *out = cJSON_CreateArray();
        std::vector<std::string> seen;
        int n = cJSON_GetArraySize(merged);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(merged, i);
            std::string k = merge_key(it);
            bool dup = false;
            for (auto &x : seen) if (x == k) { dup = true; break; }
            if (dup) continue;
            seen.push_back(k);
            cJSON_AddStringToObject(it, "src_label", src_label(Jstr(it, "src").c_str()));
            cJSON_AddItemToArray(out, cJSON_Duplicate(it, 1));
        }
        cJSON_Delete(merged);

        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", out);
        cJSON_AddStringToObject(j, "query", query.c_str());
        cJSON_AddStringToObject(j, "src", src.c_str());
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- playurl（主源失败时自动跨平台换源：QQ 优先——SVIP 权益直接生效）
    if (p == "/api/playurl") {
        std::string src = qget(q, "src");
        std::string sid = qget(q, "id");
        std::string stitle = qget(q, "title");
        std::string sartist = qget(q, "artist");
        std::string sq = qget(q, "q", "lossless");   // std | high | lossless
        int qStart = sq == "std" ? 2 : sq == "high" ? 1 : 0;
        int qBr = sq == "std" ? 128000 : sq == "high" ? 320000 : 0;
        if (src.empty() || sid.empty()) return fail_json("missing src/id");
        cJSON *data = nullptr;
        if (src == "wy") data = wy_playurl(env, sid, qBr);
        else if (src == "qq") data = qq_playurl(env, sid, qStart);
        else if (src == "kg") data = kg_playurl(env, sid);
        else if (src == "qs") data = qs_playurl(env, sid);
        else return fail_json("unknown src");

        std::string purl = Jstr(data, "url");
        if (purl.empty() && !stitle.empty()) {
            std::string query = stitle;
            if (!sartist.empty()) query += " " + sartist;
            const char *order[3] = {"qq", "wy", "kg"};
            auto trySrc = [&](const std::string &alt) -> cJSON * {
                cJSON *hits = nullptr;
                if (alt == "qq") hits = qq_search(env, query);
                else if (alt == "wy") hits = wy_search(env, query);
                else if (alt == "kg") hits = kg_search(env, query);
                if (!hits) return nullptr;
                int n = cJSON_GetArraySize(hits);
                for (int i = 0; i < n && i < 3; i++) {
                    cJSON *h = cJSON_GetArrayItem(hits, i);
                    std::string aid = Jstr(h, "id");
                    if (aid.empty()) continue;
                    cJSON *alt2 = nullptr;
                    if (alt == "qq") alt2 = qq_playurl(env, aid, qStart);
                    else if (alt == "wy") alt2 = wy_playurl(env, aid, qBr);
                    else if (alt == "kg") alt2 = kg_playurl(env, aid);
                    if (alt2 && !Jstr(alt2, "url").empty()) {
                        // 用搜索结果的标题/歌手信息补全
                        std::string at = Jstr(h, "title");
                        std::string aa = Jstr(h, "artist");
                        if (!at.empty() && Jstr(alt2, "title").empty())
                            cJSON_AddStringToObject(alt2, "title", at.c_str());
                        if (!aa.empty() && Jstr(alt2, "artist").empty())
                            cJSON_AddStringToObject(alt2, "artist", aa.c_str());
                        cJSON_AddStringToObject(alt2, "switched_from", src_label(src));
                        cJSON_AddStringToObject(alt2, "switched_to", src_label(alt));
                        cJSON_Delete(hits);
                        return alt2;
                    }
                    if (alt2) cJSON_Delete(alt2);
                }
                cJSON_Delete(hits);
                return nullptr;
            };
            for (int i = 0; i < 3; i++) {
                std::string alt = order[i];
                if (alt == src) continue;
                cJSON *alt2 = trySrc(alt);
                if (alt2) {
                    cJSON_Delete(data);
                    data = alt2;
                    break;
                }
            }
        }

        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- lyric
    if (p == "/api/lyric") {
        std::string src = qget(q, "src");
        std::string sid = qget(q, "id");
        cJSON *data = nullptr;
        if (src == "wy") data = wy_lyric(env, sid);
        else if (src == "qq") data = qq_lyric(env, sid);
        else if (src == "qs") {
            cJSON *pp = qs_playurl(env, sid);
            data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "src", "qs");
            cJSON_AddStringToObject(data, "id", sid.c_str());
            cJSON_AddStringToObject(data, "lrc", Jstr(pp, "lrc").c_str());
            cJSON_Delete(pp);
        } else return fail_json("lyric not implemented for " + src);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- playlist
    if (p == "/api/playlist") {
        std::string src = qget(q, "src");
        std::string pid = qget(q, "id");
        if (pid.empty() || (src != "wy" && src != "qq" && src != "qs")) {
            return fail_json("need src=wy|qq|qs and id");
        }
        cJSON *data = src == "wy" ? wy_playlist(env, pid)
                      : src == "qq" ? qq_playlist(env, pid)
                      : qs_playlist(env, pid);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- newsong
    if (p == "/api/newsong") {
        cJSON *data = wy_newsong(env);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    if (p == "/api/recommend") {
        cJSON *data = wy_recommend(env, 8);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    if (p == "/api/mine/playlists") {
        cJSON *data = wy_user_playlists(env);
        cJSON *qqs = qq_user_playlists(env);
        int n = cJSON_GetArraySize(qqs);
        for (int i = 0; i < n; i++)
            cJSON_AddItemToArray(data, cJSON_Duplicate(cJSON_GetArrayItem(qqs, i), 1));
        cJSON_Delete(qqs);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", data);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- local playlists (imported, mergeable)
    if (p == "/api/local/playlists" && method == "GET") {
        cJSON *data = localpls_load(env);
        // summary: keep order but only id/title/cover/count
        cJSON *sum = cJSON_CreateArray();
        int n = cJSON_GetArraySize(data);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(data, i);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", Jstr(it, "id").c_str());
            cJSON_AddStringToObject(o, "title", Jstr(it, "title").c_str());
            cJSON_AddStringToObject(o, "cover", Jstr(it, "cover").c_str());
            cJSON *tr = Jarr(it, "tracks");
            cJSON_AddNumberToObject(o, "count", tr ? cJSON_GetArraySize(tr) : 0);
            cJSON_AddItemToArray(sum, o);
        }
        cJSON_Delete(data);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON_AddItemToObject(j, "data", sum);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    if (p == "/api/local/playlist" && method == "GET") {
        std::string id = qget(q, "id");
        cJSON *arr = localpls_load(env);
        cJSON *found = nullptr;
        int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(arr, i);
            if (Jstr(it, "id") == id) { found = it; break; }
        }
        cJSON *j = cJSON_CreateObject();
        if (found) {
            cJSON *data = cJSON_Duplicate(found, 1);
            cJSON_AddNumberToObject(j, "code", 0);
            cJSON_AddItemToObject(j, "data", data);
        } else {
            cJSON_AddNumberToObject(j, "code", 1);
            cJSON_AddStringToObject(j, "message", "歌单不存在");
        }
        cJSON_Delete(arr);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    if (method == "POST" && p == "/api/local/playlists") {
        cJSON *b = body.empty() ? nullptr : cJSON_Parse(body.c_str());
        std::string id = b ? Jstr(b, "id") : "";
        std::string title = b ? Jstr(b, "title") : "";
        std::string cover = b ? Jstr(b, "cover") : "";
        cJSON *tracks = b ? Jobj(b, "tracks") : nullptr;
        if (tracks && !cJSON_IsArray(tracks)) tracks = nullptr;
        if (title.empty() && !tracks) {
            cJSON_Delete(b);
            return fail_json("need title or tracks");
        }
        std::string outId = localpls_upsert(env, id.c_str(), title.c_str(),
                                            cover.c_str(),
                                            tracks ? cJSON_Duplicate(tracks, 1) : nullptr);
        cJSON_Delete(b);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", outId.c_str());
        cJSON_AddItemToObject(j, "data", d);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    if (method == "POST" && p == "/api/local/playlist/delete") {
        cJSON *b = body.empty() ? nullptr : cJSON_Parse(body.c_str());
        std::string id = b ? Jstr(b, "id") : "";
        if (b) cJSON_Delete(b);
        cJSON *arr = localpls_load(env);
        int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(arr, i);
            if (Jstr(it, "id") == id) {
                cJSON_DeleteItemFromArray(arr, i);
                break;
            }
        }
        localpls_save(env, arr);
        cJSON_Delete(arr);
        return "{\"code\":0}";
    }

    // ---- import a shared playlist link -> saved as a new local playlist
    if (method == "POST" && p == "/api/import/playlist") {
        cJSON *b = body.empty() ? nullptr : cJSON_Parse(body.c_str());
        std::string url = b ? Jstr(b, "url") : "";
        if (b) cJSON_Delete(b);
        if (url.empty()) return fail_json("missing url");
        cJSON *parsed = import_parse(env, url);
        if (cJSON_GetObjectItemCaseSensitive(parsed, "error")) {
            std::string msg = Jstr(parsed, "error");
            cJSON_Delete(parsed);
            return fail_json(msg);
        }
        std::string title = Jstr(parsed, "title");
        if (title.empty()) {
            title = src_label(Jstr(parsed, "src")) + std::string(" 歌单");
        }
        std::string cover = Jstr(parsed, "cover");
        cJSON *tracks = Jarr(parsed, "tracks");
        cJSON *tracksCopy = tracks ? cJSON_Duplicate(tracks, 1) : cJSON_CreateArray();
        std::string outId = localpls_upsert(env, "", title.c_str(), cover.c_str(), tracksCopy);
        int cnt = cJSON_GetArraySize(tracksCopy);
        std::string psrc = Jstr(parsed, "src");
        cJSON_Delete(parsed);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", outId.c_str());
        cJSON_AddStringToObject(d, "title", title.c_str());
        cJSON_AddStringToObject(d, "src", psrc.c_str());
        cJSON_AddNumberToObject(d, "count", cnt);
        cJSON_AddItemToObject(j, "data", d);
        std::string s = Jprint(j);
        cJSON_Delete(j);
        return s;
    }

    // ---- POST endpoints
    if (method == "POST" && (p == "/api/session" || p == "/api/login/cookie")) {
        cJSON *b = body.empty() ? nullptr : cJSON_Parse(body.c_str());
        std::string src = b ? Jstr(b, "src") : "";
        std::string cookie = b ? Jstr(b, "cookie") : "";
        if (b) cJSON_Delete(b);
        if (src != "qq" && src != "wy" && src != "kg" && src != "qs") {
            return fail_json("src must be qq|wy|kg|qs");
        }
        if (cookie.empty()) {
            save_session(env, src, nullptr);
            return "{\"code\":0,\"data\":{\"src\":\"" + src + "\",\"saved\":false}}";
        }
        // normalize
        std::string c = cookie;
        {
            // collapse whitespace
            std::string o;
            bool sp = false;
            for (char ch : c) {
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                    if (!sp && !o.empty()) o += ' ';
                    sp = true;
                } else {
                    o += ch;
                    sp = false;
                }
            }
            c = o;
        }
        {
            size_t p2;
            while ((p2 = c.find("Cookie:")) != std::string::npos) c.replace(p2, 7, "");
            while ((p2 = c.find("cookie:")) != std::string::npos) c.replace(p2, 7, "");
        }
        std::string cUp = c;
        for (auto &ch : cUp) ch = (char) toupper((unsigned char) ch);
        std::string cLow = c;
        for (auto &ch : cLow) ch = (char) tolower((unsigned char) ch);

        std::string nick, err;
        if (src == "wy" && cUp.find("MUSIC_U") == std::string::npos) {
            err = "网易云 Cookie 必须包含 MUSIC_U（在 music.163.com 登录后从请求头复制）";
        } else if (src == "qq" && cLow.find("qm_keyst") == std::string::npos &&
                   cLow.find("p_skey") == std::string::npos &&
                   cLow.find("uin=") == std::string::npos) {
            err = "QQ Cookie 需含 uin，以及 qm_keyst 或 p_skey 之一（y.qq.com 登录后复制）";
        }
        if (!err.empty()) return fail_json(err);

        std::string defNick = src == "wy" ? "网易云用户" : src == "qq" ? "QQ用户"
                                                    : src == "kg" ? "酷狗用户" : "汽水用户";
        if (src == "wy") {
            cJSON *prof = wy_profile_json(env, c);
            nick = Jstr(prof, "nickname");
            cJSON_Delete(prof);
        }
        if (nick.empty()) nick = defNick;
        Sess s;
        s.cookie = c;
        s.nickname = nick;
        s.via = "cookie";
        save_session(env, src, &s);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "code", 0);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "src", src.c_str());
        cJSON_AddStringToObject(d, "nickname", nick.c_str());
        cJSON_AddStringToObject(d, "via", "cookie");
        cJSON_AddItemToObject(j, "data", d);
        std::string out = Jprint(j);
        cJSON_Delete(j);
        return out;
    }

    return fail_json("not found", 1);
}

// ---------------------------------------------------------------- JNI entry

extern "C" JNIEXPORT jstring JNICALL
Java_com_meloglass_app_MainActivity_00024Bridge_api(JNIEnv *env, jobject /*thiz*/,
                                                    jstring jmethod, jstring jpath, jstring jbody) {
    std::string method = jstr2(env, jmethod);
    std::string path = jstr2(env, jpath);
    std::string body = jstr2(env, jbody);
    std::string result = dispatch(env, method, path, body);
    return env->NewStringUTF(result.c_str());
}
