// main/app_portal.c —— 配网门户:内嵌配置页 + 配置/Wi-Fi/上传 API。
//
// 页面是手机浏览器打开的 http://192.168.4.1;接口:
//   GET  /api/config  当前配置(不含 Wi-Fi 密码;附带热点剩余毫秒数,页面心跳用)
//   POST /api/config  保存名片资料(name/org/title/hide/qr_text[4]/qr_label[4]/qr_mode/lang)
//   POST /api/wifi    保存 STA 凭据(写入 NVS 并延长热点窗口,让用户继续上传)
//   POST /api/avatar  头像(96x96 RGB565 原始字节,body 为 0 清除)
//   POST /api/qr0..3  二维码槽 0..3 的上传图(128x128 1bpp,body 为 0 清除);
//                     每槽内容来源由 qr_mode 位图选择:bit n = 1 用上传图,0 用网页生成文本
//   GET/POST /api/notes 提词稿(notes.txt,UTF-8 纯文本,一行 = 一段,上限 32KB);
//                     页面支持粘贴 / .txt 读入 / .pptx 备注提取(浏览器端解包,零依赖)
//   POST /api/done    用户点"完成并联网":数秒后关热点并立即联网校时
// 图片由页面 JS 裁剪:默认居中正方形选区,可拖动/缩放;固件不做图片解码。
// 注意:store 分区曾因 FATFS 长文件名未启用(CONFIG_FATFS_LFN_NONE)导致
// fopen("avatar.rgb565") 失败(8.3 短名放不下 6 字符扩展名),页面表现为
// "写入失败";而 qr_b.img 恰好合规所以能传成功。现已在 sdkconfig 打开 LFN。
#include "app_portal.h"
#include "app_config.h"
#include "app_store.h"
#include "app_wifi.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_portal";

