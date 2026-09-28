import math,sys,collections
src=open(sys.argv[1]).read()
def parse(s):
    i=0;n=len(s);stack=[[]]
    while i<n:
        c=s[i]
        if c=='(':stack.append([]);i+=1
        elif c==')':t=stack.pop();stack[-1].append(t);i+=1
        elif c.isspace():i+=1
        elif c=='"':
            j=i+1;buf=[]
            while s[j]!='"':
                if s[j]=='\\':j+=1
                buf.append(s[j]);j+=1
            stack[-1].append(''.join(buf));i=j+1
        else:
            j=i
            while j<n and not s[j].isspace() and s[j] not in '()':j+=1
            stack[-1].append(s[i:j]);i=j
    return stack[0][0]
T=parse(src)
def f(node,key):
    for x in node:
        if isinstance(x,list) and x and x[0]==key:return x
def fa(node,key):return [x for x in node if isinstance(x,list) and x and x[0]==key]
def netname(node):
    n=f(node,'net')
    if not n:return None
    return n[-1] if len(n)>1 else None
pads=[];segs=[];vias=[]
CU=['F.Cu','B.Cu']
for fp in fa(T,'footprint'):
    ref=[p[2] for p in fa(fp,'property') if p[1]=='Reference']
    ref=ref[0] if ref else '?'
    at=f(fp,'at');fx,fy=float(at[1]),float(at[2]);fr=float(at[3]) if len(at)>3 else 0
    th=math.radians(fr)
    for p in fa(fp,'pad'):
        pat=f(p,'at');px,py=float(pat[1]),float(pat[2]);pr=float(pat[3]) if len(pat)>3 else 0
        wx=fx+px*math.cos(th)+py*math.sin(th);wy=fy-px*math.sin(th)+py*math.cos(th)
        sz=f(p,'size');w,h=float(sz[1]),float(sz[2])
        ls=f(p,'layers')[1:]
        L=set()
        for l in ls:
            if l in ('*.Cu','F&B.Cu'):L|=set(CU)
            elif l in CU:L.add(l)
        nm=netname(p)
        if nm and L:pads.append(dict(ref=ref,num=p[1],x=wx,y=wy,w=w,h=h,a=pr,shape=p[3],L=L,net=nm))
for s in fa(T,'segment'):
    st=f(s,'start');en=f(s,'end')
    segs.append(dict(x1=float(st[1]),y1=float(st[2]),x2=float(en[1]),y2=float(en[2]),w=float(f(s,'width')[1]),L={f(s,'layer')[1]},net=netname(s)))
for v in fa(T,'via'):
    at=f(v,'at');vias.append(dict(x=float(at[1]),y=float(at[2]),d=float(f(v,'size')[1]),L=set(CU),net=netname(v)))
items=[('p',i) for i in range(len(pads))]+[('s',i) for i in range(len(segs))]+[('v',i) for i in range(len(vias))]
par={it:it for it in items}
def find(a):
    while par[a]!=a:par[a]=par[par[a]];a=par[a]
    return a
def uni(a,b):par[find(a)]=find(b)
def inpad(p,x,y,tol):
    dx,dy=x-p['x'],y-p['y'];t=math.radians(p['a'])
    lx=dx*math.cos(t)-dy*math.sin(t);ly=dx*math.sin(t)+dy*math.cos(t)
    if p['shape']=='circle':return math.hypot(lx,ly)<=p['w']/2+tol
    return abs(lx)<=p['w']/2+tol and abs(ly)<=p['h']/2+tol
def dseg(px,py,s):
    ax,ay,bx,by=s['x1'],s['y1'],s['x2'],s['y2'];dx,dy=bx-ax,by-ay;l=dx*dx+dy*dy
    t=0 if l==0 else max(0,min(1,((px-ax)*dx+(py-ay)*dy)/l))
    return math.hypot(px-ax-t*dx,py-ay-t*dy)
bynet=collections.defaultdict(list)
for it in items:
    d={'p':pads,'s':segs,'v':vias}[it[0]][it[1]]
    bynet[d['net']].append(it)
for net,its in bynet.items():
    P=[i for i in its if i[0]=='p'];S=[i for i in its if i[0]=='s'];V=[i for i in its if i[0]=='v']
    for a in S:
        sa=segs[a[1]]
        for b in S:
            if b<=a:continue
            sb=segs[b[1]]
            if not(sa['L']&sb['L']):continue
            tol=0.01
            if min(dseg(sa['x1'],sa['y1'],sb),dseg(sa['x2'],sa['y2'],sb),dseg(sb['x1'],sb['y1'],sa),dseg(sb['x2'],sb['y2'],sa))<=max(sa['w'],sb['w'])/2*0.5+tol:uni(a,b)
        for v in V:
            vv=vias[v[1]]
            if min(math.hypot(sa['x1']-vv['x'],sa['y1']-vv['y']),math.hypot(sa['x2']-vv['x'],sa['y2']-vv['y']))<=vv['d']/2:uni(a,v)
        for p in P:
            pp=pads[p[1]]
            if not(sa['L']&pp['L']):continue
            if inpad(pp,sa['x1'],sa['y1'],0.01) or inpad(pp,sa['x2'],sa['y2'],0.01):uni(a,p)
    for v in V:
        vv=vias[v[1]]
        for p in P:
            if inpad(pads[p[1]],vv['x'],vv['y'],0.01):uni(v,p)
tot=0;report=[]
for net,its in bynet.items():
    P=[i for i in its if i[0]=='p']
    if len(P)<2 or net.startswith('unconnected'):continue
    comps=collections.defaultdict(list)
    for p in P:comps[find(p)].append(pads[p[1]]['ref']+'.'+pads[p[1]]['num'])
    k=len(comps)-1
    if k:tot+=k;report.append((net,k,[v for v in comps.values()]))
nets=[n for n,its in bynet.items() if sum(1 for i in its if i[0]=='p')>=2 and not n.startswith('unconnected')]
print('pads',len(pads),'segs',len(segs),'vias',len(vias),'multi-pad nets',len(nets))
print('UNROUTED connections',tot,'in',len(report),'nets')
for net,k,c in sorted(report,key=lambda r:-r[1]):
    print(f'{net}: {k} missing ->',' | '.join(','.join(x[:4])+('...' if len(x)>4 else '') for x in c)[:300])
L=collections.Counter()
for s in segs:L[next(iter(s['L']))]+=math.hypot(s['x2']-s['x1'],s['y2']-s['y1'])
print('track length mm',{k:round(v) for k,v in L.items()})
