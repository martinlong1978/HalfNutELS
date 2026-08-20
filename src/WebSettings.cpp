#include "WebSettings.h"
#include "WebServer.h"
#include <globalstate.h>   // motion-mode guard in saveLathePreferences()
#include <WiFi.h>
#include <DNSServer.h>
#include <cstdlib>
#include <cstring>

WebServer* webServer;

// Captive portal DNS: answers every query with the softAP IP so any host the
// client looks up resolves to us.
DNSServer dnsServer;
const byte DNS_PORT = 53;


// Both saved structs live back-to-back in ONE 4 KB flash sector, inside the
// `nvs` partition (0x9000, 0x5000 - see my_4MB.csv). constexpr rather than
// plain globals so the layout can be checked at compile time below; nothing
// outside this file references them.
constexpr uint32_t NVM_Offset = 0x9000;
constexpr uint32_t address = 0x3000;
constexpr uint32_t latheaddress = address + sizeof(WebSettings);

// --- Sector budget --------------------------------------------------------
// setValues() and saveLathePreferences() both erase exactly ONE sector,
// (NVM_Offset + address) / 4096, and then write BOTH structs into it. That is
// only correct while both structs fit inside that single sector. If they ever
// stop fitting, LatheConfig spills into the next sector, which is never
// erased - and flash writes can only clear bits, so the spilled bytes would be
// AND-ed into whatever was already there. That corrupts silently: no error is
// returned, the data just reads back wrong. Hence a compile-time budget.
//
// Current numbers:
//     sizeof(WebSettings) = 868   (4 + 32 + 63 + 512 + 256 = 867, padded to 868)
//     sizeof(LatheConfig) =  44
//     total               = 912 of 4096  ->  3184 bytes headroom
// Adding fields is therefore safe today, but is NOT unlimited - grow
// WebSettings::url and this is the assert that will stop you.
//
// NOTE what the 256-byte debugUrl addition cost: LatheConfig is stored at
// `latheaddress = address + sizeof(WebSettings)`, so growing WebSettings moved
// it, invalidating every device's stored settings. That is why CHECKVALUE was
// bumped alongside it (latheconfig.h). Any future growth here has the same
// consequence and needs the same bump.
constexpr uint32_t SECTOR_SIZE = 4096;
constexpr uint32_t SETTINGS_BYTES = sizeof(WebSettings) + sizeof(LatheConfig);

// The single sector every writer in this file erases. Named once so the three
// call sites - setValues(), saveLathePreferences() and the factory reset below
// - cannot drift apart, and so the asserts above are provably about the same
// sector the code actually erases.
constexpr uint32_t SETTINGS_SECTOR = (NVM_Offset + address) / SECTOR_SIZE;

static_assert(((NVM_Offset + address) % SECTOR_SIZE) + SETTINGS_BYTES <= SECTOR_SIZE,
              "WebSettings + LatheConfig no longer fit the single erased sector - "
              "LatheConfig would spill into a sector that is never erased");

// spi_flash_read/write require 4-byte-aligned offsets and lengths. Both struct
// sizes are multiples of 4 (each has 4-byte alignment, so sizeof rounds up),
// which is also what keeps `latheaddress` aligned - assert it rather than rely
// on it.
static_assert((NVM_Offset + address) % 4 == 0, "WebSettings flash offset must be 4-byte aligned");
static_assert((NVM_Offset + latheaddress) % 4 == 0, "LatheConfig flash offset must be 4-byte aligned");
static_assert(sizeof(WebSettings) % 4 == 0, "flashWrite length must be a multiple of 4");
static_assert(sizeof(LatheConfig) % 4 == 0, "flashWrite length must be a multiple of 4");

// ...and the whole region must stay inside the nvs partition (0x9000 + 0x5000).
static_assert(NVM_Offset + address + SETTINGS_BYTES <= 0x9000 + 0x5000,
              "settings region escapes the nvs partition");

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





