/* MeloGlass APP 端上 API 薄层。
 * 业务逻辑全部在 C++（meloglass.cpp，经 AndroidBridge.api 进出）。
 * 这里只做：JS 调用桥接 + 网易云二维码本地渲染（qrcode-generator）。
 * 若在桌面浏览器直接打开（无 AndroidBridge），回落到本地 Python 代理 127.0.0.1:8787。 */
'use strict';

const HAS_BRIDGE = typeof window.AndroidBridge !== 'undefined' && window.AndroidBridge && window.AndroidBridge.api;
const API = (location.port === '8787' ? '' : 'http://127.0.0.1:8787'); // fallback only

function _bridgeCall(method, path, body) {
  const raw = window.AndroidBridge.api(method, path, body || '');
  return JSON.parse(raw);
}

function _wyQrSvg(url) {
  try {
    const qr = qrcode(0, 'M');
    qr.addData(url);
    qr.make();
    return 'data:image/svg+xml;utf8,' + encodeURIComponent(qr.createSvgTag(4));
  } catch (e) {
    return '';
  }
}

function _postProcess(path, data) {
  // 网易云：C++ 返回 url，本地生成二维码 SVG（不经过第三方）
  if (path.indexOf('/api/login/qr') === 0 && data && data.mode === 'qr' && data.url && !data.qrimg) {
    const svg = _wyQrSvg(data.url);
    if (svg) data.qrimg = svg;
  }
  return data;
}

async function api(path) {
  if (HAS_BRIDGE) {
    const j = _bridgeCall('GET', path, '');
    if (j && j.data) _postProcess(path, j.data);
    return j;
  }
  const r = await fetch(API + path);
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}

async function apiPost(path, body) {
  if (HAS_BRIDGE) {
    return _bridgeCall('POST', path, JSON.stringify(body || {}));
  }
  const r = await fetch(API + path, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body || {})
  });
  return r.json();
}