/* 内嵌配置页(默认中文,页面右上角可切 English)。为省转义,HTML/JS 里统一用单引号,
 * C 字符串内不出现 ASCII 双引号(引号用「」或全角)。
 * 注意:input 的 id 不能叫 name/title——那会撞上 window.name/window.title 内置属性。
 * 每个可见标签都有 id:JS 端按 D 字典做中/英切换;所有请求都有成功(绿)/失败(红)回显。 */
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>BadgeBot</title><style>"
"body{font-family:system-ui;background:#0f141b;color:#e9eef4;max-width:420px;margin:0 auto;padding:12px}"
"h2{margin:8px 0}fieldset{border:1px solid #2a323c;border-radius:8px;margin:12px 0}"
"legend{color:#ffd928;padding:0 6px}"
"label{display:block;color:#93a3b0;font-size:13px;margin-top:8px}"
"input,textarea{width:100%;box-sizing:border-box;padding:8px;margin:4px 0;background:#11161d;"
"color:#e9eef4;border:1px solid #2a323c;border-radius:6px}"
"textarea{font:14px/1.5 system-ui;resize:vertical}"
"button{padding:10px 18px;background:#ffd928;color:#111;border:0;border-radius:6px;font-weight:700}"
"button.minor{background:#2a323c;color:#e9eef4;font-weight:400}"
"button.done{width:100%;margin-top:12px;background:#2ea043;color:#fff}"
"span{margin-left:8px;font-size:14px;color:#7ce38b}"
".chk{display:flex;align-items:center;gap:8px;margin:8px 0}.chk input{width:auto}"
".slott{margin-top:12px;padding-top:6px;border-top:1px dashed #2a323c;"
"font-weight:700;font-size:13px}"
".top{display:flex;justify-content:space-between;align-items:center}"
".hint{color:#93a3b0;font-size:13px;margin-top:14px}"
"#beat{font-size:13px;margin-top:10px;color:#7ce38b}"
"#cwrap{position:relative;font-size:0;line-height:0;margin-top:6px}"
"#cimg{max-width:280px;max-height:280px}"
"#cbox{position:absolute;border:2px dashed #ffd928;touch-action:none;box-sizing:border-box}"
"#chandle{position:absolute;right:-9px;bottom:-9px;width:18px;height:18px;"
"background:#ffd928;border-radius:3px;touch-action:none}"
"</style></head><body>"
"<div class='top'><h2>BadgeBot</h2><button id='btnlang' class='minor' onclick='toggleLang()'>EN</button></div>"
"<p id='beat'></p>"
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
"<button id='b_savecard' onclick='saveCfg()'>保存名片</button><span id='cm'></span>"
"</fieldset>"
"<fieldset><legend id='l_qrs'>二维码(4 槽,每槽可选 网页生成 或 上传图片)</legend>"
"<div id='qrs'></div>"
"<button id='b_saveqr' onclick='saveQr()'>保存二维码设置</button><span id='qrm'></span>"
"</fieldset>"
"<fieldset><legend id='l_notes'>提词稿(一行 = 一段/PPT 一页)</legend>"
"<textarea id='notes' rows=5></textarea><div class=slott id='notesn'></div>"
"<label id='l_ntxt'>或上传 .txt(每行一页)</label><input type=file id='fnt' accept=.txt>"
"<button id='b_ntxt' class=minor>读入 txt</button>"
"<label id='l_npx'>或上传 .pptx 自动提取每页备注</label><input type=file id='fpx' accept=.pptx>"
"<button id='b_npx' class=minor>提取备注</button>"
"<button id='b_ns'>保存提词稿</button>"
"<button id='b_nc' class='minor'>清除</button><span id='nm'></span>"
"</fieldset>"
"<fieldset><legend id='l_img'>图片(手动裁剪为正方形)</legend>"
"<label id='l_avatar'>头像(输出 96x96)</label><input type='file' id='fav' accept='image/*'>"
"<button id='b_upav' onclick=\"openCrop(fid('fav').files[0],'/api/avatar',96,'rgb565',fid('am'))\">裁剪并上传头像</button>"
"<button id='b_clr1' class='minor' onclick=\"clr('/api/avatar',fid('am'))\">清除</button><span id='am'></span>"
"<div id='crop' style='display:none;margin-top:10px'>"
"<label id='l_crop'>拖动虚线框选裁剪区;右下角手柄缩放(默认居中)</label>"
"<div id='cwrap'><img id='cimg'><div id='cbox'>"
"<div id='chandle'></div></div></div>"
"<button id='b_cropok' onclick='finishCrop()'>确认上传</button>"
"<button id='b_cancel' class='minor' onclick='cancelCrop()'>取消</button>"
"</div>"
"</fieldset>"
"<button id='b_done' class='done' onclick='finish()'>完成并联网</button><span id='dm'></span>"
"<p id='hint' class='hint'>保存 Wi-Fi 后热点保持开启;完成所有修改后点「完成并联网」,工牌会关闭热点并立即联网校时。</p>"
"<script>"
"var LANG=0;"
"var D={"
"zh:{wifi:'Wi-Fi(仅支持 2.4GHz)',ssid:'SSID',pass:'密码',savewifi:'保存 Wi-Fi',"
"card:'名片',name:'姓名',org:'公司',title:'岗位',hide:'隐藏公司/岗位',savecard:'保存名片',"
"qrs:'二维码(4 槽,每槽可选 网页生成 或 上传图片)',ql:'槽位说明(如:微信)',"
"qg:'网页生成',qu:'上传图片',qph:'链接或文本',upq:'裁剪并上传',saveqr:'保存二维码设置',"
"notes:'提词稿(一行 = 一段/PPT 一页)',ntxt:'或上传 .txt(每行一页)',"
"npx:'或上传 .pptx 自动提取每页备注',rdtxt:'读入 txt',expx:'提取备注',"
"saven:'保存提词稿',nseg:'段',nempty:'内容为空',nbig:'超过 32KB 上限',"
"perr:'pptx 解析失败(需 .pptx;老 .ppt 请先另存为 .pptx)',pnone:'未找到备注页',"
"pbrowser:'浏览器太旧,不支持解压',"
"img:'图片(手动裁剪为正方形)',avatar:'头像(输出 96x96)',upav:'裁剪并上传头像',clr:'清除',"
"crop:'拖动虚线框选裁剪区;右下角手柄缩放(默认居中)',ok:'确认上传',cancel:'取消',"
"done:'完成并联网',pick:'请先选择图片',dec:'图片解码失败,请换 JPG/PNG 试',"
"proc:'处理中…',uping:'上传中…',net:'网络错误(热点可能已关闭)',"
"alive:'热点在线 · 剩余约 ',unit:' 秒',dead:'设备不可达:热点可能已关闭,请在工牌重新进入配网后刷新本页',"
"hint:'保存 Wi-Fi 后热点保持开启;完成所有修改后点「完成并联网」,工牌会关闭热点并立即联网校时。'},"
"en:{wifi:'Wi-Fi (2.4GHz only)',ssid:'SSID',pass:'Password',savewifi:'Save Wi-Fi',"
"card:'Card',name:'Name',org:'Company',title:'Title',hide:'Hide company/title',savecard:'Save card',"
"qrs:'QR codes (4 slots; each: generated or uploaded image)',ql:'Caption (e.g. WeChat)',"
"qg:'Generated',qu:'Uploaded image',qph:'Link or text',upq:'Crop && upload',saveqr:'Save QR setup',"
"notes:'Notes (one line = one segment/slide)',ntxt:'or upload .txt (one line per slide)',"
"npx:'or upload .pptx to extract speaker notes',rdtxt:'Load txt',expx:'Extract notes',"
"saven:'Save notes',nseg:'segments',nempty:'empty',nbig:'over the 32KB limit',"
"perr:'pptx parse failed (needs .pptx; re-save legacy .ppt first)',pnone:'no speaker notes found',"
"pbrowser:'browser too old to inflate',"
"img:'Images (manual square crop)',avatar:'Avatar (96x96 out)',upav:'Crop && upload avatar',clr:'Clear',"
"crop:'Drag the dashed box to crop; corner handle resizes (centered by default)',ok:'Upload',cancel:'Cancel',"
"done:'Finish && connect',pick:'Pick an image first',dec:'Decode failed, try JPG/PNG',"
"proc:'Working…',uping:'Uploading…',net:'Network error (hotspot may be closed)',"
"alive:'Hotspot online · about ',unit:'s left',dead:'Device unreachable - hotspot may be closed; reopen provisioning on the badge and refresh',"
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
"fid('l_hide').textContent=T('hide');"
"fid('b_savecard').textContent=T('savecard');"
"fid('l_qrs').textContent=T('qrs');fid('b_saveqr').textContent=T('saveqr');"
"for(var i=0;i<4;i++){fid('l_ql'+i).textContent=T('ql');"
"fid('l_qg'+i).textContent=T('qg');fid('l_qu'+i).textContent=T('qu');"
"fid('qt'+i).placeholder=T('qph');"
"fid('b_upq'+i).textContent=T('upq');fid('b_clrq'+i).textContent=T('clr')}"
"fid('l_notes').textContent=T('notes');fid('l_ntxt').textContent=T('ntxt');"
"fid('l_npx').textContent=T('npx');fid('b_ntxt').textContent=T('rdtxt');"
"fid('b_npx').textContent=T('expx');fid('b_ns').textContent=T('saven');"
"fid('b_nc').textContent=T('clr');notesCount();"
"fid('l_img').textContent=T('img');fid('l_avatar').textContent=T('avatar');"
"fid('b_upav').textContent=T('upav');fid('b_clr1').textContent=T('clr');"
"fid('l_crop').textContent=T('crop');fid('b_cropok').textContent=T('ok');"
"fid('b_cancel').textContent=T('cancel');"
"fid('b_done').textContent=T('done');"
"fid('hint').textContent=T('hint')}"
"function toggleLang(){LANG=1-LANG;applyLang()}"
"function sv(u,b,m){msg(m,T('proc'),true);"
"fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(b)}).then(function(r){return r.text()})"
".then(function(t){msg(m,t,true)}).catch(function(e){msg(m,T('net'),false)})}"
"function saveWifi(){sv('/api/wifi',{ssid:fid('ssid').value,pass:fid('pass').value},fid('wm'))}"
"function saveCfg(){sv('/api/config',{name:fid('bname').value,org:fid('borg').value,"
"title:fid('btitle').value,hide:fid('bhide').checked},fid('cm'))}"
"/* ---- 二维码 4 槽:每槽独立选择 生成(文本) 或 上传图片;HTML 无引号写法省转义 ---- */"
"function qmode(i){var up=fid('qu'+i).checked;"
"fid('qt'+i).style.display=up?'none':'block';"
"fid('fq'+i).style.display=up?'block':'none';"
"fid('b_upq'+i).style.display=up?'inline-block':'none';"
"fid('b_clrq'+i).style.display=up?'inline-block':'none'}"
"function buildQr(){var h='';"
"for(var i=0;i<4;i++){"
"h+=\'<div class=slott>QR\'+(i+1)+\'</div>\'+"
"\'<label id=l_ql\'+i+\'></label><input id=ql\'+i+\' maxlength=23>\'+"
"\'<div class=chk><input type=radio name=qm\'+i+\' id=qg\'+i+\'><label id=l_qg\'+i+\' for=qg\'+i+\'></label>\'+"
"\'<input type=radio name=qm\'+i+\' id=qu\'+i+\'><label id=l_qu\'+i+\' for=qu\'+i+\'></label></div>\'+"
"\'<input id=qt\'+i+\'>\'+"
"\'<input type=file id=fq\'+i+\' accept=image/*>\'+"
"\'<button id=b_upq\'+i+\' class=minor></button>\'+"
"\'<button id=b_clrq\'+i+\' class=minor></button><span id=q\'+i+\'m></span>\';"
"}fid('qrs').innerHTML=h;"
"for(var i=0;i<4;i++){(function(i){"
"fid('qg'+i).addEventListener('change',function(){qmode(i)});"
"fid('qu'+i).addEventListener('change',function(){qmode(i)});"
"fid('b_upq'+i).addEventListener('click',function(){"
"openCrop(fid('fq'+i).files[0],'/api/qr'+i,128,'bw',fid('q'+i+'m'))});"
"fid('b_clrq'+i).addEventListener('click',function(){clr('/api/qr'+i,fid('q'+i+'m'))});"
"})(i)}}"
"function saveQr(){var b={qr_text:[],qr_label:[],qr_mode:0};"
"for(var i=0;i<4;i++){b.qr_text.push(fid('qt'+i).value);"
"b.qr_label.push(fid('ql'+i).value);"
"if(fid('qu'+i).checked)b.qr_mode|=(1<<i)}"
"sv('/api/config',b,fid('qrm'))}"
"/* ---- 提词稿:归一化成一行一段;txt 读入;pptx 备注提取(浏览器解包,零依赖) ---- */"
"function notesText(){return fid('notes').value.split(/\\r?\\n/)"
".map(function(s){return s.replace(/\\r$/,'')})"
".filter(function(s){return s.length>0}).join('\\n')}"
"function notesCount(){if(!fid('notesn'))return;"
"var t=notesText();fid('notesn').textContent=(t?t.split('\\n').length:0)+' '+T('nseg')}"
"function saveNotes(){var t=notesText();"
"if(!t){msg(fid('nm'),T('nempty'),false);return}"
"if(t.length>32768){msg(fid('nm'),T('nbig'),false);return}"
"msg(fid('nm'),T('proc'),true);"
"fetch('/api/notes',{method:'POST',headers:{'Content-Type':'text/plain'},body:t})"
".then(function(r){return r.text()}).then(function(tx){msg(fid('nm'),tx,true)})"
".catch(function(e){msg(fid('nm'),T('net'),false)})}"
"function loadTxt(){var f=fid('fnt').files[0];if(!f){msg(fid('nm'),T('pick'),false);return}"
"var r=new FileReader();"
"r.onload=function(){fid('notes').value=r.result;notesCount();msg(fid('nm'),T('ok'),true)};"
"r.readAsText(f,'utf-8')}"
"function rdU16(b,o){return b[o]|(b[o+1]<<8)}"
"function rdU32(b,o){return (b[o]|(b[o+1]<<8)|(b[o+2]<<16)+b[o+3]*16777216)>>>0}"
"function loadPptx(){var f=fid('fpx').files[0];if(!f){msg(fid('nm'),T('pick'),false);return}"
"if(typeof DecompressionStream=='undefined'){msg(fid('nm'),T('pbrowser'),false);return}"
"msg(fid('nm'),T('proc'),true);"
"var r=new FileReader();"
"r.onload=function(){try{parsePptx(new Uint8Array(r.result))}"
"catch(e){msg(fid('nm'),T('perr'),false)}};"
"r.readAsArrayBuffer(f)}"
"function parsePptx(b){"
"var e=-1,i;"
"for(i=b.length-22;i>=0&&i>b.length-22-65536;i--){"
"if(b[i]==0x50&&b[i+1]==0x4b&&b[i+2]==0x05&&b[i+3]==0x06){e=i;break}}"
"if(e<0)throw 0;"
"var n=rdU16(b,e+10),cd=rdU32(b,e+16),items=[];"
"for(var k=0;k<n;k++){"
"if(!(b[cd]==0x50&&b[cd+1]==0x4b&&b[cd+2]==0x01&&b[cd+3]==0x02))break;"
"var cs=rdU32(b,cd+20),nl=rdU16(b,cd+28),el=rdU16(b,cd+30),cl=rdU16(b,cd+32),lho=rdU32(b,cd+42);"
"var name='';"
"for(var j=0;j<nl;j++)name+=String.fromCharCode(b[cd+46+j]);"
"items.push({n:name,cs:cs,lho:lho});cd+=46+nl+el+cl}"
"var rx=/^ppt\\/notesSlides\\/notesSlide\\d+\\.xml$/;"
"var slides=items.filter(function(x){return rx.test(x.n)});"
"slides.sort(function(a,c){"
"return parseInt(a.n.replace(/[^\\d]/g,''),10)-parseInt(c.n.replace(/[^\\d]/g,''),10)});"
"if(!slides.length){msg(fid('nm'),T('pnone'),false);return}"
"var outs=[],done=0;"
"slides.forEach(function(x,ix){"
"var hl=rdU16(b,x.lho+26)+rdU16(b,x.lho+28);"
"var data=b.subarray(x.lho+30+hl,x.lho+30+hl+x.cs);"
"new Response(new Blob([data]).stream().pipeThrough("
"new DecompressionStream('deflate-raw'))).arrayBuffer()"
".then(function(ab){"
"var xml=new TextDecoder('utf-8').decode(ab);"
"var parts=xml.match(/<a:t>[^<]*<\\/a:t>/g)||[];"
"outs[ix]=parts.map(function(s){return s.replace(/<\\/?a:t>/g,'')}).join(' ');"
"if(++done==slides.length){"
"fid('notes').value=outs.filter(function(s){return s}).join('\\n');"
"notesCount();msg(fid('nm'),T('ok'),true)}})"
".catch(function(){msg(fid('nm'),T('perr'),false)})})}"
"function finish(){sv('/api/done',{},fid('dm'))}"
"function clr(u,m){msg(m,T('proc'),true);"
"fetch(u,{method:'POST'}).then(function(r){return r.text()})"
".then(function(t){msg(m,t,true)}).catch(function(e){msg(m,T('net'),false)})}"
"/* ---- 手动裁剪:默认居中正方形,可拖动/缩放,确认后按选区裁剪并上传 ---- */"
"var C={img:null,x:0,y:0,s:0,tgt:null};"
"function boxPos(){var b=fid('cbox');b.style.left=C.x+'px';b.style.top=C.y+'px';"
"b.style.width=C.s+'px';b.style.height=C.s+'px'}"
"function clampBox(){var w=fid('cimg').clientWidth,h=fid('cimg').clientHeight;"
"C.s=Math.max(32,Math.min(C.s,w,h));"
"C.x=Math.max(0,Math.min(C.x,w-C.s));C.y=Math.max(0,Math.min(C.y,h-C.s));boxPos()}"
"var drag=null;"
"fid('cbox').addEventListener('pointerdown',function(e){if(e.target.id=='chandle')return;"
"drag={x:e.clientX,y:e.clientY,ox:C.x,oy:C.y};"
"fid('cbox').setPointerCapture(e.pointerId);e.preventDefault()});"
"fid('chandle').addEventListener('pointerdown',function(e){e.stopPropagation();"
"drag={rs:1,x:e.clientX,y:e.clientY,os:C.s};"
"fid('chandle').setPointerCapture(e.pointerId);e.preventDefault()});"
"window.addEventListener('pointermove',function(e){if(!drag)return;"
"if(drag.rs){C.s=Math.max(32,Math.min(drag.os+Math.max(e.clientX-drag.x,e.clientY-drag.y),"
"fid('cimg').clientWidth,fid('cimg').clientHeight));}"
"else{C.x=drag.ox+e.clientX-drag.x;C.y=drag.oy+e.clientY-drag.y;}"
"clampBox()});"
"window.addEventListener('pointerup',function(){drag=null});"
"function openCrop(file,u,size,mode,m){if(!file){msg(m,T('pick'),false);return}"
"C.tgt={u:u,size:size,mode:mode,m:m};msg(m,T('proc'),true);"
"var img=new Image();"
"img.onload=function(){C.img=img;fid('cimg').src=img.src;"
"fid('crop').style.display='block';"
"requestAnimationFrame(function(){var w=fid('cimg').clientWidth,h=fid('cimg').clientHeight;"
"var s=Math.floor(Math.min(w,h)*0.8);C.x=Math.floor((w-s)/2);C.y=Math.floor((h-s)/2);"
"C.s=s;clampBox()})};"
"img.onerror=function(){msg(m,T('dec'),false)};"
"img.src=URL.createObjectURL(file)}"
"function closeCrop(){fid('crop').style.display='none';C.tgt=null}"
"function cancelCrop(){closeCrop()}"
"function finishCrop(){if(!C.tgt)return;var t=C.tgt;"
"var scale=C.img.naturalWidth/fid('cimg').clientWidth;"
"var sx=Math.round(C.x*scale),sy=Math.round(C.y*scale),ss=Math.round(C.s*scale);"
"var cv=document.createElement('canvas');cv.width=t.size;cv.height=t.size;"
"var cx=cv.getContext('2d');cx.drawImage(C.img,sx,sy,ss,ss,0,0,t.size,t.size);"
"var d=cx.getImageData(0,0,t.size,t.size).data;var body;"
"if(t.mode=='rgb565'){body=new Uint8Array(t.size*t.size*2);"
"for(var i=0;i<t.size*t.size;i++){var v=((d[i*4]>>3)<<11)|((d[i*4+1]>>2)<<5)|(d[i*4+2]>>3);"
"body[i*2]=v&255;body[i*2+1]=v>>8;}}"
"else{body=new Uint8Array(t.size*t.size>>3);"
"for(var j=0;j<t.size*t.size;j++){var lum=(d[j*4]*3+d[j*4+1]*4+d[j*4+2])>>3;"
"if(lum>127)body[j>>3]|=1<<(j&7);}}"
"msg(t.m,T('uping'),true);"
"fetch(t.u,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:body})"
".then(function(r){return r.text()}).then(function(tx){msg(t.m,tx,true);closeCrop()})"
".catch(function(e){msg(t.m,T('net'),false)})}"
"/* ---- 心跳:每 10 秒探测一次设备,顺带显示热点剩余时间 ---- */"
"function beat(){fetch('/api/config').then(function(r){return r.json()})"
".then(function(c){var s=Math.max(0,Math.round((c.remaining||0)/1000));"
"fid('beat').textContent=T('alive')+s+T('unit');fid('beat').style.color='#7ce38b'})"
".catch(function(){fid('beat').textContent=T('dead');fid('beat').style.color='#ff6b6b'})}"
"setInterval(beat,10000);"
"buildQr();"
"fid('notes').addEventListener('input',notesCount);"
"fid('b_ntxt').addEventListener('click',loadTxt);"
"fid('b_npx').addEventListener('click',loadPptx);"
"fid('b_ns').addEventListener('click',saveNotes);"
"fid('b_nc').addEventListener('click',function(){fid('notes').value='';notesCount();clr('/api/notes',fid('nm'))});"
"fetch('/api/notes').then(function(r){return r.text()})"
".then(function(t){fid('notes').value=t||'';notesCount()}).catch(function(){notesCount()});"
"fetch('/api/config').then(function(r){return r.json()}).then(function(c){"
"fid('ssid').value=c.sta_ssid||'';fid('bname').value=c.name||'';"
"fid('borg').value=c.org||'';fid('btitle').value=c.title||'';"
"fid('bhide').checked=!!c.hide;"
"for(var i=0;i<4;i++){fid('qt'+i).value=(c.qr_text||[])[i]||'';"
"fid('ql'+i).value=(c.qr_label||[])[i]||'';"
"var up=((c.qr_mode||0)>>i)&1;fid('qg'+i).checked=!up;fid('qu'+i).checked=up;qmode(i)}"
"LANG=c.lang?1:0;applyLang()}).then(beat).catch(function(){applyLang();beat()});"
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
    cJSON *qt = cJSON_AddArrayToObject(j, "qr_text");
    cJSON *ql = cJSON_AddArrayToObject(j, "qr_label");
    for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
        cJSON_AddItemToArray(qt, cJSON_CreateString(c->qr_text[i]));
        cJSON_AddItemToArray(ql, cJSON_CreateString(c->qr_label[i]));
    }
    cJSON_AddNumberToObject(j, "qr_mode", c->qr_mode);
    cJSON_AddBoolToObject(j, "hide", c->hide_org_title);
    cJSON_AddNumberToObject(j, "layout", c->layout);
    cJSON_AddNumberToObject(j, "theme", c->theme);
    cJSON_AddNumberToObject(j, "lang", c->lang);
    cJSON_AddStringToObject(j, "sta_ssid", ssid);
    cJSON_AddNumberToObject(j, "remaining", (double)app_wifi_portal_remaining_ms());
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
    cJSON *qt = cJSON_GetObjectItem(j, "qr_text");
    cJSON *ql = cJSON_GetObjectItem(j, "qr_label");
    if (cJSON_IsArray(qt)) {
        for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
            cJSON *s = cJSON_GetArrayItem(qt, i);
            if (cJSON_IsString(s))
                strlcpy(c.qr_text[i], s->valuestring, sizeof(c.qr_text[i]));
        }
    }
    if (cJSON_IsArray(ql)) {
        for (int i = 0; i < APP_CFG_QR_SLOTS; i++) {
            cJSON *s = cJSON_GetArrayItem(ql, i);
            if (cJSON_IsString(s))
                strlcpy(c.qr_label[i], s->valuestring, sizeof(c.qr_label[i]));
        }
    }
    if ((item = cJSON_GetObjectItem(j, "qr_mode")) && cJSON_IsNumber(item))
        c.qr_mode = (uint8_t)(item->valueint & 0x0F);
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
    // 保存后热点保持开启,让用户继续上传图片/保存资料;点"完成并联网"或超时才关
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
// fopen 失败时打印 errno:若再现"写入失败",优先怀疑 flash/挂载状态而非长度。
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
        ESP_LOGW(TAG, "上传 %s 打开文件失败(errno=%d)", path, errno);
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

