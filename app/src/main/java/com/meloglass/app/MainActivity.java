package com.meloglass.app;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

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
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * MeloGlass 液态玻璃音乐 APP —— Java 胶水层。
 * 所有业务逻辑（搜索 / 播放链 / 会话）在 C++（app/src/main/cpp/meloglass.cpp）。
 * Java 只负责：
 *   1. WebView 宿主 + JS 桥（AndroidBridge.api → native C++）
 *   2. Http.request：C++ 通过 JNI 调用平台 HTTP 栈（TLS 由系统处理）
 *   3. Store.read/write：C++ 持久化会话到 filesDir
 *   4. openWebLogin：内置 WebView 打开各平台官方登录页（QQ/微信/账号密码/扫码都随官方支持），
 *      登录态 Cookie 自动抓回，交给 JS → C++ 保存
 */
public class MainActivity extends Activity {

    private static final String START_URL = "file:///android_asset/www/index.html";

    static {
        System.loadLibrary("meloglass");
    }

    private WebView web;
    private final Handler uiHandler = new Handler(Looper.getMainLooper());

    // ---- 官方页登录（WebView 全屏层）----
    private ViewGroup loginOverlay;
    private WebView loginWebView;
    private Runnable loginPollTask;
    private String loginPollSrc;

    /** 各平台：官方登录入口 URL + 登录态检测域 + 成功关键字 */
    private static final Map<String, Object[]> LOGIN_CFG = new HashMap<>();
    static {
        LOGIN_CFG.put("qq", new Object[]{
                "https://xui.ptlogin2.qq.com/cgi-bin/xlogin?appid=716027609&daid=383&style=35"
                        + "&s_url=https%3A%2F%2Fy.qq.com%2F&pt_3rd_aid=100497308",
                new String[]{
                        "https://y.qq.com/", "https://i.y.qq.com/", "https://u.y.qq.com/",
                        "https://ui.ptlogin2.qq.com/", "https://xui.ptlogin2.qq.com/",
                        "https://ptlogin2.qq.com/", "https://graph.qq.com/",
                        "https://open.weixin.qq.com/", "https://qq.com/"
                },
                new String[]{"qm_keyst", "uin=", "wxuin", "p_skey", "luin", "p_uin"}});
        LOGIN_CFG.put("kg", new Object[]{
                "https://openplat-user.kugou.com/weixin/index.php?ktype=0&appid=1058&https=1&domain=kugou.com",
                new String[]{
                        "https://login-user.kugou.com/", "https://www.kugou.com/",
                        "https://m.kugou.com/", "https://user.kugou.com/",
                        "https://openplat-user.kugou.com/", "https://open.weixin.qq.com/",
                        "https://graph.qq.com/", "https://xui.ptlogin2.qq.com/"
                },
                new String[]{"userid=", "kugou_login_name", "KuGoo=", "Kugoo", "token=", "KugooID"}});
        LOGIN_CFG.put("qs", new Object[]{
                "https://music.douyin.com/",
                new String[]{
                        "https://music.douyin.com/", "https://qishui.douyin.com/",
                        "https://www.douyin.com/", "https://sso.douyin.com/",
                        "https://www.qishui.com/", "https://api.qishui.com/"
                },
                new String[]{"sessionid", "sid_tt", "sid_guard", "sid_ucp_v1"}});
        LOGIN_CFG.put("wy", new Object[]{
                "https://music.163.com/m/login",
                new String[]{"https://music.163.com/", "https://y.music.163.com/"},
                new String[]{"MUSIC_U"}});
    }

    private static final Map<String, String> LOGIN_TITLE = new HashMap<>();
    static {
        LOGIN_TITLE.put("qq", "QQ音乐（支持 QQ / 微信登录）");
        LOGIN_TITLE.put("kg", "酷狗音乐");
        LOGIN_TITLE.put("qs", "汽水音乐（抖音扫码登录）");
        LOGIN_TITLE.put("wy", "网易云音乐");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FileDirs.init(getFilesDir());
        CookieManager.getInstance().setAcceptCookie(true);
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
        if (loginOverlay != null) {
            closeLoginWeb();
            return;
        }
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

        @JavascriptInterface
        public void openWebLogin(String src, String url) {
            uiHandler.post(() -> showLoginWeb(src, url));
        }
    }

    // ---------------- 官方页登录 ----------------

