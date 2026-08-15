#include "WebSettings.h"
#include "WebServer.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <cstdlib>
#include <cstring>

WebServer* webServer;

// Captive portal DNS: answers every query with the softAP IP so any host the
// client looks up resolves to us.
DNSServer dnsServer;
const byte DNS_PORT = 53;


const uint32_t NVM_Offset = 0x9000;
uint32_t address = 0x3000;
uint32_t latheaddress = 0x3000 + sizeof(WebSettings);

#define DEFAULTWEBSETTING(setting, default) html += ((webSettings->check == CHECKVALUE ) ? setting : default)
#define DEFAULTLATHESETTING(setting, default) html += ((latheConfig->check == CHECKVALUE ) ? setting : default)

#define DEFAULTLATHEINTSETTING(setting, default, format) \
    sprintf(tempbuffer, format, setting); \
    html += ((latheConfig->check == CHECKVALUE ) ? tempbuffer : default)

// Non-throwing form-field parsers. On empty/invalid input they keep the given
// fallback (the LatheConfig default), and clamp to a safe minimum, so a blank
// or 0 field can never crash setValues() (std::stoi used to throw) or later
// divide by zero (encoder PPR, gearbox denominator).
static int parseIntArg(const char* name, int fallback, int minVal) {
    String a = webServer->arg(name);
    char* end = nullptr;
    long v = strtol(a.c_str(), &end, 10);
    if (a.length() == 0 || end == a.c_str()) v = fallback;
    if (v < minVal) v = minVal;
    return (int)v;
}

static float parseFloatArg(const char* name, float fallback, float minVal) {
    String a = webServer->arg(name);
    char* end = nullptr;
    double v = strtod(a.c_str(), &end);
    if (a.length() == 0 || end == a.c_str()) v = fallback;
    if (v < minVal) v = minVal;
    return (float)v;
}

// Bounded copy into a fixed char[] (arg is attacker/typo controllable).
static void copyArg(const char* name, char* dst, size_t dstSize) {
    String a = webServer->arg(name);
    strncpy(dst, a.c_str(), dstSize - 1);
    dst[dstSize - 1] = '\0';
}