// Shared page chrome. The portal serves more than one page now (the settings
// form, and the two-step factory reset below), all to the same phone over the
// same captive portal, so the stylesheet lives here rather than as a second
// copy that would drift out of step with the first.
static const char PAGE_CSS[] = R"CSS(<style>
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
.card.danger{border-color:var(--bad);background:var(--bad-bg)}
.card.danger>header{border-bottom-color:var(--bad)}
.card.danger>header h2{color:var(--bad)}
.zone{margin-top:34px}
.msg p{margin:0 0 10px;font-size:13.5px}
.msg p:last-of-type{margin-bottom:2px}
.msg p.lead{font-weight:700;font-size:15.5px;color:var(--bad)}
.msg ul{margin:0 0 12px;padding-left:20px;font-size:13.5px;color:var(--muted)}
.msg li{margin-bottom:4px}
.msg strong{color:var(--text)}
.btn.danger{display:block;width:100%;text-align:center;text-decoration:none;background:var(--bad);color:var(--bad-bg);box-shadow:var(--shadow);margin:10px 0 8px}
.btn.safe{display:block;width:100%;text-align:center;text-decoration:none;background:var(--surface);color:var(--text);border-color:var(--line-strong);margin:14px 0 4px}
.bolt.bad{background:var(--bad);color:var(--bad-bg)}
</style>)CSS";

// Opens a portal page: doctype, head (with `title`), the shared stylesheet and
// <body>. Everything this file serves starts here.
static String pageOpen(const char* title) {
    String html = "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<link rel='icon' href='data:,'><title>";
    html += title;
    html += "</title>";
    html += PAGE_CSS;
    html += "</head><body>";
    return html;
}

void showPage() {
    WebSettings* webSettings = getWebSettings();
    LatheConfig* latheConfig = getLatheSettings();
    Serial.println("Serving page");

    char tempbuffer[50];

    String html = pageOpen("ELS Setup");

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
    DEFAULTWEBSETTING(webSettings->url, "https://github.com/martinlong1978/HalfNutELS/releases/latest/download/elstft.bin");
    html += "'></div><div class='help hintline'>Where holding Half-Nut pulls OTA updates from.</div><div class='help err'>Must start with http:// or https://</div></div>";

    // The motion-trace sink. Not a required field: leave it blank and the
    // Debug capture tile still records, it just has nowhere to send the trace.
    // The default offered here is the server in tools/debugsink - keep the two
    // in step if either changes.
    html += "<div class='field'><label for='debugUrl'>Debug capture URL</label><div class='control'><input id='debugUrl' name='debugUrl' type='url' placeholder='http://host:8088/capture' value='";
    DEFAULTWEBSETTING(webSettings->debugUrl, "http://hass.longhome.co.uk:8088/capture");
    html += "'></div><div class='help hintline'>Where the Debug capture tile POSTs its motion trace. Blank to disable.</div><div class='help err'>Must start with http:// or https://</div></div>";
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

    html += "<p style='text-align:center;color:var(--muted);font-size:12px;margin:4px 0 0'>Saved to the device. Press Restart to reboot with the new settings.</p>";
    html += "</form>";

    // --- Danger zone ------------------------------------------------------
    // Deliberately outside the settings <form> and below every ordinary
    // control, in its own red card: this is not a variant of Save, and it is
    // emphatically not the Restart button in the bar at the foot of the page.
    // The link is step one of two - it only opens the confirmation page, which
    // is where anything is actually erased.
    html += "<section class='card danger zone'><header><h2>Danger zone</h2></header>";
    html += "<div class='field msg'>";
    html += "<p class='lead'>Factory reset</p>";
    html += "<p>Erases <strong>everything</strong> the device has stored: the Wi-Fi network and password, the firmware update URL, and every lathe measurement on this page. It restarts into first-boot setup and comes back as the <strong>ELS_Wifi</strong> access point, to be set up again from scratch.</p>";
    html += "<p>This is not the Restart button. It keeps nothing, and it will disconnect you from the device.</p>";
    html += "<a class='btn danger' href='/factoryreset'>Erase all settings&hellip;</a>";
    html += "</div></section>";

    html += "</div>";

    html += "<div class='actions'><div class='inner'><button class='btn ghost' type='button' id='restartBtn'>Restart</button><button class='btn primary' type='submit' form='f' id='saveBtn'>Save settings</button></div></div>";
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
document.getElementById('restartBtn').addEventListener('click',function(){if(confirm('Restart the device now? Saved settings are kept.'))location.href='/reset';});
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
    copyArg("debugUrl", settings.debugUrl, sizeof(settings.debugUrl));
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

    ESP.flashEraseSector(SETTINGS_SECTOR);
    ESP.flashWrite(NVM_Offset + address, (uint32_t*)&settings, sizeof(WebSettings));
    ESP.flashWrite(NVM_Offset + latheaddress,  (uint32_t*)&config, sizeof(LatheConfig));

    showPage();

}