// 槽 0..3 的上传图共用同一处理:文件名 qr0.img..qr3.img,128x128 1bpp = 2048 字节
static esp_err_t post_qr0(httpd_req_t *req) { return upload_asset(req, "qr0.img", 128 * 128 / 8); }
static esp_err_t post_qr1(httpd_req_t *req) { return upload_asset(req, "qr1.img", 128 * 128 / 8); }
static esp_err_t post_qr2(httpd_req_t *req) { return upload_asset(req, "qr2.img", 128 * 128 / 8); }
static esp_err_t post_qr3(httpd_req_t *req) { return upload_asset(req, "qr3.img", 128 * 128 / 8); }

// ---- 提词稿(notes.txt:UTF-8 纯文本,一行 = 一段,上限 32KB) ----

#define NOTES_MAX_BYTES 32768

static esp_err_t post_notes(httpd_req_t *req)
{
    if (!app_store_ready()) {
        return send_text(req, "存储不可用 / Storage unavailable");
    }
    int len = req->content_len;
    if (len < 0 || len > NOTES_MAX_BYTES) {
        return send_text(req, "太长(上限 32KB) / Too long (32KB max)");
    }
    if (len == 0) {
        app_store_write("notes.txt", NULL, 0);
        return send_text(req, "已清除 / Cleared");
    }
    // 32KB 放不进栈上缓冲:流式 recv 直写文件,半截即回滚
    FILE *f = fopen("/store/notes.txt", "wb");
    if (!f) {
        ESP_LOGW(TAG, "notes.txt 打开失败(errno=%d)", errno);
        return send_text(req, "写入失败 / Write failed");
    }
    char buf[512];
    int remaining = len, got = 0, failed = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining);
        if (n <= 0) { failed = 1; break; }
        if (fwrite(buf, 1, n, f) != (size_t)n) { failed = 1; break; }
        remaining -= n;
        got += n;
    }
    fclose(f);
    if (failed) {
        ESP_LOGW(TAG, "notes.txt 上传中断,已回滚(收 %d/%d)", got, len);
        app_store_write("notes.txt", NULL, 0);
        return send_text(req, "接收中断,已回滚 / Interrupted, rolled back");
    }
    ESP_LOGI(TAG, "提词稿已保存(%d 字节)", len);
    return send_text(req, "已保存 / Saved");
}

