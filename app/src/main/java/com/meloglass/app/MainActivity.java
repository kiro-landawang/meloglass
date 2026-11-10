package com.meloglass.app;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

/**
 * MeloGlass 液态玻璃音乐 APP —— Java 胶水层。
 * 所有业务逻辑（搜索 / 播放链 / 扫码登录 / 会话）在 C++（app/src/main/cpp/meloglass.cpp）。
 * Java 只负责：
 *   1. WebView 宿主 + JS 桥（AndroidBridge.api → native C++）
 *   2. Http.request：C++ 通过 JNI 调用平台 HTTP 栈（TLS 由系统处理）
 *   3. Store.read/write：C++ 持久化会话到 filesDir
 */
public class MainActivity extends Activity {

    private static final String START_URL = "file:///android_asset/www/index.html";

    static {
        System.loadLibrary("meloglass");
    }

    private WebView web;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FileDirs.init(getFilesDir());
        web = new WebView(this);
        setContentView(web);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        web.setWebViewClient(new WebViewClient());
        web.setWebChromeClient(new WebChromeClient());
        web.addJavascriptInterface(new Bridge(), "AndroidBridge");
        web.loadUrl(START_URL);
    }

    @Override
    public void onBackPressed() {
        if (web != null && web.canGoBack()) {
            web.goBack();
        } else {
            super.onBackPressed();
        }
    }

    /** JS 调用入口：AndroidBridge.api(method, path, bodyJson) → C++ */
    public class Bridge {
        @JavascriptInterface
        public native String api(String method, String path, String body);

        @JavascriptInterface
        public String ping() {
            return "meloglass-cpp";
        }
    }

    /** filesDir 持有者（onCreate 时初始化，供静态 Store 使用）。 */
    public static class FileDirs {
        private static File filesDir = null;

        public static void init(File dir) {
            filesDir = dir;
        }

        public static File filesDir() {
            return filesDir;
        }
    }

    /** C++ 通过 JNI 调用的 HTTP 传输。返回 JSON：{s, b64, ct, sc:[set-cookie...]} */
    public static class Http {
        public static String request(String url, String method, String headersJson, byte[] body) {
            JSONObject resp = new JSONObject();
            HttpURLConnection conn = null;
            try {
                conn = (HttpURLConnection) new URL(url).openConnection();
                conn.setRequestMethod(method);
                conn.setConnectTimeout(15000);
                conn.setReadTimeout(20000);
                conn.setInstanceFollowRedirects(true);
                conn.setRequestProperty("User-Agent",
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                + "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
                conn.setRequestProperty("Accept", "application/json,text/javascript,*/*;q=0.8");
                if (headersJson != null && headersJson.length() > 0) {
                    JSONObject hh = new JSONObject(headersJson);
                    Iterator<String> it = hh.keys();
                    while (it.hasNext()) {
                        String k = it.next();
                        String v = hh.optString(k, "");
                        if (v.length() > 0) conn.setRequestProperty(k, v);
                    }
                }
                if (body != null && body.length > 0) {
                    conn.setDoOutput(true);
                    conn.getOutputStream().write(body);
                    conn.getOutputStream().flush();
                }
                int code = conn.getResponseCode();
                InputStream ris = code >= 400 ? conn.getErrorStream() : conn.getInputStream();
                ByteArrayOutputStream buf = new ByteArrayOutputStream();
                if (ris != null) {
                    byte[] chunk = new byte[8192];
                    int n;
                    while ((n = ris.read(chunk)) > 0) buf.write(chunk, 0, n);
                    ris.close();
                }
                String ct = conn.getContentType() == null ? "" : conn.getContentType();
                List<String> sc = new ArrayList<String>();
                Map<String, List<String>> hdrs = conn.getHeaderFields();
                if (hdrs != null) {
                    for (Map.Entry<String, List<String>> e : hdrs.entrySet()) {
                        if (e.getKey() != null && e.getKey().equalsIgnoreCase("Set-Cookie")
                                && e.getValue() != null) {
                            sc.addAll(e.getValue());
                        }
                    }
                }
                resp.put("s", code);
                resp.put("b64", android.util.Base64.encodeToString(
                        buf.toByteArray(), android.util.Base64.NO_WRAP));
                resp.put("ct", ct);
                resp.put("sc", new JSONArray(sc));
            } catch (Exception e) {
                try {
                    resp.put("s", 0);
                    resp.put("b64", android.util.Base64.encodeToString(
                            String.valueOf(e.getMessage()).getBytes(StandardCharsets.UTF_8),
                            android.util.Base64.NO_WRAP));
                    resp.put("ct", "text/plain");
                    resp.put("sc", new JSONArray());
                } catch (Exception ignored) {
                }
            } finally {
                if (conn != null) conn.disconnect();
            }
            return resp.toString();
        }
    }

    /** C++ 通过 JNI 调用的文件存取（filesDir 下）。 */
    public static class Store {
        public static String read(String name) {
            try {
                File base = FileDirs.filesDir();
                if (base == null) return "";
                File f = new File(base, name);
                if (!f.exists()) return "";
                FileInputStream in = new FileInputStream(f);
                ByteArrayOutputStream buf = new ByteArrayOutputStream();
                byte[] chunk = new byte[8192];
                int n;
                while ((n = in.read(chunk)) > 0) buf.write(chunk, 0, n);
                in.close();
                return new String(buf.toByteArray(), StandardCharsets.UTF_8);
            } catch (Exception e) {
                return "";
            }
        }

        public static void write(String name, String content) {
            try {
                File base = FileDirs.filesDir();
                if (base == null) return;
                File f = new File(base, name);
                FileOutputStream out = new FileOutputStream(f);
                out.write(content.getBytes(StandardCharsets.UTF_8));
                out.flush();
                out.close();
            } catch (Exception ignored) {
            }
        }
    }
}
