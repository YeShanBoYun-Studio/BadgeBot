// main/app_portal.c —— 配网门户:内嵌配置页 + 配置/Wi-Fi/上传 API。
//
// 页面是手机浏览器打开的 http://192.168.4.1;接口:
//   GET  /api/config  当前配置(不含 Wi-Fi 密码)
//   POST /api/config  保存名片资料(name/org/title/hide/qr_a/gh)
//   POST /api/wifi    保存 STA 凭据(写入 NVS 并延长热点窗口,让用户继续上传)
//   POST /api/avatar  头像(96x96 RGB565 原始字节,body 为 0 清除)
//   POST /api/qrimg   二维码图(128x128 1bpp,body 为 0 清除)
//   POST /api/done    用户点「完成并联网」:数秒后关热点并立即联网校时/拉热力图
// 上传图片由页面 JS 居中裁成正方形后转原始像素,固件不做图片解码。
#include "app_portal.h"
#include "app_config.h"
#include "app_store.h"
#include "app_wifi.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_portal";

/* 内嵌配置页(默认中文,页面右上角可切 English)。为省转义,HTML/JS 里统一用单引号。
 * 注意:input 的 id 不能叫 name/title——那会撞上 window.name/window.title 内置属性,
 * 导致取值拿到 undefined;统一用 fid() 显式取值。
 * 每个可见标签都有 id:JS 端按 D 字典做中/英切换;所有请求都有成功(绿)/失败(红)回显。 */
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>BadgeBot</title><style>"
"body{font-family:system-ui;background:#0f141b;color:#e9eef4;max-width:420px;margin:0 auto;padding:12px}"
"h2{margin:8px 0}fieldset{border:1px solid #2a323c;border-radius:8px;margin:12px 0}"
"legend{color:#ffd928;padding:0 6px}"
"label{display:block;color:#93a3b0;font-size:13px;margin-top:8px}"
"input{width:100%;box-sizing:border-box;padding:8px;margin:4px 0;background:#11161d;"
"color:#e9eef4;border:1px solid #2a323c;border-radius:6px}"
"button{padding:10px 18px;background:#ffd928;color:#111;border:0;border-radius:6px;font-weight:700}"
"button.minor{background:#2a323c;color:#e9eef4;font-weight:400}"
"button.done{width:100%;margin-top:12px;background:#2ea043;color:#fff}"
"span{margin-left:8px;font-size:14px;color:#7ce38b}"
".chk{display:flex;align-items:center;gap:8px;margin:8px 0}.chk input{width:auto}"
".top{display:flex;justify-content:space-between;align-items:center}"
".hint{color:#93a3b0;font-size:13px;margin-top:14px}"
"</style></head><body>"
"<div class='top'><h2>BadgeBot</h2><button id='btnlang' class='minor' onclick='toggleLang()'>EN</button></div>"
"<fieldset><legend id='l_wifi'>Wi-Fi(仅支持 2.4GHz)</legend>"
"<label id='l_ssid'>SSID</label><input id='ssid'>"
"<label id='l_pass'>密码</label><input id='pass' type='password'>"
"<button id='b_savewifi' onclick='saveWifi()'>保存 Wi-Fi</button><span id='wm'></span>"
"</fieldset>"
"<fieldset><legend id='l_card'>名片</legend>"
"<label id='l_name'>姓名</label><input id='bname'>"
"<label id='l_org'>公司</label><input id='borg'>"
"<label id='l_title'>岗位</label><input id='btitle'>"
"<div class='chk'><input id='bhide' type='checkbox'><label id='l_hide' for='bhide'>隐藏公司/岗位</label></div>"
"<label id='l_qra'>二维码内容(链接或文本)</label><input id='bqr'>"
"<button id='b_savecard' onclick='saveCfg()'>保存名片</button><span id='cm'></span>"
"</fieldset>"
"<fieldset><legend id='l_img'>图片(上传即居中裁成正方形)</legend>"
"<label id='l_avatar'>头像(96x96 居中裁切)</label><input type='file' id='fav' accept='image/*'>"
"<button id='b_upav' onclick=\"up('/api/avatar',fid('fav').files[0],96,'rgb565',fid('am'))\">上传头像</button>"
"<button id='b_clr1' class='minor' onclick=\"clr('/api/avatar',fid('am'))\">清除</button><span id='am'></span>"
"<label id='l_qrimg'>二维码图(微信等,128x128 黑白)</label><input type='file' id='fqr' accept='image/*'>"
"<button id='b_upqr' onclick=\"up('/api/qrimg',fid('fqr').files[0],128,'bw',fid('qm'))\">上传二维码图</button>"
"<button id='b_clr2' class='minor' onclick=\"clr('/api/qrimg',fid('qm'))\">清除</button><span id='qm'></span>"
"</fieldset>"
"<fieldset><legend id='l_gh'>GitHub 热力图</legend>"
"<label id='l_user'>用户名</label><input id='bgh'>"
"<button id='b_savegh' onclick='saveGh()'>保存</button><span id='gm'></span>"
"</fieldset>"
"<button id='b_done' class='done' onclick='finish()'>完成并联网</button><span id='dm'></span>"
"<p id='hint' class='hint'>保存 Wi-Fi 后热点保持开启;完成所有修改后点「完成并联网」,工牌会关闭热点并立即联网校时、拉取热力图。</p>"
"<script>"
"var LANG=0;"
"var D={"
"zh:{wifi:'Wi-Fi(仅支持 2.4GHz)',ssid:'SSID',pass:'密码',savewifi:'保存 Wi-Fi',"
"card:'名片',name:'姓名',org:'公司',title:'岗位',hide:'隐藏公司/岗位',"
"qra:'二维码内容(链接或文本)',savecard:'保存名片',"
"img:'图片(上传即居中裁成正方形)',avatar:'头像(96x96 居中裁切)',upav:'上传头像',clr:'清除',"
"qrimg:'二维码图(微信等,128x128 黑白)',upqr:'上传二维码图',"
"gh:'GitHub 热力图',user:'用户名',savegh:'保存',"
"done:'完成并联网',pick:'请先选择图片',dec:'图片解码失败,请换 JPG/PNG 试',"
"proc:'处理中…',net:'网络错误(热点可能已关闭)',"
"hint:'保存 Wi-Fi 后热点保持开启;完成所有修改后点「完成并联网」,工牌会关闭热点并立即联网校时、拉取热力图。'},"
"en:{wifi:'Wi-Fi (2.4GHz only)',ssid:'SSID',pass:'Password',savewifi:'Save Wi-Fi',"
"card:'Card',name:'Name',org:'Company',title:'Title',hide:'Hide company/title',"
"qra:'QR content (link or text)',savecard:'Save card',"
"img:'Images (center-cropped to square)',avatar:'Avatar (96x96 center-crop)',upav:'Upload avatar',clr:'Clear',"
"qrimg:'QR image (WeChat etc., 128x128 B/W)',upqr:'Upload QR image',"
"gh:'GitHub heatmap',user:'Username',savegh:'Save',"
"done:'Finish && connect',pick:'Pick an image first',dec:'Decode failed, try JPG/PNG',"
"proc:'Working…',net:'Network error (hotspot may be closed)',"
"hint:'The hotspot stays on after saving Wi-Fi. When everything is done, press Finish to close it and go online.'}"
"};"
"function fid(id){return document.getElementById(id)}"
"function T(k){return (LANG?D.en:D.zh)[k]}"
"function msg(m,t,ok){m.textContent=t;m.style.color=ok?'#7ce38b':'#ff6b6b'}"
"function applyLang(){"
"fid('btnlang').textContent=LANG?'中文':'EN';"
"fid('l_wifi').textContent=T('wifi');fid('l_ssid').textContent=T('ssid');"
"fid('l_pass').textContent=T('pass');fid('b_savewifi').textContent=T('savewifi');"
"fid('l_card').textContent=T('card');fid('l_name').textContent=T('name');"
"fid('l_org').textContent=T('org');fid('l_title').textContent=T('title');"
"fid('l_hide').textContent=T('hide');fid('l_qra').textContent=T('qra');"
"fid('b_savecard').textContent=T('savecard');"
"fid('l_img').textContent=T('img');fid('l_avatar').textContent=T('avatar');"
"fid('b_upav').textContent=T('upav');fid('b_clr1').textContent=T('clr');"
"fid('l_qrimg').textContent=T('qrimg');fid('b_upqr').textContent=T('upqr');"
"fid('b_clr2').textContent=T('clr');"
"fid('l_gh').textContent=T('gh');fid('l_user').textContent=T('user');"
"fid('b_savegh').textContent=T('savegh');fid('b_done').textContent=T('done');"
"fid('hint').textContent=T('hint')}"
"function toggleLang(){LANG=1-LANG;applyLang()}"
"function sv(u,b,m){msg(m,T('proc'),true);"
"fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(b)}).then(function(r){return r.text()})"
".then(function(t){msg(m,t,true)}).catch(function(e){msg(m,T('net'),false)})}"
"function saveWifi(){sv('/api/wifi',{ssid:fid('ssid').value,pass:fid('pass').value},fid('wm'))}"
"function saveCfg(){sv('/api/config',{name:fid('bname').value,org:fid('borg').value,"
"title:fid('btitle').value,hide:fid('bhide').checked,qr_a:fid('bqr').value},fid('cm'))}"
"function saveGh(){sv('/api/config',{gh:fid('bgh').value},fid('gm'))}"
"function finish(){sv('/api/done',{},fid('dm'))}"
"function proc(file,size,mode,ok,fail){var img=new Image();"
"img.onload=function(){var s=Math.min(img.width,img.height);"
"var cv=document.createElement('canvas');cv.width=size;cv.height=size;"
"var cx=cv.getContext('2d');"
"cx.drawImage(img,(img.width-s)/2,(img.height-s)/2,s,s,0,0,size,size);"
"var d=cx.getImageData(0,0,size,size).data;var body;"
"if(mode=='rgb565'){body=new Uint8Array(size*size*2);"
"for(var i=0;i<size*size;i++){var v=((d[i*4]>>3)<<11)|((d[i*4+1]>>2)<<5)|(d[i*4+2]>>3);"
"body[i*2]=v&255;body[i*2+1]=v>>8;}}"
"else{body=new Uint8Array(size*size>>3);"
"for(var i=0;i<size*size;i++){var lum=(d[i*4]*3+d[i*4+1]*4+d[i*4+2])>>3;"
"if(lum>127)body[i>>3]|=1<<(i&7);}}"
"ok(body)};"
"img.onerror=function(){fail()};"
"img.src=URL.createObjectURL(file)}"
"function up(u,f,size,mode,m){if(!f){msg(m,T('pick'),false);return}"
"msg(m,T('uping'),true);"
"proc(f,size,mode,function(body){fetch(u,{method:'POST',"
"headers:{'Content-Type':'application/octet-stream'},body:body})"
".then(function(r){return r.text()}).then(function(t){msg(m,t,true)})"
".catch(function(e){msg(m,T('net'),false)})},"
"function(){msg(m,T('dec'),false)})}"
"function clr(u,m){msg(m,T('proc'),true);"
"fetch(u,{method:'POST'}).then(function(r){return r.text()})"
".then(function(t){msg(m,t,true)}).catch(function(e){msg(m,T('net'),false)})}"
"fetch('/api/config').then(function(r){return r.json()}).then(function(c){"
"fid('ssid').value=c.sta_ssid||'';fid('bname').value=c.name||'';"
"fid('borg').value=c.org||'';fid('btitle').value=c.title||'';"
"fid('bhide').checked=!!c.hide;fid('bqr').value=c.qr_a||'';fid('bgh').value=c.gh||'';"
"LANG=c.lang?1:0;applyLang()}).catch(function(){applyLang()});"
"</script></body></html>";

