import re, os, shutil, configparser
# Deband is left out: in vkBasalt it smeared dark flat art (the title-screen emblems) into grainy smudges.
SRC=os.path.expanduser('~/.local/share/vkBasalt/mgs4/shaders')
BASE=os.path.expanduser('~/.local/share/vkBasalt/mgs4')
CONF=os.path.expanduser('~/.config/vkBasalt'); os.makedirs(CONF, exist_ok=True)
P=os.path.join(BASE,'presets')
presets={'lite':os.path.join(P,'MGS4_Clean_Modern_Lite.ini'),'normal':os.path.join(P,'MGS4_Clean_Modern.ini'),'strong':os.path.join(P,'MGS4_Clean_Modern_Strong.ini')}
order=[('Tonemap.fx','tonemap'),('LiftGammaGain.fx','liftgammagain'),('Curves.fx','curves'),('Vibrance.fx','vibrance'),('CAS.fx','cas'),('FakeHDR.fx','fakehdr')]
def patch(text, name, val):
    m=re.search(r'uniform\s+\w+\s+'+re.escape(name)+r'\b', text)
    if not m: raise SystemExit('missing uniform %s'%name)
    m2=re.search(r'>\s*=\s*([^;]+);', text[m.end():])
    s=m.end()+m2.start(1); e=m.end()+m2.end(1)
    typ=re.match(r'uniform\s+(\w+)', text[m.start():]).group(1)
    v=val.strip()
    if typ.startswith('float') and ',' in v: v='float%d(%s)'%(v.count(',')+1, v)
    elif typ=='bool': v='true' if v in ('1','true') else 'false'
    return text[:s]+v+text[e:]
for pname,ini in presets.items():
    raw=open(ini).read()
    i=raw.index('\n[')
    top,rest=raw[:i],raw[i+1:]
    tl=[l for l in top.splitlines() if l.startswith('Techniques=')][0]
    tech=[t.split('@')[1] for t in tl.split('=',1)[1].split(',')]
    cp=configparser.ConfigParser(); cp.optionxform=str; cp.read_string(rest)
    d=os.path.join(BASE,pname); os.makedirs(d, exist_ok=True)
    for f in ('ReShade.fxh','ReShadeUI.fxh'): shutil.copy(os.path.join(SRC,f), d)
    used=[]
    for fx,key in order:
        if fx not in tech: continue
        if fx=='CAS.fx':                              # vkBasalt's built-in CAS (the .fx needs newer ReShade)
            used.append((fx,key)); casSharp=cp.get(fx,'Sharpening'); continue
        t=open(os.path.join(SRC,fx)).read()
        if cp.has_section(fx):
            for k,v in cp.items(fx): t=patch(t,k,v)
        if 'BUFFER_COLOR_BIT_DEPTH' in t:              # vkBasalt only defines BUFFER_COLOR_DEPTH
            t=t.replace('#include "ReShadeUI.fxh"', '#include "ReShadeUI.fxh"\n#ifndef BUFFER_COLOR_BIT_DEPTH\n#define BUFFER_COLOR_BIT_DEPTH 8\n#endif', 1)
        open(os.path.join(d,fx),'w').write(t); used.append((fx,key))
    lines=['# MGS4 "Clean Modern" look (%s), ported from the MGS4 Clean Modern ReShade preset'%pname,
           '# to vkBasalt. Toggle with the Home key.',
           'effects = '+':'.join(k for _,k in used)]
    for fx,key in used:
        if key!='cas': lines.append('%s = "%s"'%(key, os.path.join(d,fx)))
    lines.append('casSharpness = %s'%casSharp)
    lines+=['reshadeIncludePath = "%s"'%d,'reshadeTexturePath = "%s"'%os.path.join(BASE,'textures'),
            'depthCapture = off','toggleKey = Home','enableOnLaunch = True']
    open(os.path.join(CONF,'mgs4-%s.conf'%pname),'w').write('\n'.join(lines)+'\n')
    print(pname, [k for _,k in used])
