import re
LIB="/usr/share/kicad/symbols/"
def block(text, start):
    d=0
    for i in range(start,len(text)):
        c=text[i]
        if c=='(': d+=1
        elif c==')':
            d-=1
            if d==0: return text[start:i+1]
    raise ValueError
_cache={}
def libtext(lib):
    if lib not in _cache: _cache[lib]=open(LIB+lib+".kicad_sym").read()
    return _cache[lib]
def get_symbol(lib,name,as_name=None,newlib=None):
    t=libtext(lib)
    i=t.index(f'(symbol "{name}"')
    b=block(t,i)
    m=re.search(r'\(extends "([^"]+)"\)',b[:200])
    if m:
        base=m.group(1)
        bb=block(t,t.index('(symbol "'+base+'"'))
        # take base graphics/pins, keep derived name
        b=bb.replace(f'(symbol "{base}_', f'(symbol "{name}_')
        b=b.replace(f'(symbol "{base}"', f'(symbol "{name}"',1)
    full=f'{newlib or lib}:{as_name or name}'
    nm=as_name or name
    b=b.replace(f'(symbol "{name}_', f'(symbol "{nm}_').replace(f'(symbol "{name}"', f'(symbol "{full}"',1)
    pins={}
    for pm in re.finditer(r'\(pin (\w+) \w+ \(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\).*?\(name "([^"]*)".*?\(number "([^"]*)"',b,re.S):
        pins[pm.group(6)]=dict(type=pm.group(1),x=float(pm.group(2)),y=float(pm.group(3)),a=float(pm.group(4)),name=pm.group(5))
    return b,pins