// --- Device-writable preferences ------------------------------------------
// Theme and DRO datum, and nothing else - see the declarations in
// WebSettings.h for why geometry never crosses this boundary.

// Reads the stored LatheConfig onto the stack (no heap, unlike
// getLatheSettings(): these run from a menu press on the DisplayTask and have
// no reason to allocate). Returns true only when flash actually held a blob
// this firmware recognises. On false, `out` holds nothing trustworthy - the
// read may have half-filled it - so a false return means "use your own
// defaults, and do not write this back". Both callers below do exactly that.
static bool readStoredLatheConfig(LatheConfig& out) {
    // `check` is the one member with no in-class initialiser, so clear it
    // before the read: a failed or partial flashRead must never leave an
    // indeterminate sentinel that could happen to compare equal to CHECKVALUE.
    out.check = 0;
    // alignof(LatheConfig) == 4 (asserted in latheconfig.h), so &out satisfies
    // flashRead's 4-byte-aligned uint32_t* contract.
    if (!ESP.flashRead(NVM_Offset + latheaddress, (uint32_t*)&out,
                       sizeof(LatheConfig))) {
        return false;
    }
    return out.check == CHECKVALUE;
}

bool readLathePreferences(uint8_t& theme, DroDatumPreference& droDatum) {
    LatheConfig stored;
    if (!readStoredLatheConfig(stored)) {
        // Unreachable in normal operation: main.cpp enters AP setup rather
        // than starting the machine when the stored blob fails this same
        // check, so nothing that could call this exists. Answer with the
        // defaults anyway rather than with whatever was on the stack.
        theme = THEME_DARK;
        droDatum = DroDatumPreference::Left;
        return false;
    }
    theme = normaliseTheme(stored.theme);
    droDatum = toDroDatumPreference(stored.droDatum);
    return true;
}