void showPage() {
    WebSettings* webSettings = getWebSettings();
    LatheConfig* latheConfig = getLatheSettings();
    Serial.println("Serving page");

    char tempbuffer[50];

    String html = "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<link rel='icon' href='data:,'><title>ELS Setup</title>";
    html += R"CSS(<style>
*{box-sizing:border-box}
:root{--bg:#eef1f4;--surface:#fff;--surface-2:#f4f6f9;--line:#d7dce3;--line-strong:#c3cad3;
--text:#191d23;--muted:#626d7a;--field-bg:#fff;--accent:#cf7405;--accent-ink:#fff;--accent-soft:#fbeccd;
--bad:#c02626;--bad-bg:#fdecec;--good:#10794f;--radius:13px;--radius-lg:18px;--pad:15px;
--shadow:0 1px 2px rgba(20,30,45,.06),0 8px 24px rgba(20,30,45,.06);
--font:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
@media (prefers-color-scheme:dark){:root{--bg:#101216;--surface:#191c22;--surface-2:#20242c;
--line:#2c313b;--line-strong:#3a414d;--text:#e8ebf0;--muted:#9aa4b1;--field-bg:#14171c;
--accent:#ffab2e;--accent-ink:#1a1205;--accent-soft:#33240d;--bad:#ff6b6b;--bad-bg:#2a1719;--good:#45d69a;
--shadow:0 1px 2px rgba(0,0,0,.4),0 10px 30px rgba(0,0,0,.35)}}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--text);font-family:var(--font);line-height:1.5;-webkit-font-smoothing:antialiased}
.wrap{max-width:480px;margin:0 auto;padding:22px 14px 120px}
.mast{display:flex;align-items:center;gap:13px;padding:2px 4px 18px}
.bolt{flex:none;width:42px;height:42px;border-radius:var(--radius);display:grid;place-items:center;background:var(--accent);color:var(--accent-ink);box-shadow:var(--shadow)}
.bolt svg{width:24px;height:24px}
.mast h1{margin:0;font-size:20px;font-weight:700;letter-spacing:-.01em}
.mast p{margin:1px 0 0;font-size:12.5px;color:var(--muted)}
.chip{margin-left:auto;display:inline-flex;align-items:center;gap:6px;font-size:11px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--good);background:rgba(16,121,79,.14);border:1px solid rgba(16,121,79,.34);padding:6px 10px;border-radius:999px;white-space:nowrap}
.chip i{width:7px;height:7px;border-radius:50%;background:var(--good)}
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--radius-lg);box-shadow:var(--shadow);padding:4px 16px 8px;margin-bottom:16px}
.card>header{padding:14px 2px 8px;margin-bottom:4px;border-bottom:1px solid var(--line)}
.card>header h2{margin:0;font-size:11.5px;font-weight:700;letter-spacing:.16em;text-transform:uppercase}
.field{padding:13px 0 14px;border-bottom:1px solid var(--line)}
.field:last-child{border-bottom:0}
.field>label{display:block;font-size:13.5px;font-weight:600;margin-bottom:7px}
.u{color:var(--muted);font-weight:500}
.control{position:relative;display:flex;align-items:stretch}
input[type=text],input[type=password],input[type=url],input[type=number]{width:100%;font:500 16px/1.2 var(--font);color:var(--text);background:var(--field-bg);border:1.5px solid var(--line-strong);border-radius:var(--radius);padding:var(--pad);appearance:none;-moz-appearance:textfield;transition:border-color .12s,box-shadow .12s}
input::-webkit-outer-spin-button,input::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
input:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-soft)}
.adorn{position:absolute;top:0;right:0;bottom:0;display:flex;align-items:center;padding:0 14px;font-size:12.5px;font-weight:600;color:var(--muted);pointer-events:none}
.has-adorn input{padding-right:58px}
.stp{display:flex;flex:none;margin-left:8px;gap:6px}
.stp button{width:46px;border:1.5px solid var(--line-strong);background:var(--surface-2);color:var(--text);border-radius:var(--radius);font-size:20px;cursor:pointer;display:grid;place-items:center}
.stp button:active{background:var(--accent-soft);border-color:var(--accent)}
.help{margin-top:7px;font-size:12.5px;color:var(--muted);min-height:1em}
.field.invalid input{border-color:var(--bad);background:var(--bad-bg)}
.field .err{display:none;color:var(--bad);font-weight:600}
.field.invalid .err{display:block}
.field.invalid .hintline{display:none}
.pair{display:flex;align-items:center;gap:10px}
.pair .control{flex:1}
.colon{font-size:22px;font-weight:600;color:var(--muted)}
.reveal{position:absolute;right:6px;top:6px;bottom:6px;border:0;background:transparent;color:var(--muted);cursor:pointer;padding:0 10px;border-radius:9px;font-size:12.5px;font-weight:600}
.has-reveal input{padding-right:62px}
.switchrow{display:flex;align-items:center;gap:14px}
.switchtxt{flex:1}
.switch{flex:none;position:relative;width:54px;height:32px}
.switch input{position:absolute;opacity:0;width:100%;height:100%;margin:0}
.track{position:absolute;inset:0;border-radius:999px;background:var(--line-strong);transition:background .15s}
.track::after{content:"";position:absolute;top:3px;left:3px;width:26px;height:26px;border-radius:50%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.3);transition:transform .15s}
.switch input:checked+.track{background:var(--accent)}
.switch input:checked+.track::after{transform:translateX(22px)}
.actions{position:fixed;left:0;right:0;bottom:0;display:flex;padding:14px 16px;background:var(--surface);border-top:1px solid var(--line)}
.actions .inner{width:100%;max-width:480px;margin:0 auto;display:flex;gap:12px}
.btn{border-radius:var(--radius);padding:15px 16px;font-size:15px;font-weight:700;cursor:pointer;border:1.5px solid transparent}
.btn.primary{flex:1;background:var(--accent);color:var(--accent-ink);box-shadow:var(--shadow)}
.btn.ghost{background:transparent;color:var(--muted);border-color:var(--line-strong)}
.toast{position:fixed;left:50%;bottom:88px;transform:translate(-50%,20px);background:var(--text);color:var(--surface);font-size:13.5px;font-weight:600;padding:12px 18px;border-radius:999px;box-shadow:var(--shadow);opacity:0;pointer-events:none;transition:opacity .2s,transform .2s;max-width:90vw;text-align:center}
.toast.show{opacity:1;transform:translate(-50%,0)}
@media (prefers-reduced-motion:reduce){*{transition:none!important}}
</style>)CSS";
    html += "</head><body>";

    html += R"HDR(<div class='wrap'><header class='mast'><span class='bolt'><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="8.5" width="4.5" height="7" rx="1"/><path d="M7.5 12h12"/><path d="M9.5 9.5 8 12l1.5 2.5M12.5 9.5 11 12l1.5 2.5M15.5 9.5 14 12l1.5 2.5M18.5 9.5 17 12l1.5 2.5"/></svg></span><div><h1>ELS Setup</h1><p>Electronic leadscrew configuration</p></div><span class='chip'><i></i>ELS_Wifi</span></header>)HDR";

    html += "<form method='POST' action='/set' id='f' novalidate>";

    // --- Network & Updates ---
    html += "<section class='card'><header><h2>Network &amp; Updates</h2></header>";
    html += "<div class='field'><label for='ssid'>Wi-Fi network (SSID)</label><div class='control'><input id='ssid' name='ssid' type='text' maxlength='31' placeholder='Your Wi-Fi name' required value='";
    DEFAULTWEBSETTING(webSettings->ssid, "");
    html += "'></div><div class='help hintline'>The network the device joins in normal operation.</div><div class='help err'>Enter a network name.</div></div>";

    html += "<div class='field'><label for='password'>Wi-Fi password</label><div class='control has-reveal'><input id='password' name='password' type='password' maxlength='62' placeholder='Network password' value='";
    DEFAULTWEBSETTING(webSettings->password, "");
    html += "'><button type='button' class='reveal' data-reveal='password'>Show</button></div><div class='help hintline'>Leave blank for an open network.</div></div>";

    html += "<div class='field'><label for='url'>Firmware update URL</label><div class='control'><input id='url' name='url' type='url' placeholder='http://.../firmware.bin' value='";
    DEFAULTWEBSETTING(webSettings->url, "http://hass.longhome.co.uk/els/elstft.bin");
    html += "'></div><div class='help hintline'>Where holding Half-Nut pulls OTA updates from.</div><div class='help err'>Must start with http:// or https://</div></div>";
    html += "</section>";

    // --- Lathe Geometry ---
    html += "<section class='card'><header><h2>Lathe Geometry</h2></header>";
    html += "<div class='field'><label for='spindleEncoderPpr'>Spindle encoder <span class='u'>— pulses / rev</span></label><div class='control has-adorn'><input id='spindleEncoderPpr' name='spindleEncoderPpr' type='number' inputmode='numeric' min='1' step='1' required data-int data-min='1' value='";
    DEFAULTLATHEINTSETTING(latheConfig->spindleEncoderPpr, "1200", "%d");
    html += "'><span class='adorn'>PPR</span><div class='stp'><button type='button' data-step='-1'>–</button><button type='button' data-step='1'>+</button></div></div><div class='help hintline'>Counts per spindle revolution.</div><div class='help err'>Whole number ≥ 1 (0 would divide by zero).</div></div>";

    html += "<div class='field'><label for='stepperPpr'>Leadscrew stepper <span class='u'>— steps / rev</span></label><div class='control has-adorn'><input id='stepperPpr' name='stepperPpr' type='number' inputmode='numeric' min='1' step='1' required data-int data-min='1' value='";
    DEFAULTLATHEINTSETTING(latheConfig->stepperPpr, "400", "%d");
    html += "'><span class='adorn'>PPR</span><div class='stp'><button type='button' data-step='-1'>–</button><button type='button' data-step='1'>+</button></div></div><div class='help hintline'>Driver steps for one motor turn (incl. microstepping).</div><div class='help err'>Whole number ≥ 1.</div></div>";

    html += "<div class='field' id='ratiofield'><label>Gearbox ratio <span class='u'>— motor : leadscrew</span></label><div class='pair'><div class='control'><input id='gearboxRatioNumerator' name='gearboxRatioNumerator' type='number' inputmode='numeric' min='1' step='1' required data-int data-min='1' aria-label='ratio numerator' value='";
    DEFAULTLATHEINTSETTING(latheConfig->gearboxRatioNumerator, "2", "%d");
    html += "'></div><span class='colon'>:</span><div class='control'><input id='gearboxRatioDenominator' name='gearboxRatioDenominator' type='number' inputmode='numeric' min='1' step='1' required data-int data-min='1' aria-label='ratio denominator' value='";
    DEFAULTLATHEINTSETTING(latheConfig->gearboxRatioDenominator, "1", "%d");
    html += "'></div></div><div class='help hintline'>Direct drive is 1 : 1.</div><div class='help err'>Both sides must be whole numbers ≥ 1.</div></div>";

    html += "<div class='field'><label for='leadscrewPitchMm'>Leadscrew pitch</label><div class='control has-adorn'><input id='leadscrewPitchMm' name='leadscrewPitchMm' type='number' inputmode='decimal' min='0.001' step='0.001' required data-min='0.001' value='";
    DEFAULTLATHEINTSETTING(latheConfig->leadscrewPitchMm, "2.54", "%g");
    html += "'><span class='adorn'>mm</span></div><div class='help hintline'>Carriage travel per leadscrew turn.</div><div class='help err'>Must be greater than 0.</div></div>";

    html += "<div class='field'><div class='switchrow'><div class='switchtxt'><label for='invertDirection' style='margin:0'>Invert motor direction</label><div class='help hintline' style='margin-top:4px'>Flip if the carriage runs the wrong way.</div></div><label class='switch'><input id='invertDirection' name='invertDirection' value='invert' type='checkbox'";
    if (latheConfig->check != CHECKVALUE || latheConfig->invertDirection) {
        html += " checked";
    }
    html += "><span class='track'></span></label></div></div>";
    html += "</section>";

    // --- Motion Limits ---
    html += "<section class='card'><header><h2>Motion Limits</h2></header>";
    html += "<div class='field'><label for='jogSpeed'>Max jog speed</label><div class='control has-adorn'><input id='jogSpeed' name='jogSpeed' type='number' inputmode='numeric' min='1' step='1' required data-int data-min='1' value='";
    DEFAULTLATHEINTSETTING(latheConfig->jogSpeed, "40", "%d");
    html += "'><span class='adorn'>mm/s</span><div class='stp'><button type='button' data-step='-1'>–</button><button type='button' data-step='1'>+</button></div></div><div class='help hintline'>Top speed when jogging by hand.</div><div class='help err'>Whole number ≥ 1.</div></div>";

    html += "<div class='field'><label for='leadscrewAcceleration'>Acceleration</label><div class='control has-adorn'><input id='leadscrewAcceleration' name='leadscrewAcceleration' type='number' inputmode='numeric' min='1' step='1' required data-int data-min='1' value='";
    DEFAULTLATHEINTSETTING(latheConfig->leadscrewAcceleration, "150", "%d");
    html += "'><span class='adorn'>mm/s²</span><div class='stp'><button type='button' data-step='-1'>–</button><button type='button' data-step='1'>+</button></div></div><div class='help hintline'>Higher is snappier, but can stall the motor.</div><div class='help err'>Whole number ≥ 1.</div></div>";

    html += "<div class='field'><label for='leadscrewMaxSpeed'>Max leadscrew speed</label><div class='control has-adorn'><input id='leadscrewMaxSpeed' name='leadscrewMaxSpeed' type='number' inputmode='numeric' min='1' step='1' required data-int data-min='1' value='";
    DEFAULTLATHEINTSETTING(latheConfig->leadscrewMaxSpeed, "40", "%d");
    html += "'><span class='adorn'>mm/s</span><div class='stp'><button type='button' data-step='-1'>–</button><button type='button' data-step='1'>+</button></div></div><div class='help hintline'>Speed cap while feeding / threading.</div><div class='help err'>Whole number ≥ 1.</div></div>";
    html += "</section>";

    html += "<p style='text-align:center;color:var(--muted);font-size:12px;margin:4px 0 0'>Saved to the device. Press Reset to restart with new settings.</p>";
    html += "</form></div>";

    html += "<div class='actions'><div class='inner'><button class='btn ghost' type='button' id='resetBtn'>Reset</button><button class='btn primary' type='submit' form='f' id='saveBtn'>Save settings</button></div></div>";
    html += "<div class='toast' id='toast' role='status' aria-live='polite'></div>";

    html += R"JS(<script>