static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_config(httpd_req_t *req)
{
    const app_config_t *c = app_config_get();
    wifi_config_t sta = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &sta);
    char ssid[(sizeof sta.sta.ssid) + 1] = { 0 };
    memcpy(ssid, sta.sta.ssid, sizeof(sta.sta.ssid));   // SSID 不保证 NUL 结尾

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", c->name);
    cJSON_AddStringToObject(j, "org", c->org);
    cJSON_AddStringToObject(j, "title", c->title);
    cJSON_AddStringToObject(j, "qr_a", c->qr_a);
    cJSON_AddStringToObject(j, "gh", c->gh_user);
    cJSON_AddBoolToObject(j, "hide", c->hide_org_title);
    cJSON_AddNumberToObject(j, "layout", c->layout);
    cJSON_AddNumberToObject(j, "theme", c->theme);
    cJSON_AddNumberToObject(j, "lang", c->lang);
    cJSON_AddStringToObject(j, "sta_ssid", ssid);
    const char *body = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)body);
    cJSON_Delete(j);
    return ESP_OK;
}

// 读 POST body(上限 1KB,足够本门户的所有表单)
static char *read_body(httpd_req_t *req)
{
    static char buf[1024];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) return NULL;
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) return NULL;
        got += n;
    }
    buf[total] = '\0';
    return buf;
}