bool saveLathePreferences(uint8_t theme, DroDatumPreference droDatum) {
    // A flash erase takes the flash lock and disables the instruction cache on
    // BOTH cores. Nothing in main.cpp or lib/leadscrew is IRAM_ATTR (only the
    // keyarray timer ISR is), so the SpindleTask on core 0 freezes for the
    // whole operation - tens of milliseconds for a 4 KB sector erase - and
    // step generation stops dead. Mid-cut that loses spindle sync and ruins
    // the thread.
    //
    // setValues(), the only other writer, is safe because it runs solely in AP
    // config mode where no motion exists. This one is called from the running
    // machine's menu, so it has to defend itself. See docs/ux-redesign.md for
    // the caller-side story.
    //
    // Checked first, before any flash access at all: if the answer is "no",
    // nothing else needs doing.
    GlobalMotionMode motion = GlobalState::getInstance()->getMotionMode();
    if (motion != GlobalMotionMode::MM_DISABLED &&
        motion != GlobalMotionMode::MM_UNSET) {
        Serial.println("saveLathePreferences: refused, carriage under power");
        return false;
    }

    // Read-modify-write step 1a: the stored LatheConfig. THIS is what carries
    // the user's commissioned geometry across the erase - it is read back out
    // of flash and written straight back, so the nine geometry fields make the
    // round trip without this code, or its caller, ever holding an opinion
    // about what they should be.
    //
    // Refuse if it is not a blob we recognise. Rewriting it would leave those
    // unknown bytes in place with our two preference bytes on top; stamping
    // CHECKVALUE onto them would be worse still, promoting garbage to valid
    // geometry. Neither is worth doing for a theme toggle.
    LatheConfig stored;
    if (!readStoredLatheConfig(stored)) {
        Serial.println("saveLathePreferences: refused, no valid stored settings");
        return false;
    }

    // The modify. Two bytes, normalised; see applyLathePreferences() in
    // lib/config, which is where this rule is host-tested.
    applyLathePreferences(stored, theme, droDatum);

    // Read-modify-write step 1b: pull the CURRENT WebSettings out of flash so
    // they can be rewritten unchanged. getWebSettings() always returns a
    // (heap-allocated) struct - even pre-first-run, it is just whatever
    // bytes are presently in that sector (valid, or unconfigured/garbage
    // with check != CHECKVALUE) - and either way, writing those same bytes
    // back after the erase is a no-op on their meaning, so there is no need
    // to branch on webSettings->check here.
    WebSettings* webSettings = getWebSettings();

    // Step 2/3: erase the shared 4 KB sector once, then rewrite BOTH structs
    // - the preserved WebSettings and the preserved-plus-two-bytes LatheConfig
    // - into it. Erasing without immediately rewriting both would leave a
    // window where a reset/power-loss loses everything in the sector; there is
    // no safer ordering available with a single-sector, erase-as-a-unit flash
    // region.
    ESP.flashEraseSector(SETTINGS_SECTOR);
    ESP.flashWrite(NVM_Offset + address, (uint32_t*)webSettings, sizeof(WebSettings));
    ESP.flashWrite(NVM_Offset + latheaddress, (uint32_t*)&stored, sizeof(LatheConfig));

    // getWebSettings() hands us an owned pointer - free it rather than
    // leaking it (see the known leak at main.cpp:124-125, which this must
    // not replicate).
    delete webSettings;
    return true;
}

void reset() {
    ESP.restart();
}

// --- Factory reset --------------------------------------------------------
//
// Two requests, on purpose. GET /factoryreset only renders a confirmation page
// and hands out a one-shot token; POST /factoryreset/confirm is the only thing
// that erases anything, and only if it is given that token back. So no single
// tap - and no stray GET from a link prefetcher, a captive-portal probe or a
// mis-typed URL - can wipe the machine, and the confirmation survives a phone
// with JavaScript turned off, which a window.confirm() would not.
//
// The route is /factoryreset rather than anything nearer /reset, which merely
// reboots: the two must not be confusable in a URL bar or a bookmark.
//
// WHAT IT ERASES, AND WHY THAT IS "FIRST BOOT": WebSettings and LatheConfig sit
// back-to-back in the single 4 KB sector asserted at the top of this file, so
// erasing SETTINGS_SECTOR sets every byte of BOTH structs to 0xFF. CHECKVALUE
// is asserted (latheconfig.h) to be neither 0xFFFFFFFF nor 0, so afterwards
// `webSettings->check == CHECKVALUE && config->check == CHECKVALUE` in
// setup() is false, and main.cpp takes its !checks branch into
// runWifiSettings(). That is the same path a brand-new device takes, reached
// through the same test - nothing here has to imitate first boot, it simply
// stops being configured.
//
// Nothing is written back. Unlike saveLathePreferences(), which must rewrite
// both structs immediately because an erase-without-write window would lose the
// user's settings, here the erased state IS the goal: a power cut between the
// erase and the restart leaves exactly the intended outcome.
static uint32_t factoryResetToken = 0;   // 0 means "none outstanding"

static uint32_t issueFactoryResetToken() {
    uint32_t token = 0;
    while (token == 0) {   // 0 is the sentinel, so never hand it out
        token = ((uint32_t)random(0x7FFFFFFF) << 1) ^ (uint32_t)micros();
    }
    factoryResetToken = token;
    return token;
}

