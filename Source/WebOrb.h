#pragma once
// Μικρός web server για τον orb: σελίδα + WebSocket, ώστε να παίζεις τον orb από το Safari του iPad.
// Ο server γράφει/διαβάζει μόνο στα δικά του threads. Το Main του δίνει την κατάσταση (setState) και δέχεται μηνύματα (onMessage).
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace webOrb
{
using namespace juce;

// SHA-1 + base64 για το handshake του WebSocket
static inline String wsAccept (const String& key)
{
    const auto data = (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").toStdString();
    std::vector<uint8> msg (data.begin(), data.end());
    const uint64 ml = (uint64) msg.size() * 8;
    msg.push_back (0x80);
    while (msg.size() % 64 != 56) msg.push_back (0);
    for (int i = 7; i >= 0; --i) msg.push_back ((uint8) (ml >> (i * 8)));
    auto rol = [] (uint32 v, int s) { return (v << s) | (v >> (32 - s)); };
    uint32 h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
    for (size_t off = 0; off < msg.size(); off += 64)
    {
        uint32 w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32) msg[off + (size_t) i * 4] << 24) | ((uint32) msg[off + (size_t) i * 4 + 1] << 16)
                 | ((uint32) msg[off + (size_t) i * 4 + 2] << 8) | (uint32) msg[off + (size_t) i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32 a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i)
        {
            uint32 f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                      k = 0xCA62C1D6u; }
            const uint32 t = rol (a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol (b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint8 out[20];
    const uint32 hs[5] { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8) (hs[i] >> (24 - 8 * j));
    return Base64::toBase64 (out, 20);
}

static inline MemoryBlock wsFrame (const String& text, uint8 opcode = 0x1)
{
    MemoryBlock mb;
    const auto utf = text.toStdString();
    const size_t n = utf.size();
    const uint8 b0 = (uint8) (0x80 | opcode);
    mb.append (&b0, 1);
    if (n < 126) { const uint8 l = (uint8) n; mb.append (&l, 1); }
    else if (n < 65536) { const uint8 h[3] { 126, (uint8) (n >> 8), (uint8) n }; mb.append (h, 3); }
    else { uint8 h[9]; h[0] = 127; for (int i = 0; i < 8; ++i) h[1 + i] = (uint8) ((uint64) n >> (56 - 8 * i)); mb.append (h, 9); }
    if (n > 0) mb.append (utf.data(), n);
    return mb;
}

class Server : public Thread
{
public:
    Server() : Thread ("FlowOrbWeb") {}
    ~Server() override { stop(); }

    String token;
    int port = 0;
    std::function<void (const String&)> onMessage;       // καλείται από thread του server
    std::function<MemoryBlock()> getImage;                // JPEG της εικόνας του orb

    bool start (int firstPort)
    {
        for (int p = firstPort; p < firstPort + 10; ++p)
            if (listener.createListener (p))
            {
                port = p;
                startThread();
                return true;
            }
        return false;
    }

    void stop()
    {
        signalThreadShouldExit();
        stopThread (3000);
        OwnedArray<Client> old;
        {
            const ScopedLock sl (lock);
            old.swapWith (clients);
        }
        old.clear();   // έξω από το lock: τα threads των clients το παίρνουν καθώς τελειώνουν
        listener.close();
    }

    void setState (const String& json)
    {
        const ScopedLock sl (lock);
        state = json;
        ++stateVer;
    }

    int getNumClients() const { const ScopedLock sl (lock); return numWs; }

private:
    struct Client : public Thread
    {
        Client (Server& o, StreamingSocket* s) : Thread ("FlowOrbClient"), owner (o), sock (s) {}
        ~Client() override { signalThreadShouldExit(); stopThread (2000); }

        Server& owner;
        std::unique_ptr<StreamingSocket> sock;

        bool writeAll (const void* d, int n)
        {
            const auto* p = static_cast<const char*> (d);
            int done = 0;
            while (done < n)
            {
                if (threadShouldExit()) return false;
                const int k = sock->write (p + done, n - done);
                if (k <= 0) return false;
                done += k;
            }
            return true;
        }

        bool readExact (uint8* p, int n)
        {
            int got = 0, idle = 0;
            while (got < n)
            {
                if (threadShouldExit()) return false;
                const int r = sock->waitUntilReady (true, 100);
                if (r < 0) return false;
                if (r == 0) { if (++idle > 40) return false; continue; }
                idle = 0;
                const int k = sock->read (p + got, n - got, false);
                if (k <= 0) return false;
                got += k;
            }
            return true;
        }

        void respond (const String& status, const String& type, const void* body, int len)
        {
            const String head = "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + String (len)
                                + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
            const auto h = head.toStdString();
            writeAll (h.data(), (int) h.size());
            if (len > 0) writeAll (body, len);
        }

        void respondText (const String& status, const String& type, const String& body)
        {
            const auto b = body.toStdString();
            respond (status, type, b.data(), (int) b.size());
        }

        void run() override
        {
            // αίτημα HTTP
            String req;
            char buf[1024];
            int idle = 0;
            while (! threadShouldExit())
            {
                const int r = sock->waitUntilReady (true, 200);
                if (r < 0) return;
                if (r == 0) { if (++idle > 25) return; continue; }
                const int n = sock->read (buf, 1023, false);
                if (n <= 0) return;
                req += String::fromUTF8 (buf, n);
                if (req.contains ("\r\n\r\n")) break;
                if (req.length() > 8192) return;
            }
            const auto lines = StringArray::fromLines (req);
            if (lines.isEmpty()) return;
            const auto first = StringArray::fromTokens (lines[0], " ", "");
            if (first.size() < 2 || first[0] != "GET") { respondText ("405 Method Not Allowed", "text/plain", "no"); return; }
            const String target = first[1];
            const String path = target.upToFirstOccurrenceOf ("?", false, false);
            const String query = target.fromFirstOccurrenceOf ("?", false, false);
            bool authorised = false;
            for (auto& kv : StringArray::fromTokens (query, "&", ""))
                if (kv.startsWith ("k=") && kv.substring (2) == owner.token) authorised = true;
            String wsKey;
            bool upgrade = false;
            for (auto& l : lines)
            {
                if (l.startsWithIgnoreCase ("Sec-WebSocket-Key:")) wsKey = l.fromFirstOccurrenceOf (":", false, false).trim();
                if (l.startsWithIgnoreCase ("Upgrade:") && l.containsIgnoreCase ("websocket")) upgrade = true;
            }

            if (! authorised) { respondText ("403 Forbidden", "text/plain; charset=utf-8", "Flow orb: open the link shown in the Flow menu (INFO, iPad orb)."); return; }

            if (path == "/ws" && upgrade && wsKey.isNotEmpty())
            {
                const String head = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAccept (wsKey) + "\r\n\r\n";
                const auto h = head.toStdString();
                if (! writeAll (h.data(), (int) h.size())) return;
                runWebSocket();
                return;
            }
            if (path == "/img")
            {
                MemoryBlock img = owner.getImage ? owner.getImage() : MemoryBlock();
                if (img.getSize() == 0) respondText ("404 Not Found", "text/plain", "none");
                else respond ("200 OK", "image/jpeg", img.getData(), (int) img.getSize());
                return;
            }
            if (path == "/" || path == "/index.html") { respondText ("200 OK", "text/html; charset=utf-8", pageHtml()); return; }
            respondText ("404 Not Found", "text/plain", "not found");
        }

        void runWebSocket()
        {
            { const ScopedLock sl (owner.lock); ++owner.numWs; }
            uint32 sentVer = 0;
            while (! threadShouldExit() && sock->isConnected())
            {
                String s; uint32 v;
                { const ScopedLock sl (owner.lock); s = owner.state; v = owner.stateVer; }
                if (v != sentVer && s.isNotEmpty())
                {
                    sentVer = v;
                    const auto f = wsFrame (s);
                    if (! writeAll (f.getData(), (int) f.getSize())) break;
                }
                const int r = sock->waitUntilReady (true, 20);
                if (r < 0) break;
                if (r == 0) continue;

                uint8 hdr[2];
                if (! readExact (hdr, 2)) break;
                const int opcode = hdr[0] & 0x0F;
                const bool masked = (hdr[1] & 0x80) != 0;
                uint64 len = hdr[1] & 0x7F;
                if (len == 126) { uint8 e[2]; if (! readExact (e, 2)) break; len = ((uint64) e[0] << 8) | e[1]; }
                else if (len == 127) { uint8 e[8]; if (! readExact (e, 8)) break; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | e[i]; }
                if (len > 65536) break;
                uint8 mask[4] { 0, 0, 0, 0 };
                if (masked && ! readExact (mask, 4)) break;
                std::vector<uint8> payload ((size_t) len);
                if (len > 0 && ! readExact (payload.data(), (int) len)) break;
                if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];

                if (opcode == 0x8) break;
                if (opcode == 0x9) { const auto pong = wsFrame (String::fromUTF8 (reinterpret_cast<const char*> (payload.data()), (int) payload.size()), 0xA); writeAll (pong.getData(), (int) pong.getSize()); }
                else if (opcode == 0x1 && owner.onMessage) owner.onMessage (String::fromUTF8 (reinterpret_cast<const char*> (payload.data()), (int) payload.size()));
            }
            { const ScopedLock sl (owner.lock); --owner.numWs; }
        }
    };

    void run() override
    {
        while (! threadShouldExit())
        {
            const int r = listener.waitUntilReady (true, 200);
            if (r > 0)
                if (auto* s = listener.waitForNextConnection())
                {
                    auto* c = new Client (*this, s);
                    {
                        const ScopedLock sl (lock);
                        clients.add (c);
                    }
                    c->startThread();
                }
            // καθάρισε όσους τελείωσαν
            const ScopedLock sl (lock);
            for (int i = clients.size(); --i >= 0;)
                if (! clients.getUnchecked (i)->isThreadRunning()) clients.remove (i);
        }
    }

    static const char* pageHtml();

    StreamingSocket listener;
    mutable CriticalSection lock;
    OwnedArray<Client> clients;
    String state;
    uint32 stateVer = 0;
    int numWs = 0;
};

inline const char* Server::pageHtml()
{
    return R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Flow orb"><title>Flow orb</title>
<style>
html,body{margin:0;height:100%;background:#262220;overflow:hidden;touch-action:none;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none;overscroll-behavior:none}
canvas{position:fixed;left:0;top:0;width:100%;height:100%;display:block;touch-action:none}
#go{position:fixed;left:0;top:0;right:0;bottom:0;display:flex;align-items:center;justify-content:center;flex-direction:column;background:#262220;color:#e8e0d8;font:600 26px -apple-system,Helvetica,sans-serif;z-index:5}
#go small{font-weight:400;font-size:15px;opacity:.55;margin-top:12px}
</style></head><body>
<canvas id="c"></canvas>
<div id="go">Tap to start<small>full screen</small></div>
<script>
const tok=new URLSearchParams(location.search).get('k')||'';
const cv=document.getElementById('c'),g=cv.getContext('2d');
let W=0,H=0,DPR=1;
function fit(){DPR=window.devicePixelRatio||1;W=window.innerWidth;H=window.innerHeight;cv.width=Math.round(W*DPR);cv.height=Math.round(H*DPR);}
window.addEventListener('resize',fit);fit();
const COL=['#e8a06a','#7cc97f','#e578b8','#6f8fe8'];
let S={p:[[-.35,-.35],[.35,-.35],[.35,.35],[-.35,.35]],k:[],l:-1,r:0,m:0,ph:0,t:'ORB',iv:0};
let img=null,imgVer=-1,ws=null,connected=false,wl=null;
const lift=[0,0,0,0],drag={},lastSend=[0,0,0,0];
function geom(){return{cx:W/2,cy:H*0.5,R:Math.min(H*0.40,W*0.27)}}
function dot(c){const q=geom(),k=q.R-12;return[q.cx+S.p[c][0]*k,q.cy+S.p[c][1]*k]}
function send(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o))}
function connect(){
 ws=new WebSocket('ws://'+location.host+'/ws?k='+encodeURIComponent(tok));
 ws.onopen=function(){connected=true};
 ws.onclose=function(){connected=false;setTimeout(connect,1000)};
 ws.onerror=function(){};
 ws.onmessage=function(e){try{const s=JSON.parse(e.data);const op=S.p;S=s;Object.values(drag).forEach(function(c){S.p[c]=op[c]});
  if(s.iv!==imgVer){imgVer=s.iv;const i=new Image();i.onload=function(){img=i};i.onerror=function(){img=null};i.src='/img?k='+encodeURIComponent(tok)+'&v='+s.iv}}catch(x){}};
}
connect();
function btnRec(){return{x:W/2-110,y:H-62,w:220,h:46}}
function btnClr(){return{x:W/2+130,y:H-62,w:120,h:46}}
function inR(r,x,y){return x>=r.x&&x<=r.x+r.w&&y>=r.y&&y<=r.y+r.h}
function move(c,x,y,force){
 const q=geom(),k=q.R-12;let px=(x-q.cx)/k,py=(y-q.cy)/k;const l=Math.hypot(px,py);if(l>1){px/=l;py/=l}
 S.p[c]=[px,py];const now=performance.now();
 if(force||now-lastSend[c]>25){lastSend[c]=now;send({t:'move',c:c,x:px,y:py})}}
const cv2=cv;
cv2.addEventListener('pointerdown',function(e){
 e.preventDefault();
 if(inR(btnRec(),e.clientX,e.clientY)){send({t:'rec'});return}
 if(S.m&&inR(btnClr(),e.clientX,e.clientY)){send({t:'clear'});return}
 let best=-1,bd=56;
 for(let c=0;c<4;c++){const d=dot(c);const dd=Math.hypot(d[0]-e.clientX,d[1]-e.clientY);if(dd<bd){bd=dd;best=c}}
 if(best>=0){drag[e.pointerId]=best;try{cv2.setPointerCapture(e.pointerId)}catch(x){}
  send({t:'touch',c:best,d:1});move(best,e.clientX,e.clientY,true)}
},{passive:false});
cv2.addEventListener('pointermove',function(e){const c=drag[e.pointerId];if(c!==undefined){e.preventDefault();move(c,e.clientX,e.clientY,false)}},{passive:false});
function up(e){const c=drag[e.pointerId];if(c!==undefined){delete drag[e.pointerId];
 send({t:'move',c:c,x:S.p[c][0],y:S.p[c][1]});send({t:'touch',c:c,d:0})}}
cv2.addEventListener('pointerup',up);cv2.addEventListener('pointercancel',up);
document.addEventListener('touchmove',function(e){e.preventDefault()},{passive:false});
document.addEventListener('gesturestart',function(e){e.preventDefault()});
async function wake(){try{if(navigator.wakeLock)wl=await navigator.wakeLock.request('screen')}catch(x){}}
document.addEventListener('visibilitychange',function(){if(document.visibilityState==='visible'&&document.getElementById('go').style.display==='none')wake()});
document.getElementById('go').addEventListener('click',async function(){
 const el=document.documentElement;
 try{if(el.requestFullscreen)await el.requestFullscreen();else if(el.webkitRequestFullscreen)el.webkitRequestFullscreen()}catch(x){}
 wake();this.style.display='none';fit();
});
function rr(x,y,w,h,r){g.beginPath();g.moveTo(x+r,y);g.arcTo(x+w,y,x+w,y+h,r);g.arcTo(x+w,y+h,x,y+h,r);g.arcTo(x,y+h,x,y,r);g.arcTo(x,y,x+w,y,r);g.closePath()}
function draw(t){
 g.setTransform(DPR,0,0,DPR,0,0);
 g.fillStyle='#262220';g.fillRect(0,0,W,H);
 const q=geom(),cx=q.cx,cy=q.cy,R=q.R;
 g.font='600 '+Math.round(Math.max(13,H*0.026))+'px -apple-system,Helvetica,sans-serif';
 g.fillStyle='#a8a096';g.textAlign='center';g.textBaseline='middle';
 g.fillText(S.t||'ORB',cx,Math.max(28,cy-R-34));
 // orb
 g.save();g.beginPath();g.arc(cx,cy,R,0,Math.PI*2);g.clip();
 g.fillStyle='#1e1b19';g.fillRect(cx-R,cy-R,2*R,2*R);
 if(img){const s=Math.min(img.width,img.height);g.globalAlpha=0.92;g.drawImage(img,(img.width-s)/2,(img.height-s)/2,s,s,cx-R,cy-R,2*R,2*R);g.globalAlpha=1}
 else{g.fillStyle='#2e2a27';for(let y=-R;y<R;y+=7)for(let x=-R;x<R;x+=7)g.fillRect(cx+x,cy+y,1.4,1.4)}
 g.restore();
 g.strokeStyle='#4a423b';g.lineWidth=1.5;g.beginPath();g.arc(cx,cy,R,0,Math.PI*2);g.stroke();
 // λίστες
 const fs=Math.round(Math.max(13,H*0.03)),rowH=fs*1.9;
 g.font='600 '+fs+'px -apple-system,Helvetica,sans-serif';
 for(let c=0;c<4;c++){
  const left=(c===0||c===3),top=(c<2);
  const x=left?cx-R-Math.max(24,W*0.03):cx+R+Math.max(24,W*0.03);
  const y0=top?cy-R*0.78:cy+R*0.12;
  g.fillStyle=COL[c];g.beginPath();g.arc(x+(left?0:0),y0-rowH*0.8,6,0,Math.PI*2);g.fill();
  for(let r=0;r<4;r++){
   const k=S.k[c*4+r]||[0,0,''];const y=y0+rowH*r;
   g.textBaseline='middle';
   if(k[0]){
    const val=Math.round(k[1])+'%';
    g.textAlign=left?'right':'left';g.fillStyle='#f3f4f6';g.font='700 '+fs+'px -apple-system,Helvetica,sans-serif';
    if(left){g.fillText(val,x,y);g.font='400 '+Math.round(fs*0.85)+'px -apple-system,Helvetica,sans-serif';g.fillStyle='#8a8077';g.fillText(k[2],x-g.measureText(val).width-10-(fs*1.6),y)}
    else{g.fillText(val,x,y);const wv=g.measureText(val).width;g.font='400 '+Math.round(fs*0.85)+'px -apple-system,Helvetica,sans-serif';g.fillStyle='#8a8077';g.fillText(k[2],x+wv+10,y)}
   }else{g.textAlign=left?'right':'left';g.fillStyle='#4a423b';g.font='400 '+fs+'px -apple-system,Helvetica,sans-serif';g.fillText('—',x,y)}
  }
 }
 // κύκλοι που αιωρούνται
 for(let c=0;c<4;c++){
  let d=Object.values(drag).indexOf(c)>=0;
  lift[c]+=((d?1:0)-lift[c])*0.22;
  const p=dot(c),r=(d?1.15:1)*Math.max(15,R*0.085);
  const bob=0.5+0.5*Math.sin(t*0.0018+c*1.7);
  const h=(4+1.5*bob*(1-lift[c])+9*lift[c])*(R/120);
  const cxs=p[0]+h*0.35,cys=p[1]+h*1.4*(R/120)+4,rx=r*0.95,bl=(5+h*0.95);
  const a=Math.max(0.2,Math.min(0.85,0.86-h*0.02/(R/120)));
  g.save();g.translate(cxs,cys);g.scale(1,0.5);
  const gr=g.createRadialGradient(0,0,0,0,0,rx+bl);gr.addColorStop(0,'rgba(0,0,0,'+a+')');gr.addColorStop(1,'rgba(0,0,0,0)');
  g.fillStyle=gr;g.beginPath();g.arc(0,0,rx+bl,0,Math.PI*2);g.fill();g.restore();
  const py=p[1]-h*0.45;
  g.fillStyle='#faf8f4';g.beginPath();g.arc(p[0],py,r,0,Math.PI*2);g.fill();
 }
 // πρόοδος 16 μέτρων και κουμπιά
 const by=cy+R+Math.max(18,H*0.03);
 g.fillStyle='#3a3430';rr(cx-R,by,2*R,5,2.5);g.fill();
 g.fillStyle=S.r?'#e8837a':'#bdb6ad';rr(cx-R,by,Math.max(5,2*R*(S.ph||0)),5,2.5);g.fill();
 const b=btnRec();g.fillStyle=S.r?'#5a2f2c':'#3a332e';rr(b.x,b.y,b.w,b.h,12);g.fill();
 g.strokeStyle=S.r?'#e8837a':'#4a423b';g.lineWidth=2;rr(b.x,b.y,b.w,b.h,12);g.stroke();
 g.fillStyle=S.r?'#e8837a':'#e8e0d8';g.font='600 '+Math.round(fs*0.95)+'px -apple-system,Helvetica,sans-serif';g.textAlign='center';g.textBaseline='middle';
 g.fillText(S.r?'REC motion':'Rec motion',b.x+b.w/2,b.y+b.h/2);
 if(S.m){const c2=btnClr();g.fillStyle='#3a332e';rr(c2.x,c2.y,c2.w,c2.h,12);g.fill();g.fillStyle='#e8837a';g.fillText('✕ motion',c2.x+c2.w/2,c2.y+c2.h/2)}
 if(!connected){g.fillStyle='rgba(38,34,32,0.8)';g.fillRect(0,0,W,H);g.fillStyle='#e8e0d8';g.font='600 22px -apple-system,Helvetica,sans-serif';g.textAlign='center';g.fillText('connecting…',W/2,H/2)}
 requestAnimationFrame(draw);
}
requestAnimationFrame(draw);
</script></body></html>)HTML";
}
} // namespace webOrb
