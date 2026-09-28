#!/usr/bin/env python3
# Regenerate the XHyper Fuzz Watch dashboard (fuzzwatch.html) from current campaign state.
import re, os, glob, json, datetime
CAMP="/home/jose/xhyper-workspace/.fuzz-wip/s2-campaign"
OUT=os.path.join(CAMP,"fuzzwatch.html")

def parse_campaign(logpath, crashdir, manager_match):
    lines=open(logpath,errors="ignore").read().splitlines() if os.path.exists(logpath) else []
    pts=[]
    for ln in lines:
        m=re.search(r'(\d\d\d\d/\d\d/\d\d \d\d:\d\d:\d\d).*candidates=\d+ corpus=(\d+) coverage=(\d+) exec total=(\d+)',ln)
        if m: pts.append({"t":m.group(1),"corpus":int(m.group(2)),"cov":int(m.group(3)),"exec":int(m.group(4))})
    sampled=pts[::max(1,len(pts)//60)] if pts else []
    series=[{"x":i,"cov":p["cov"],"exec":p["exec"],"corpus":p["corpus"]} for i,p in enumerate(sampled)]
    last=pts[-1] if pts else {"cov":0,"corpus":0,"exec":0}
    ncrash=len([d for d in glob.glob(os.path.join(crashdir,"*")) if os.path.isdir(d)])
    running=bool(os.popen("ps -eo args | grep -q '%s' && echo 1"%manager_match).read().strip())
    return {"series":series,"last":last,"crashes":ncrash,"running":running}

# run123 = baseline S1+S2+S3 (primary chart); s4 = enriched /dev/gunyah model (doorbell/msgqueue + VM_START)
c123=parse_campaign(os.path.join(CAMP,"run123.log"),os.path.join(CAMP,"workdir-s123","crashes"),"[s]yz-manager.*s123")
S4="/home/jose/xhyper-workspace/.fuzz-wip/s4-campaign"
c4=parse_campaign(os.path.join(S4,"run-s4.log"),os.path.join(S4,"workdir-s4","crashes"),"[s]yz-manager.*s4.cfg")
series=c123["series"]; last=c123["last"]; ncrash=c123["crashes"]; running=c123["running"]
snap=datetime.datetime.now().strftime("%Y-%m-%d %H:%M (+08)")
data_js=json.dumps({"series":series,"last":last,"snapshot":snap,"crashes":ncrash,"running":running,
    "campaigns":[
        {"id":"run123","label":"S1+S2+S3 baseline","cov":c123["last"].get("cov",0),"corpus":c123["last"].get("corpus",0),"exec":c123["last"].get("exec",0),"crashes":c123["crashes"],"running":c123["running"]},
        {"id":"s4","label":"enriched /dev/gunyah (doorbell+msgqueue, VM_START)","cov":c4["last"].get("cov",0),"corpus":c4["last"].get("corpus",0),"exec":c4["last"].get("exec",0),"crashes":c4["crashes"],"running":c4["running"]},
    ]})

HTML=r'''<title>XHyper Fuzz Watch</title>
<style>
:root{--bg:#f4f6f5;--panel:#ffffff;--panel2:#eef1ef;--ink:#141a18;--muted:#5d6b64;--line:#d9e0dc;--accent:#2f9e63;--accent2:#1f7d8c;--warn:#c98a1e;--crit:#cc4c52;--el2:#7c4dc4;--el1:#c98a1e;color-scheme:light;}
:root:not([data-theme="light"]){@media (prefers-color-scheme:dark){--bg:#0e1413;--panel:#151d1b;--panel2:#1b2523;--ink:#e9efec;--muted:#93a49c;--line:#26332f;--accent:#41b878;--accent2:#3fb0c2;--warn:#e0a83a;--crit:#e5686e;--el2:#a983e6;--el1:#e0a83a;color-scheme:dark;}}
:root[data-theme="dark"]{--bg:#0e1413;--panel:#151d1b;--panel2:#1b2523;--ink:#e9efec;--muted:#93a49c;--line:#26332f;--accent:#41b878;--accent2:#3fb0c2;--warn:#e0a83a;--crit:#e5686e;--el2:#a983e6;--el1:#e0a83a;color-scheme:dark;}
*{box-sizing:border-box}
body{background:var(--bg);color:var(--ink);font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;}
.wrap{max-width:860px;margin:0 auto;padding:16px;padding-block:20px}
.mono{font-family:"JetBrains Mono",ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-variant-numeric:tabular-nums}
h1{font-size:1.35rem;margin:0;letter-spacing:-.01em}
.sub{color:var(--muted);font-size:.82rem;margin-top:3px}
.pill{display:inline-flex;align-items:center;gap:6px;font-size:.72rem;font-weight:600;padding:4px 10px;border-radius:999px;background:color-mix(in srgb,var(--accent) 18%,transparent);color:var(--accent);letter-spacing:.03em;text-transform:uppercase}
.pill.off{background:color-mix(in srgb,var(--muted) 18%,transparent);color:var(--muted)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--accent);animation:pulse 2.2s infinite}
.pill.off .dot{background:var(--muted);animation:none}
@keyframes pulse{0%{box-shadow:0 0 0 0 color-mix(in srgb,var(--accent) 50%,transparent)}70%{box-shadow:0 0 0 8px transparent}100%{box-shadow:0 0 0 0 transparent}}
@media (prefers-reduced-motion:reduce){.dot{animation:none}}
.head{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap}
.tiles{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-top:18px}
@media (max-width:560px){.tiles{grid-template-columns:repeat(2,1fr)}}
.tile{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px 13px}
.tile .k{font-size:.68rem;color:var(--muted);text-transform:uppercase;letter-spacing:.05em}
.tile .v{font-size:1.5rem;margin-top:4px;font-weight:600}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px;margin-top:16px}
.card h2{font-size:.95rem;margin:0 0 12px;letter-spacing:.01em}
canvas{width:100%;height:200px;display:block}
.chartmeta{display:flex;justify-content:space-between;color:var(--muted);font-size:.72rem;margin-top:6px}
.find{border-left:4px solid var(--line);padding:10px 12px;border-radius:0 10px 10px 0;background:var(--panel2);margin-top:10px}
.find.el2{border-left-color:var(--el2)} .find.el1{border-left-color:var(--el1)}
.find .ft{font-weight:600;font-size:.9rem;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.tag{font-size:.62rem;font-weight:700;padding:2px 7px;border-radius:6px;letter-spacing:.04em}
.tag.el2{background:color-mix(in srgb,var(--el2) 20%,transparent);color:var(--el2)}
.tag.el1{background:color-mix(in srgb,var(--el1) 20%,transparent);color:var(--el1)}
.tag.rep{background:color-mix(in srgb,var(--crit) 20%,transparent);color:var(--crit)}
.find .fd{color:var(--muted);font-size:.8rem;margin-top:5px;line-height:1.45}
.surf{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
@media (max-width:560px){.surf{grid-template-columns:1fr}}
.s{background:var(--panel2);border:1px solid var(--line);border-radius:10px;padding:11px 12px}
.s .n{font-weight:600;font-size:.86rem} .s .d{color:var(--muted);font-size:.76rem;margin-top:3px;line-height:1.4}
.ok{color:var(--accent);font-weight:700}
.note{color:var(--muted);font-size:.74rem;margin-top:18px;line-height:1.5;border-top:1px solid var(--line);padding-top:12px}
a{color:var(--accent2)}
</style>
<div class="wrap">
  <div class="head">
    <div><h1>XHyper Fuzz Watch</h1><div class="sub mono" id="snap"></div></div>
    <span class="pill" id="pill"><span class="dot"></span><span id="statustxt">Running</span> · S1+S2+S3</span>
  </div>
  <div class="tiles">
    <div class="tile"><div class="k">EL2 coverage</div><div class="v mono" id="cov">–</div></div>
    <div class="tile"><div class="k">Corpus</div><div class="v mono" id="corpus">–</div></div>
    <div class="tile"><div class="k">Programs run</div><div class="v mono" id="exec">–</div></div>
    <div class="tile"><div class="k">Crashes</div><div class="v mono" id="crashes">–</div></div>
  </div>
  <div class="card"><h2>Campaigns</h2><div id="camps"></div></div>
  <div class="card"><h2>EL2 coverage over time <span style="font-weight:400;color:var(--muted);font-size:.78rem">· S1+S2+S3 baseline</span></h2><canvas id="chart" width="820" height="200" aria-label="coverage chart"></canvas><div class="chartmeta mono"><span id="cmin"></span><span id="cmax"></span></div></div>
  <div class="card"><h2>Findings</h2>
    <div class="find el2"><div class="ft">F4 · Lent-memory ioctl copy_from_user &rarr; stage-2 external abort (host DoS) <span class="tag el2">EL2 root</span><span class="tag rep">new · s4</span></div><div class="fd">Found by the enriched /dev/gunyah model (lend variant + doorbell/msgqueue) minutes after deploy. The VM lends/donates host memory, then a later <span class="mono">gunyah_vm_ioctl</span> does <span class="mono">copy_from_user</span> on an arg pointer in the now-unmapped window &rarr; <span class="mono">XHYPER_HOST_EXIT_UNRESOLVED</span> (ipa 0x49e002xx) &rarr; host oops in <span class="mono">__arch_copy_from_user</span>. Same class as F3, distinct trigger/signature.</div></div>
    <div class="find el2"><div class="ft">F3 · Host memory-donation &rarr; stage-2 external abort (host DoS) <span class="tag el2">EL2 root</span><span class="tag rep">reproducible</span></div><div class="fd">Heavy <span class="mono">GH_VM_SET_USER_MEM_REGION</span> donations of in-use host memory + <span class="mono">GH_VM_START</span>: XHyper drops the pages from the host stage-2, a later host access faults unresolvably (<span class="mono">XHYPER_HOST_EXIT_UNRESOLVED</span>) &rarr; host oops in strncpy_from_user. Re-seeding re-triggered the identical crash.</div></div>
    <div class="find el2"><div class="ft">F2 · Hypercall error return leaks callee-saved regs <span class="tag el2">EL2 root</span></div><div class="fd">On a malformed host HVC, XHyper's error return leaves internal values in guest x19&ndash;x28 instead of restoring/sanitizing them as C Gunyah does &mdash; ABI deviation + potential EL2&rarr;EL1 leak.</div></div>
    <div class="find el1"><div class="ft">F1 · gunyah-driver crashes <span class="tag el1">EL1 host</span></div><div class="fd">Earlier S1 run: Internal error in <span class="mono">gh_vm_ioctl</span> (via GH_VM_REMOVE_FUNCTION) and <span class="mono">eventfd_release</span> null-deref. Guest-kernel driver side, XHyper healthy.</div></div>
  </div>
  <div class="card"><h2>Input surfaces</h2><div class="surf">
    <div class="s"><div class="n">S1 <span class="ok">&check;</span></div><div class="d">/dev/gunyah VM-lifecycle ioctls (well-formed driver path).</div></div>
    <div class="s"><div class="n">S2 <span class="ok">&check;</span></div><div class="d">Raw host HVC injector &mdash; arbitrary imm 0x6000&ndash;0x60ff + x0&ndash;x7 to the root table.</div></div>
    <div class="s"><div class="n">S3 <span class="ok">&check;</span></div><div class="d">Guest-internal SYZOS execution via /dev/gunyah &mdash; verified reaching EL2 (0&rarr;3794 solo).</div></div>
  </div></div>
  <div class="note" id="note"></div>
</div>
<script>
var D=__DATA__;
function fmt(n){return (n==null?'\u2013':n.toLocaleString('en-US'))}
document.getElementById('snap').textContent='snapshot '+D.snapshot+' \u00b7 auto-refresh 30 min';
document.getElementById('cov').textContent=fmt(D.last.cov);
document.getElementById('corpus').textContent=fmt(D.last.corpus);
document.getElementById('exec').textContent=fmt(D.last.exec);
document.getElementById('crashes').textContent=fmt(D.crashes);
if(!D.running){var p=document.getElementById('pill');p.className='pill off';document.getElementById('statustxt').textContent='Stopped';}
(function(){var el=document.getElementById('camps');if(!el||!D.campaigns)return;var h='';D.campaigns.forEach(function(c){var st=c.running?'<span class="ok">running</span>':'<span style="color:var(--muted)">stopped</span>';h+='<div class="s" style="margin-top:8px"><div class="n">'+c.label+' &middot; '+st+'</div><div class="d mono">cov '+fmt(c.cov)+' &middot; corpus '+fmt(c.corpus)+' &middot; run '+fmt(c.exec)+' &middot; crashes '+fmt(c.crashes)+'</div></div>';});el.innerHTML=h;})();
document.getElementById('note').innerHTML='Periodically-refreshed snapshot baked into the page (auto-refreshed ~every 30 min) \u2014 not a live proxy of syz-manager, which Claude artifacts cannot reach. For the live interactive dashboard run a tunnel yourself: <span class="mono">npx localtunnel --port 56760</span>.';
(function(){var c=document.getElementById('chart'),ctx=c.getContext('2d');
function css(v){return getComputedStyle(document.documentElement).getPropertyValue(v).trim()}
function draw(){var dpr=window.devicePixelRatio||1,W=c.clientWidth,H=200;c.width=W*dpr;c.height=H*dpr;ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,W,H);
var s=D.series.filter(function(p){return p.cov>0});if(!s.length){ctx.fillStyle=css('--muted');ctx.font='13px system-ui';ctx.fillText('warming up\u2026',10,24);return;}
var pL=8,pR=8,pT=12,pB=8,covs=s.map(function(p){return p.cov});var mn=Math.min.apply(null,covs),mx=Math.max.apply(null,covs);if(mx===mn)mx=mn+1;
var x=function(i){return pL+(W-pL-pR)*(i/(s.length-1||1))},y=function(v){return pT+(H-pT-pB)*(1-(v-mn)/(mx-mn))};
ctx.strokeStyle=css('--line');ctx.lineWidth=1;for(var g=0;g<=3;g++){var gy=pT+(H-pT-pB)*g/3;ctx.beginPath();ctx.moveTo(pL,gy);ctx.lineTo(W-pR,gy);ctx.stroke();}
var acc=css('--accent');ctx.beginPath();ctx.moveTo(x(0),y(s[0].cov));for(var i=1;i<s.length;i++)ctx.lineTo(x(i),y(s[i].cov));ctx.lineTo(x(s.length-1),H-pB);ctx.lineTo(x(0),H-pB);ctx.closePath();ctx.globalAlpha=.14;ctx.fillStyle=acc;ctx.fill();ctx.globalAlpha=1;
ctx.beginPath();ctx.moveTo(x(0),y(s[0].cov));for(var j=1;j<s.length;j++)ctx.lineTo(x(j),y(s[j].cov));ctx.strokeStyle=acc;ctx.lineWidth=2;ctx.stroke();
ctx.fillStyle=acc;ctx.beginPath();ctx.arc(x(s.length-1),y(s[s.length-1].cov),3.5,0,7);ctx.fill();
document.getElementById('cmin').textContent=mn.toLocaleString('en-US')+' blocks';document.getElementById('cmax').textContent='peak '+mx.toLocaleString('en-US');}
draw();window.addEventListener('resize',draw);var mq=window.matchMedia('(prefers-color-scheme:dark)');mq.addEventListener&&mq.addEventListener('change',draw);})();
</script>'''
open(OUT,"w").write(HTML.replace("__DATA__",data_js))
print("refreshed",OUT,"| cov=%s corpus=%s exec=%s crashes=%d running=%s"%(last.get("cov"),last.get("corpus"),last.get("exec"),ncrash,running))