static esp_err_t post_config(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_text(req, "bad request");
    cJSON *j = cJSON_Parse(body);
    if (!j) return send_text(req, "bad json");

    app_config_t c = *app_config_get();
    cJSON *item;
    if ((item = cJSON_GetObjectItem(j, "name"))  && cJSON_IsString(item))
        strlcpy(c.name, item->valuestring, sizeof(c.name));
    if ((item = cJSON_GetObjectItem(j, "org"))   && cJSON_IsString(item))
        strlcpy(c.org, item->valuestring, sizeof(c.org));
    if ((item = cJSON_GetObjectItem(j, "title")) && cJSON_IsString(item))
        strlcpy(c.title, item->valuestring, sizeof(c.title));
    if ((item = cJSON_GetObjectItem(j, "qr_a"))  && cJSON_IsString(item))
        strlcpy(c.qr_a, item->valuestring, sizeof(c.qr_a));
    if ((item = cJSON_GetObjectItem(j, "gh"))    && cJSON_IsString(item))
        strlcpy(c.gh_user, item->valuestring, sizeof(c.gh_user));
    if ((item = cJSON_GetObjectItem(j, "hide"))  && cJSON_IsBool(item))
        c.hide_org_title = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(j, "lang"))  && cJSON_IsNumber(item))
        c.lang = (uint8_t)(item->valueint != 0);
    cJSON_Delete(j);

    app_config_update(&c);
    ESP_LOGI(TAG, "门户保存名片资料");
    return send_text(req, "已保存 / Saved");
}