static esp_err_t get_notes(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (!app_store_ready()) return httpd_resp_send(req, "", 0);
    FILE *f = fopen("/store/notes.txt", "rb");
    if (!f) return httpd_resp_send(req, "", 0);
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const httpd_uri_t URIS[] = {
    { .uri = "/",          .method = HTTP_GET,  .handler = get_index  },
    { .uri = "/api/config",.method = HTTP_GET,  .handler = get_config },
    { .uri = "/api/config",.method = HTTP_POST, .handler = post_config},
    { .uri = "/api/wifi",  .method = HTTP_POST, .handler = post_wifi  },
    { .uri = "/api/done",  .method = HTTP_POST, .handler = post_done  },
    { .uri = "/api/avatar",.method = HTTP_POST, .handler = post_avatar},
    { .uri = "/api/qr0",   .method = HTTP_POST, .handler = post_qr0   },
    { .uri = "/api/qr1",   .method = HTTP_POST, .handler = post_qr1   },
    { .uri = "/api/qr2",   .method = HTTP_POST, .handler = post_qr2   },
    { .uri = "/api/qr3",   .method = HTTP_POST, .handler = post_qr3   },
    { .uri = "/api/notes", .method = HTTP_GET,  .handler = get_notes  },
    { .uri = "/api/notes", .method = HTTP_POST, .handler = post_notes },
};

static httpd_handle_t s_server;

esp_err_t app_portal_http_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;   // 上传路径(FATFS 写入 + recv 循环)在默认 4KB 栈上偏紧
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
