/**
 * @file http_server.cpp
 * @brief
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include "http_server.h"
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "../app_config.h"
#include "../hal/hal.h"
#include "../net/net_manager.h"
#include "../rtsp/rtsp_server.h"
#include "../onvif/onvif.h"
#include "../apis/camera/api_cam.h"


static AsyncWebServer* _server = nullptr;


// Sends the 401 challenge itself when the request is not allowed
static bool _check_auth(AsyncWebServerRequest* request)
{
    auto& config = HAL::hal::GetHal()->getConfig();
    if (!HAL::hal::GetHal()->isAuthEnabled())
        return true;
    if (request->authenticate(config.auth_user.c_str(), config.auth_password.c_str(), APP_MODEL))
        return true;

    request->requestAuthentication(APP_MODEL, true);
    return false;
}


static void _on_protected(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction handler)
{
    _server->on(uri, method, [handler](AsyncWebServerRequest* request) {
        if (_check_auth(request))
            handler(request);
    });
}


static const char _index_html[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>UnitCamS3</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#f4f4f5;color:#18181b}
main{max-width:720px;margin:0 auto;padding:16px}
h1{font-size:20px}h2{font-size:16px;margin:0 0 8px}
.card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;box-shadow:0 1px 2px #0002}
.warn{background:#fef3c7}
img{width:100%;border-radius:4px;background:#000;min-height:120px;display:block}
code{word-break:break-all;background:#f4f4f5;padding:2px 4px;border-radius:4px}
.row{margin:6px 0}
label{display:block;margin:10px 0 4px;font-size:14px}
input,select{width:100%;padding:8px;box-sizing:border-box;border:1px solid #d4d4d8;border-radius:4px;font-size:15px}
input[type=checkbox]{width:auto;margin-right:6px}
button{margin-top:16px;padding:10px 16px;border:0;border-radius:4px;background:#2563eb;color:#fff;font-size:15px}
</style></head><body><main>
<h1>UnitCamS3 &middot; RTSP / ONVIF</h1>
<div class="card warn" id="setup" hidden>Setup mode: enter your Wi-Fi below. After the reboot the camera joins that network.</div>
<div class="card">
<img id="live" alt="">
<button type="button" id="liveBtn" onclick="toggleLive()">Start preview</button>
</div>
<div class="card">
<h2>Stream</h2>
<div class="row">RTSP: <code id="rtspUrl"></code></div>
<div class="row">ONVIF: <code id="onvifUrl"></code></div>
<div class="row">Snapshot: <code id="snapshotUrl"></code></div>
<div class="row">MJPEG: <code id="mjpegUrl"></code></div>
<div class="row" id="status"></div>
</div>
<form class="card" method="post" action="/api/v1/config">
<h2>Wi-Fi</h2>
<label>SSID</label><input name="wifiSsid" id="wifiSsid" maxlength="32">
<label>Password (leave empty to keep the current one)</label><input name="wifiPass" type="password" maxlength="64">
<label>Hostname</label><input name="hostname" id="hostname" pattern="[A-Za-z0-9\-]{1,31}" required>
<h2 style="margin-top:20px">Camera</h2>
<label>Resolution</label>
<select name="frameSize" id="frameSize">
<option value="QVGA">QVGA 320x240</option><option value="CIF">CIF 400x296</option>
<option value="HVGA">HVGA 480x320</option><option value="VGA">VGA 640x480</option>
<option value="SVGA">SVGA 800x600</option><option value="XGA">XGA 1024x768</option>
<option value="HD">HD 1280x720</option><option value="SXGA">SXGA 1280x1024</option>
<option value="UXGA">UXGA 1600x1200</option>
</select>
<label>JPEG quality (6 = best, 63 = smallest)</label><input name="jpegQuality" id="jpegQuality" type="number" min="6" max="63">
<label>RTSP max fps</label><input name="fps" id="fps" type="number" min="1" max="30">
<label><input type="checkbox" name="vflip" id="vflip">Vertical flip</label>
<label><input type="checkbox" name="hmirror" id="hmirror">Horizontal mirror</label>
<h2 style="margin-top:20px">Access</h2>
<div class="row" id="authState"></div>
<label>Username (web page, RTSP, ONVIF)</label><input name="authUser" id="authUser" maxlength="32" pattern="[A-Za-z0-9_.\-]{1,32}" required>
<label>Password (leave empty to keep the current one)</label><input name="authPass" type="password" maxlength="64" autocomplete="new-password">
<label><input type="checkbox" name="authDisable" id="authDisable">No password, open access</label>
<button>Save and reboot</button>
</form>
</main>
<script>
let live=false;
function toggleLive(){
  live=!live;
  document.getElementById('live').src=live?'/api/v1/stream':'data:,';
  document.getElementById('liveBtn').textContent=live?'Stop preview':'Start preview';
}
fetch('/api/v1/info').then(r=>r.json()).then(d=>{
  for(const k of ['rtspUrl','onvifUrl','snapshotUrl','mjpegUrl'])document.getElementById(k).textContent=d[k];
  for(const k of ['wifiSsid','hostname','frameSize','jpegQuality','fps','authUser'])document.getElementById(k).value=d[k];
  for(const k of ['vflip','hmirror'])document.getElementById(k).checked=d[k];
  document.getElementById('authDisable').checked=!d.authEnabled;
  document.getElementById('authState').textContent=d.authEnabled?'Password protected':'Open access: anyone on the network can watch and change settings';
  document.getElementById('setup').hidden=!d.setupMode;
  document.getElementById('status').textContent='RTSP clients: '+d.rtspClients+' | IP: '+d.ip+' | uptime: '+d.uptime+' s';
});
</script>
</body></html>
)rawliteral";


static void _handle_index(AsyncWebServerRequest* request)
{
    request->send_P(200, "text/html", _index_html);
}


static void _handle_info(AsyncWebServerRequest* request)
{
    auto& config = HAL::hal::GetHal()->getConfig();

    DynamicJsonDocument doc(1536);
    doc["ip"] = NetManager::get_local_ip().toString();
    doc["mac"] = WiFi.macAddress();
    doc["setupMode"] = NetManager::is_setup_mode();
    doc["rtspUrl"] = NetManager::get_rtsp_url();
    doc["onvifUrl"] = NetManager::get_onvif_url();
    doc["snapshotUrl"] = NetManager::get_snapshot_url();
    doc["mjpegUrl"] = NetManager::get_mjpeg_url();
    doc["rtspClients"] = RtspServer::get_client_count();
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["freePsram"] = ESP.getFreePsram();
    doc["wifiSsid"] = config.wifi_ssid;
    doc["hostname"] = config.hostname;
    doc["frameSize"] = config.frame_size;
    doc["jpegQuality"] = config.jpeg_quality;
    doc["fps"] = config.fps;
    doc["vflip"] = config.vflip;
    doc["hmirror"] = config.hmirror;
    doc["authUser"] = config.auth_user;
    doc["authEnabled"] = HAL::hal::GetHal()->isAuthEnabled();

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}


static bool _is_valid_name(const String& name, size_t max_len, const char* extra_chars)
{
    if (name.length() == 0 || name.length() > max_len)
        return false;
    for (size_t i = 0; i < name.length(); i++)
    {
        char c = name[i];
        if (!isalnum((unsigned char)c) && strchr(extra_chars, c) == nullptr)
            return false;
    }
    return true;
}


static void _handle_set_config(AsyncWebServerRequest* request)
{
    auto& config = HAL::hal::GetHal()->getConfig();

    if (request->hasParam("wifiSsid", true))
    {
        String ssid = request->getParam("wifiSsid", true)->value();
        String pass = request->hasParam("wifiPass", true) ? request->getParam("wifiPass", true)->value() : "";
        // Empty password keeps the old one, unless the network changed (could be an open one)
        if (pass != "" || ssid != config.wifi_ssid)
            config.wifi_password = pass;
        config.wifi_ssid = ssid;
    }
    if (request->hasParam("hostname", true))
    {
        String hostname = request->getParam("hostname", true)->value();
        if (_is_valid_name(hostname, 31, "-"))
            config.hostname = hostname;
    }
    if (request->hasParam("frameSize", true))
    {
        String frame_size = request->getParam("frameSize", true)->value();
        if (HAL::hal::isValidFrameSize(frame_size))
            config.frame_size = frame_size;
    }
    if (request->hasParam("jpegQuality", true))
        config.jpeg_quality = constrain(request->getParam("jpegQuality", true)->value().toInt(), 6, 63);
    if (request->hasParam("fps", true))
        config.fps = constrain(request->getParam("fps", true)->value().toInt(), 1, 30);
    // Unchecked boxes are not posted
    config.vflip = request->hasParam("vflip", true);
    config.hmirror = request->hasParam("hmirror", true);

    // Username ends up in digest headers and xml, keep it simple
    if (request->hasParam("authUser", true))
    {
        String user = request->getParam("authUser", true)->value();
        if (_is_valid_name(user, 32, "_.-"))
            config.auth_user = user;
    }
    // A typed password wins over the "open access" box, an empty one keeps the current password
    String auth_pass = request->hasParam("authPass", true) ? request->getParam("authPass", true)->value() : "";
    if (auth_pass != "")
        config.auth_password = auth_pass.substring(0, 64);
    else if (request->hasParam("authDisable", true))
        config.auth_password = "";

    if (!HAL::hal::GetHal()->saveConfig())
    {
        request->send(500, "text/plain", "save config failed");
        return;
    }
    HAL::hal::GetHal()->printConfig();

    String html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<meta http-equiv=\"refresh\" content=\"15;url=/\"></head><body style=\"font-family:system-ui,sans-serif;padding:16px\">"
                  "<p>Saved, rebooting...</p><p>If the Wi-Fi changed, find the camera at <a href=\"http://";
    html += config.hostname;
    html += ".local/\">http://";
    html += config.hostname;
    html += ".local/</a> or look up its IP in your router.</p></body></html>";
    request->send(200, "text/html", html);

    HAL::hal::GetHal()->requestReboot();
}


void HttpServer::start()
{
    _server = new AsyncWebServer(APP_HTTP_PORT);

    // Camera apis from the user demo (load_cam_apis), behind the password
    _on_protected("/api/v1/bmp", HTTP_GET, sendBMP);
    _on_protected("/api/v1/capture", HTTP_GET, sendJpg);
    _on_protected("/api/v1/stream", HTTP_GET, streamJpg);
    _on_protected("/api/v1/control", HTTP_GET, setCameraVar);
    _on_protected("/api/v1/status", HTTP_GET, getCameraStatus);

    _on_protected("/", HTTP_GET, _handle_index);
    _on_protected("/api/v1/info", HTTP_GET, _handle_info);
    _on_protected("/api/v1/config", HTTP_POST, _handle_set_config);

    // Soap endpoints check WS-Security / http digest themselves
    Onvif::load_soap_apis(*_server);

    _server->onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "text/plain", "not found");
    });

    _server->begin();
    log_i("http server started");
}