(function(){
var f=document.getElementById('f');
function fld(i){return i.closest('.field');}
function validate(i){var l=fld(i);if(!l)return true;var v=i.value.trim(),ok=true;
if(i.hasAttribute('required')&&v==='')ok=false;
if(ok&&v!==''&&i.type==='number'){var n=Number(v);if(isNaN(n))ok=false;
if(i.hasAttribute('data-int')&&!Number.isInteger(n))ok=false;
var m=i.getAttribute('data-min');if(m!==null&&n<Number(m))ok=false;}
if(ok&&i.type==='url'&&v!==''&&!/^https?:\/\//i.test(v))ok=false;
if(l.id==='ratiofield'){var a=document.getElementById('gearboxRatioNumerator'),b=document.getElementById('gearboxRatioDenominator');
var both=[a,b].every(function(x){var n=Number(x.value);return x.value.trim()!==''&&Number.isInteger(n)&&n>=1;});
l.classList.toggle('invalid',!both);return both;}
l.classList.toggle('invalid',!ok);return ok;}
[].forEach.call(f.querySelectorAll('input'),function(i){
i.addEventListener('input',function(){validate(i);});i.addEventListener('blur',function(){validate(i);});});
[].forEach.call(document.querySelectorAll('.stp button'),function(b){b.addEventListener('click',function(){
var i=b.closest('.control').querySelector('input');var s=parseFloat(i.step)||1;
var v=(parseFloat(i.value)||0)+parseInt(b.getAttribute('data-step'),10)*s;var mn=parseFloat(i.min);
if(!isNaN(mn)&&v<mn)v=mn;i.value=(s%1===0)?Math.round(v):v.toFixed(3);validate(i);});});
[].forEach.call(document.querySelectorAll('.reveal'),function(btn){btn.addEventListener('click',function(){
var i=document.getElementById(btn.getAttribute('data-reveal'));var sh=i.type==='password';
i.type=sh?'text':'password';btn.textContent=sh?'Hide':'Show';});});
var toast=document.getElementById('toast'),tt;
function say(m){toast.textContent=m;toast.classList.add('show');clearTimeout(tt);tt=setTimeout(function(){toast.classList.remove('show');},2600);}
document.getElementById('resetBtn').addEventListener('click',function(){if(confirm('Restart the device now?'))location.href='/reset';});
f.addEventListener('submit',function(e){var bad=null;
[].forEach.call(f.querySelectorAll('input[type=number],input[required]'),function(i){if(!validate(i)&&!bad)bad=i;});
if(bad){e.preventDefault();bad.focus();bad.scrollIntoView({behavior:'smooth',block:'center'});say('Fix the highlighted fields first');}});
})();
</script>)JS";
    html += "</body></html>";

    delete webSettings;
    delete latheConfig;

    webServer->send(200, "text/html", html);
}

