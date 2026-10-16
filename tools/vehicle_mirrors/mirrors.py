import os
import sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wft import parse
from img3 import toc
def tris(g):
    v=g['verts']; I=g['idx']
    for t in range(0,len(I)-2,3):
        a,b,c=[v[I[t+k]] for k in range(3)]
        ux,uy,uz=b[0]-a[0],b[1]-a[1],b[2]-a[2]; wx,wy,wz=c[0]-a[0],c[1]-a[1],c[2]-a[2]
        nx,ny,nz=uy*wz-uz*wy,uz*wx-ux*wz,ux*wy-uy*wx; L=math.sqrt(nx*nx+ny*ny+nz*nz)
        if L<1e-9: continue
        yield (nx/L,ny/L,nz/L),(a,b,c),L/2
def names_of(bn,i): return bn[i][0] if i<len(bn) else '?'
def find(name):
    bn,gs=parse(name)
    names=[b[0] for b in bn]
    res={}
    gl=[q for g in gs if g['shader'][0]=='gta_vehicle_vehglass' for q in g['verts']]
    gzmin=min(q[2] for q in gl); gzmax=max(q[2] for q in gl); gxmax=max(abs(q[0]) for q in gl)
    ws=[q for g in gs if g['shader'][0]=='gta_vehicle_vehglass' for q in g['verts'] if q[3] is not None and names_of(bn,q[3])=='windscreen']
    wymax=max(q[1] for q in ws) if ws else 9; wymin=min(q[1] for q in ws) if ws else -9
    for side,bone in (('L','door_dside_f'),('R','door_pside_f'),('C',None)):
        best=None
        for gi,g in enumerate(gs):
            if g['shader'][0] in ('gta_vehicle_vehglass','gta_vehicle_lightsemissive','gta_vehicle_tire','gta_vehicle_shuts'): continue
            groups={}
            for n,(a,b,c),area in tris(g):
                if n[1] > -0.8: continue
                bones={p[3] for p in (a,b,c)}
                cx=(a[0]+b[0]+c[0])/3; cy=(a[1]+b[1]+c[1])/3; cz=(a[2]+b[2]+c[2])/3
                if bone:
                    if not all(bi is not None and names[bi] in (bone,'chassis','bodyshell') for bi in bones): continue
                    if (cx<0)!=(side=='L'): continue
                    if cz < gzmin-0.08 or abs(cx) < gxmax-0.04 or not (wymin-0.5 < cy < wymax+0.6): continue
                else:
                    if abs(cx)>0.25 or cz<gzmax-0.3 or not (wymin-0.3<cy<wymax+0.1): continue
                    if not all(bi is not None and names[bi] in ('chassis','bodyshell','windscreen') for bi in bones): continue
                # plane key: quantised normal + distance
                dd=n[0]*a[0]+n[1]*a[1]+n[2]*a[2]
                key=(round(n[0]*20),round(n[1]*20),round(n[2]*20),round(dd*50))
                gr=groups.setdefault(key,[0,[],n,dd]); gr[0]+=area; gr[1]+= [a,b,c]
            for key,(area,pts,n,dd) in groups.items():
                if area<0.002 or area>0.1: continue
                if side!='C' and sum(q[2] for q in pts)/len(pts) < gzmin: continue
                if best is None or area>best[0]: best=(area,pts,n,dd,gi,g['shader'][0])
        if best:
            area,pts,n,dd,gi,sh=best
            xs=[p[0] for p in pts]; ys=[p[1] for p in pts]; zs=[p[2] for p in pts]
            res[side]=(area,n,(min(xs),max(xs),min(ys),max(ys),min(zs),max(zs)),gi,sh)
    return res
if __name__=='__main__':
    names=[e[4] for e in toc(os.environ.get('VEHICLES_IMG', 'log/vehicles.img')) if e[4].endswith('.wft')]
    if len(sys.argv)>1: names=sys.argv[1:]
    for nm in names:
        try: r=find(nm)
        except Exception as ex: print(nm,'ERR',ex); continue
        line=nm
        for s in 'LRC':
            if s in r:
                a,n,bb,gi,sh=r[s]; line+='  %s: %.0fcm2 n(%.2f %.2f %.2f) x%.2f..%.2f y%.2f..%.2f z%.2f..%.2f g%d %s'%((s,a*1e4)+n+bb+(gi,sh[12:]))
            else: line+='  %s: -'%s
        print(line)
