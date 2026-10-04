import re,collections
t=open('/tmp/hex.net').read()
refs=re.findall(r'\(comp \(ref "([^"]+)"\)',t)
print("refs:",len(refs),"dupes:",[r for r,c in collections.Counter(refs).items() if c>1][:10],"unannotated:",[r for r in refs if '?' in r][:5])
nets={}
for m in re.finditer(r'\(net \(code "\d+"\) \(name "([^"]*)"\)(.*?)\)\s*(?=\(net |\)\s*\)\s*$)',t,re.S):
    nodes=re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)',m.group(2))
    nets[m.group(1)]=nodes
print("nets:",len(nets))
single=[(n,v) for n,v in nets.items() if len(v)==1]
print("single-node nets:",len(single))
for n,v in single[:40]: print("  ",n,v)
def show(name):
    for n,v in nets.items():
        if n==name or n.endswith("/"+name): print(n, sorted(v))
for nm in ["SER","SCK","RCLK","OE_N","KA","G_A1","G_C8","ISNS_A","VIN_S","+10V","SDA","SCL","VDD_IMU","+3V3"]:
    show(nm)