WebSettings* getWebSettings() {
    WebSettings* settings = new WebSettings();
    ESP.flashRead(NVM_Offset + address, (uint32_t*)settings, sizeof(WebSettings));
    return settings;
}

LatheConfig* getLatheSettings() {
    LatheConfig* settings = new LatheConfig();
    ESP.flashRead(NVM_Offset + latheaddress, (uint32_t*)settings, sizeof(LatheConfig));
    return settings;
}

void setValues() {
    Serial.printf("SSID %s\n", webServer->arg("ssid").c_str());
    Serial.printf("Password %s\n", webServer->arg("password").c_str());
    Serial.printf("update url %s\n", webServer->arg("url").c_str());
    WebSettings settings;
    copyArg("ssid", settings.ssid, sizeof(settings.ssid));
    copyArg("password", settings.password, sizeof(settings.password));
    copyArg("url", settings.url, sizeof(settings.url));
    settings.check = CHECKVALUE;

    LatheConfig config;  // members start at their safe defaults
    config.spindleEncoderPpr       = parseIntArg("spindleEncoderPpr", config.spindleEncoderPpr, 1);
    config.stepperPpr              = parseIntArg("stepperPpr", config.stepperPpr, 1);
    config.gearboxRatioNumerator   = parseIntArg("gearboxRatioNumerator", config.gearboxRatioNumerator, 1);
    config.gearboxRatioDenominator = parseIntArg("gearboxRatioDenominator", config.gearboxRatioDenominator, 1);
    config.jogSpeed                = parseIntArg("jogSpeed", config.jogSpeed, 1);
    config.leadscrewAcceleration   = parseIntArg("leadscrewAcceleration", config.leadscrewAcceleration, 1);
    config.leadscrewMaxSpeed       = parseIntArg("leadscrewMaxSpeed", config.leadscrewMaxSpeed, 1);
    config.leadscrewPitchMm        = parseFloatArg("leadscrewPitchMm", config.leadscrewPitchMm, 0.001f);
    config.invertDirection = strcmp(webServer->arg("invertDirection").c_str(), "invert") == 0;
    config.check = CHECKVALUE;

    ESP.flashEraseSector((NVM_Offset + address) / 4096);
    ESP.flashWrite(NVM_Offset + address, (uint32_t*)&settings, sizeof(WebSettings));
    ESP.flashWrite(NVM_Offset + latheaddress,  (uint32_t*)&config, sizeof(LatheConfig));

    showPage();

}