// Common masthead for the two factory-reset pages: same shape as the settings
// page's, in the danger colour, with a warning triangle instead of the bolt.
static void appendDangerMast(String& html, const char* heading, const char* sub) {
    html += R"HDR(<div class='wrap'><header class='mast'><span class='bolt bad'><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3.6 1.9 20.4h20.2z"/><path d="M12 9.4v4.8"/><path d="M12 17.3v.01"/></svg></span><div><h1>)HDR";
    html += heading;
    html += "</h1><p>";
    html += sub;
    html += "</p></div></header>";
}

// Step one of two. `stale` re-serves the page after a token that was already
// spent or lost (a back-button re-post, or a reboot between the two requests) -
// it says plainly that nothing was erased, rather than silently doing nothing.
static void sendFactoryResetPage(bool stale) {
    char tokenbuf[16];
    snprintf(tokenbuf, sizeof(tokenbuf), "%lu",
             (unsigned long)issueFactoryResetToken());

    String html = pageOpen("Confirm factory reset");
    appendDangerMast(html, "Factory reset", "Confirm before anything is erased");

    if (stale) {
        html += "<section class='card danger'><div class='field msg'>";
        html += "<p class='lead'>Nothing has been erased.</p>";
        html += "<p>That confirmation had already been used, or the device restarted since it was issued. Your settings are untouched. Read this through again and press the button at the bottom if you still want to reset.</p>";
        html += "</div></section>";
    }

    html += "<section class='card danger'><header><h2>What this erases</h2></header>";
    html += "<div class='field msg'>";
    html += "<p class='lead'>Everything, not just the lathe settings.</p>";
    html += "<ul>";
    html += "<li>The Wi-Fi network name and password</li>";
    html += "<li>The firmware update URL and the debug capture URL</li>";
    html += "<li>Spindle encoder PPR, stepper PPR, gearbox ratio, leadscrew pitch and motor direction</li>";
    html += "<li>Jog speed, acceleration and maximum leadscrew speed</li>";
    html += "<li>The display theme and DRO datum</li>";
    html += "</ul>";
    html += "<p><strong>You will lose your connection to the device.</strong> The Wi-Fi credentials are erased along with the rest, so this page will not come back, and the device cannot fetch updates again until it has been set up.</p>";
    html += "<p>There is no undo, and nothing is backed up. Write down anything above that you would not enjoy measuring again.</p>";
    html += "</div></section>";

    html += "<section class='card'><header><h2>What happens next</h2></header>";
    html += "<div class='field msg'>";
    html += "<p>The device erases its stored settings and restarts immediately into setup mode, exactly as it behaves on a first boot.</p>";
    html += "<p>It comes back as its own access point, <strong>ELS_Wifi</strong>. Join that network from your phone or laptop, open the setup page, and enter the Wi-Fi details and every lathe measurement again from scratch.</p>";
    html += "<a class='btn safe' href='/'>Cancel and keep my settings</a>";
    html += "<form method='POST' action='/factoryreset/confirm'>";
    html += "<input type='hidden' name='token' value='";
    html += tokenbuf;
    html += "'>";
    html += "<button class='btn danger' type='submit'>Erase everything and restart</button>";
    html += "</form>";
    html += "</div></section>";

    html += "</div></body></html>";

    webServer->send(200, "text/html", html);
}

static void showFactoryResetPage() {
    sendFactoryResetPage(false);
}