static esp_err_t post_wifi(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_text(req, "bad request");
    cJSON *j = cJSON_Parse(body);
    if (!j) return send_text(req, "bad json");

    cJSON *sid = cJSON_GetObjectItem(j, "ssid");
    cJSON *pwd = cJSON_GetObjectItem(j, "pass");
    if (!cJSON_IsString(sid) || sid->valuestring[0] == '\0') {
        cJSON_Delete(j);
        return send_text(req, "SSID 不能为空 / SSID required");
    }
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, sid->valuestring, sizeof(sta.sta.ssid));
    if (cJSON_IsString(pwd)) {
        strlcpy((char *)sta.sta.password, pwd->valuestring, sizeof(sta.sta.password));
    }
    cJSON_Delete(j);

    // 存储模式是 FLASH:这里写入即持久化到 NVS,之后每次脉冲零配置连接。
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 凭据保存失败: %s", esp_err_to_name(e));
        return send_text(req, "保存失败 / Save failed");
    }
    ESP_LOGI(TAG, "门户保存 Wi-Fi: %s", sta.sta.ssid);
    // 保存后热点保持开启,让用户继续上传图片/保存资料;点「完成并联网」或超时才关
    app_wifi_portal_extend(120000);
    return send_text(req, "已保存,热点保持开启 / Saved, hotspot stays on");
}