void reset() {
    ESP.restart();
}

// Redirect any unexpected request to the portal root. Returning a 302 to the
// softAP IP is what makes iOS/Android/Windows pop the "Sign in to network" sheet.
void redirectToPortal() {
    String portalUrl = String("http://") + WiFi.softAPIP().toString() + "/";
    webServer->sendHeader("Location", portalUrl, true);
    webServer->send(302, "text/plain", "");
}

void startWebServer() {
    webServer = new WebServer(80);
    webServer->on("/", HTTP_GET, showPage);
    webServer->on("/set", HTTP_POST, setValues);
    webServer->on("/reset", HTTP_GET, reset);

    // OS captive-portal probe endpoints. Redirecting these (instead of returning
    // the expected success payload) tells the OS the network is "captive" and
    // triggers the sign-in popup.
    webServer->on("/generate_204", HTTP_GET, redirectToPortal);            // Android
    webServer->on("/gen_204", HTTP_GET, redirectToPortal);                 // Android
    webServer->on("/hotspot-detect.html", HTTP_GET, redirectToPortal);     // Apple
    webServer->on("/library/test/success.html", HTTP_GET, redirectToPortal); // Apple
    webServer->on("/connecttest.txt", HTTP_GET, redirectToPortal);         // Windows
    webServer->on("/ncsi.txt", HTTP_GET, redirectToPortal);               // Windows
    webServer->on("/redirect", HTTP_GET, redirectToPortal);               // Windows
    webServer->on("/fwlink", HTTP_GET, redirectToPortal);                 // Microsoft

    // Any other host/path also shows the portal.
    webServer->onNotFound(redirectToPortal);

    webServer->begin();

    // Start the catch-all DNS server so every lookup resolves to the softAP IP.
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

    Serial.println("Server is running");

}

void wifiLoop() {
    dnsServer.processNextRequest();
    webServer->handleClient();
}