// Step two of two: the only code that erases the settings sector.
static void confirmFactoryReset() {
    // Spend the outstanding token before looking at anything else, so it is
    // one-shot however this request ends.
    const uint32_t outstanding = factoryResetToken;
    factoryResetToken = 0;

    String supplied = webServer->arg("token");
    char* end = nullptr;
    unsigned long parsed = strtoul(supplied.c_str(), &end, 10);
    if (outstanding == 0 || supplied.length() == 0 || end == supplied.c_str() ||
        (uint32_t)parsed != outstanding) {
        Serial.println("Factory reset: bad or spent token, nothing erased");
        sendFactoryResetPage(true);
        return;
    }

    Serial.println("Factory reset: erasing settings sector");
    if (!ESP.flashEraseSector(SETTINGS_SECTOR)) {
        // Deliberately does NOT restart: leaving the device up is what lets
        // the user read this, and the settings are most likely still intact.
        Serial.println("Factory reset: sector erase FAILED, not restarting");
        String html = pageOpen("Factory reset failed");
        appendDangerMast(html, "Erase failed", "The device has not been restarted");
        html += "<section class='card danger'><div class='field msg'>";
        html += "<p class='lead'>The device could not erase its settings storage.</p>";
        html += "<p>Nothing was restarted, and your settings are almost certainly still in place. Try again, or power the device off and on and then try again.</p>";
        html += "<a class='btn safe' href='/'>Back to settings</a>";
        html += "</div></section></div></body></html>";
        webServer->send(500, "text/html", html);
        return;
    }

    // Answer BEFORE restarting - the browser that sent this deserves a page
    // rather than a dropped connection - then give the response time to go out
    // over TCP. delay() is safe here: this runs on the Arduino loop task in AP
    // config mode, where no motion tasks exist at all (see the reachability
    // note above startWebServer()), and 500 ms is well inside the loop task
    // watchdog, which is still enabled on this branch.
    String html = pageOpen("Settings erased");
    appendDangerMast(html, "Settings erased", "The device is restarting");
    html += "<section class='card'><div class='field msg'>";
    html += "<p class='lead'>Everything has been erased.</p>";
    html += "<p>The device is restarting now, into first-boot setup mode.</p>";
    html += "<p>This connection is about to drop and this page will not reload. Rejoin the <strong>ELS_Wifi</strong> access point and open the setup page to configure the device again: Wi-Fi details first, then every lathe measurement.</p>";
    html += "</div></section></div></body></html>";
    webServer->send(200, "text/html", html);

    delay(500);
    ESP.restart();
}

// Redirect any unexpected request to the portal root. Returning a 302 to the
// softAP IP is what makes iOS/Android/Windows pop the "Sign in to network" sheet.
void redirectToPortal() {
    String portalUrl = String("http://") + WiFi.softAPIP().toString() + "/";
    webServer->sendHeader("Location", portalUrl, true);
    webServer->send(302, "text/plain", "");
}

// REACHABILITY - relied on by the factory reset above, so it is written down
// here rather than assumed. Derived from the call graph below, and separately
// CONFIRMED BY THE OWNER: the web page is AP-only by design, not by accident.
// Treat it as an invariant to preserve, not an observation that happened to
// hold when this was written.
//
// This function is called from exactly one place:
// runWifiSettings() in src/main.cpp, which setup() enters only when H2 is held,
// when the stored settings fail their CHECKVALUE test, or when the menu asked
// for setup. That branch never constructs the Spindle/Leadscrew objects and
// never starts SpindleTask or DisplayTask, and loop() calls wifiLoop() only
// while configMode is true. Every page served here is therefore served in AP
// config mode with no motion running at all - a factory reset cannot land
// mid-cut, and needs no equivalent of saveLathePreferences()'s under-power
// guard.
//
// If this ever gets called from the running machine, that stops being true:
// a flash erase takes the flash lock and disables the instruction cache on both
// cores, nothing in the motion path is IRAM_ATTR, and step generation would
// stop dead for tens of milliseconds. The erase would then need the same
// GlobalMotionMode guard saveLathePreferences() carries.
void startWebServer() {
    webServer = new WebServer(80);
    webServer->on("/", HTTP_GET, showPage);
    webServer->on("/set", HTTP_POST, setValues);
    webServer->on("/reset", HTTP_GET, reset);
    // Factory reset - two requests, and note how little it looks like /reset
    // above, which only reboots. The GET renders a confirmation page and issues
    // a one-shot token; the POST is the only thing that erases anything. A GET
    // on the confirm path matches no handler and falls through to
    // onNotFound(), i.e. back to the portal, which is what a reload after the
    // erase should do anyway.
    webServer->on("/factoryreset", HTTP_GET, showFactoryResetPage);
    webServer->on("/factoryreset/confirm", HTTP_POST, confirmFactoryReset);

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