static esp_err_t post_done(httpd_req_t *req)
{
    ESP_LOGI(TAG, "门户完成配置,关闭热点");
    app_portal_notify_saved();   // 数秒后配网窗口自动关闭,随后立刻脉冲联网
    return send_text(req, "再见,热点即将关闭 / Bye, closing hotspot");
}

// 流式接收 body 直写 /store 文件;content_len 为 0 表示删除该资产。
// 头像 18KB、二维码图 2KB 都超出通用读缓冲,不能一次 recv。
static esp_err_t upload_asset(httpd_req_t *req, const char *name, long expected_len)
{
    if (!app_store_ready()) {
        ESP_LOGW(TAG, "上传 %s 被拒:存储不可用", name);
        return send_text(req, "存储不可用 / Storage unavailable");
    }

    if (req->content_len == 0) {
        app_store_write(name, NULL, 0);
        return send_text(req, "已清除 / Cleared");
    }
    if (req->content_len != expected_len) {
        ESP_LOGW(TAG, "上传 %s 长度不符: got %ld want %ld",
                 name, (long)req->content_len, expected_len);
        return send_text(req, "数据长度不符 / Bad length");
    }

    char path[48];
    snprintf(path, sizeof(path), "/store/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "上传 %s 打开文件失败", path);
        return send_text(req, "写入失败 / Write failed");
    }

    char buf[512];
    int remaining = req->content_len, failed = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining);
        if (n <= 0) { failed = 1; break; }
        if (fwrite(buf, 1, n, f) != (size_t)n) { failed = 1; break; }
        remaining -= n;
    }
    fclose(f);
    if (failed) {
        ESP_LOGW(TAG, "上传 %s 中断,已回滚(剩 %d 字节)", name, remaining);
        app_store_write(name, NULL, 0);   // 半截文件直接清掉
        return send_text(req, "接收中断,已回滚 / Interrupted, rolled back");
    }
    ESP_LOGI(TAG, "资产 %s 已上传(%ld 字节)", name, expected_len);
    return send_text(req, "已上传 / Uploaded");
}

static esp_err_t post_avatar(httpd_req_t *req)
{
    return upload_asset(req, "avatar.rgb565", 96 * 96 * 2);
}

static esp_err_t post_qrimg(httpd_req_t *req)
{
    return upload_asset(req, "qr_b.img", 128 * 128 / 8);
}

static const httpd_uri_t URIS[] = {
    { .uri = "/",          .method = HTTP_GET,  .handler = get_index  },
    { .uri = "/api/config",.method = HTTP_GET,  .handler = get_config },
    { .uri = "/api/config",.method = HTTP_POST, .handler = post_config},
    { .uri = "/api/wifi",  .method = HTTP_POST, .handler = post_wifi  },
    { .uri = "/api/done",  .method = HTTP_POST, .handler = post_done  },
    { .uri = "/api/avatar",.method = HTTP_POST, .handler = post_avatar},
    { .uri = "/api/qrimg", .method = HTTP_POST, .handler = post_qrimg },
};

static httpd_handle_t s_server;

esp_err_t app_portal_http_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) return e;
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        httpd_register_uri_handler(s_server, &URIS[i]);
    }
    return ESP_OK;
}

void app_portal_http_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