    private void showLoginWeb(String src, String url) {
        stopLoginPoll();
        if (loginOverlay != null) {
            ((ViewGroup) getWindow().getDecorView()).removeView(loginOverlay);
            loginOverlay = null;
        }
        loginPollSrc = src;

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackgroundColor(Color.parseColor("#0B0E1A"));

        // 顶栏：标题 + 关闭
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(16), dp(10), dp(16), dp(10));
        TextView title = new TextView(this);
        title.setText("登录 " + LOGIN_TITLE.get(src));
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(15);
        title.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView close = new TextView(this);
        close.setText("✕ 关闭");
        close.setTextColor(0xFFB9C0FF);
        close.setTextSize(15);
        close.setPadding(dp(14), dp(8), 0, dp(8));
        close.setOnClickListener(v -> closeLoginWeb());
        bar.addView(title);
        bar.addView(close);
        box.addView(bar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        loginWebView = new WebView(this);
        WebSettings ws = loginWebView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setJavaScriptCanOpenWindowsAutomatically(true);
        ws.setSupportMultipleWindows(false);
        ws.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        ws.setUserAgentString(
                "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 "
                        + "(KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36");
        CookieManager.getInstance().setAcceptThirdPartyCookies(loginWebView, true);
        loginWebView.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, String u) {
                if (u != null && !u.startsWith("http://") && !u.startsWith("https://")) {
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(u)));
                    } catch (Exception ignored) {
                    }
                    return true;
                }
                return false;
            }

            @Override
            public void onPageFinished(WebView v, String u) {
                // SPA 页面登录按钮渲染较慢，多注入几次提高命中
                injectLoginHelper(src);
                uiHandler.postDelayed(() -> injectLoginHelper(src), 1500);
                uiHandler.postDelayed(() -> injectLoginHelper(src), 3500);
            }
        });
        loginWebView.setWebChromeClient(new WebChromeClient());
        box.addView(loginWebView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        loginOverlay = box;
        addContentView(box, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        loginWebView.loadUrl(url);

        startLoginPoll(src);
    }

    private void startLoginPoll(String src) {
        Object[] cfg = LOGIN_CFG.get(src);
        if (cfg == null) return;
        String[] domains = (String[]) cfg[1];
        String[] keys = (String[]) cfg[2];
        loginPollTask = new Runnable() {
            @Override
            public void run() {
                if (loginOverlay == null || loginPollSrc == null || !loginPollSrc.equals(src)) return;
                String merged = mergedCookies(domains);
                if (containsAny(merged, keys)) {
                    String js = "window.__nativeLogin && window.__nativeLogin("
                            + JSONObject.quote(src) + "," + JSONObject.quote(merged) + ")";
                    // 回调必须打到 APP 主 WebView，登录页里没有 __nativeLogin
                    if (web != null) web.evaluateJavascript(js, null);
                    Toast.makeText(MainActivity.this, "检测到登录成功，已保存", Toast.LENGTH_SHORT).show();
                    uiHandler.postDelayed(MainActivity.this::closeLoginWeb, 900);
                    return;
                }
                uiHandler.postDelayed(this, 1500);
            }
        };
        uiHandler.postDelayed(loginPollTask, 2500);
    }

    private void stopLoginPoll() {
        if (loginPollTask != null) {
            uiHandler.removeCallbacks(loginPollTask);
            loginPollTask = null;
        }
        loginPollSrc = null;
    }

    private void closeLoginWeb() {
        stopLoginPoll();
        if (loginOverlay != null && loginOverlay.getParent() instanceof ViewGroup) {
            ((ViewGroup) loginOverlay.getParent()).removeView(loginOverlay);
        }
        if (loginWebView != null) {
            loginWebView.destroy();
            loginWebView = null;
        }
        loginOverlay = null;
    }

    private String mergedCookies(String[] domains) {
        Map<String, String> jar = new LinkedHashMap<>();
        CookieManager cm = CookieManager.getInstance();
        for (String d : domains) {
            String c = cm.getCookie(d);
            if (c == null) continue;
            for (String pair : c.split(";")) {
                int eq = pair.indexOf('=');
                if (eq <= 0) continue;
                String k = pair.substring(0, eq).trim();
                String v = pair.substring(eq + 1).trim();
                if (!k.isEmpty()) jar.put(k, v);
            }
        }
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> e : jar.entrySet()) {
            if (sb.length() > 0) sb.append("; ");
            sb.append(e.getKey()).append('=').append(e.getValue());
        }
        return sb.toString();
    }

    private static boolean containsAny(String s, String[] keys) {
        if (s == null || s.isEmpty()) return false;
        for (String k : keys) {
            if (s.contains(k)) return true;
        }
        return false;
    }

    /** 官网首页没有登录表单时，自动点「登录」入口；已在登录页则不动。 */
    private void injectLoginHelper(String src) {
        if (loginWebView == null) return;
        String js =
                "(function(){"
                + "var href=location.href||'';"
                + "if(/ptlogin2|openplat-user\\.kugou|open\\.weixin\\.qq|passport\\/sso\\/login|\\/m\\/login/.test(href)) return;"
                + "var b=(document.body&&document.body.innerText)||'';"
                + "if(/手机号|验证码|密码登录|微信登录|QQ登录/.test(b)) return;"
                + "function hit(n){var x=(n.innerText||n.textContent||'').replace(/\\s+/g,'').trim();"
                + "return x==='登录'||x==='立即登录'||x==='登录/注册';}"
                + "var ns=document.querySelectorAll('a,button,span,div');"
                + "for(var i=0;i<ns.length;i++){if(hit(ns[i])&&ns[i].offsetParent){ns[i].click();return;}}"
                + "})();";
        loginWebView.evaluateJavascript(js, null);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    // ---------------- C++ 传输 / 存取 ----------------

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
                resp.put("url", conn.getURL().toString());
            } catch (Exception e) {
                try {
                    resp.put("s", 0);
                    resp.put("b64", android.util.Base64.encodeToString(
                            String.valueOf(e.getMessage()).getBytes(StandardCharsets.UTF_8),
                            android.util.Base64.NO_WRAP));
                    resp.put("ct", "text/plain");
                    resp.put("sc", new JSONArray());
                    resp.put("url", url);
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
