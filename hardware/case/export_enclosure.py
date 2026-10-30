"""Export the active Fusion fit prototype and check its printable meshes."""
import json
import re
import struct
from collections import Counter
from pathlib import Path

from fusion_client import FusionClient

OUT = Path(__file__).resolve().parents[2] / 'captures/case/prototype-p1'
SCRIPT = r'''
import adsk.core, adsk.fusion, json, os, re
def run(_context: str):
    app=adsk.core.Application.get()
    assert app.activeDocument.name.startswith('Enclosure P1')
    d=adsk.fusion.Design.cast(app.activeProduct)
    root=d.rootComponent
    lib=app.materialLibraries.itemByName('Fusion Appearance Library')
    appearance=d.appearances.itemByName('Graphite prototype') or d.appearances.addByCopy(lib.appearances.itemByName('Plastic - Matte (Black)'), 'Graphite prototype')
    parts=[]
    for o in root.occurrences:
        if not o.component.name.startswith('PRINT'):continue
        was_visible=o.isLightBulbOn
        o.isLightBulbOn=True
        c=o.component
        for b in c.bRepBodies:b.appearance=appearance
        name=re.sub('[^a-z0-9.-]+','-',c.name.lower()).strip('-')
        assert c.bRepBodies.count==1
        b=c.bRepBodies.item(0)
        assert b.isSolid and b.volume>0
        bb=b.boundingBox
        dims=[(getattr(bb.maxPoint,a)-getattr(bb.minPoint,a))*10 for a in ('x','y','z')]
        opts=d.exportManager.createSTLExportOptions(c,os.path.join(OUTPUT,name+'.stl'))
        opts.unitType=adsk.fusion.DistanceUnits.MillimeterDistanceUnits
        opts.isBinaryFormat=True
        opts.meshRefinement=adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        assert d.exportManager.execute(opts)
        o.isLightBulbOn=was_visible
        parts.append({'file':name+'.stl','dimensions_mm':dims,'volume_mm3':b.volume*1000})
    assert d.exportManager.execute(d.exportManager.createFusionArchiveExportOptions(os.path.join(OUTPUT,'enclosure-p1.f3d')))
    assert d.exportManager.execute(d.exportManager.createSTEPExportOptions(os.path.join(OUTPUT,'enclosure-p1.step')))
    app.activeViewport.fit()
    print(json.dumps({'parts':parts}))
'''


def validate_stl(path, expected):
    data=bytearray(path.read_bytes())
    count=struct.unpack_from('<I',data,80)[0]
    assert len(data)==84+50*count
    triangles=[]
    for i in range(count):
        xyz=struct.unpack_from('<9f',data,84+i*50+12)
        triangles.append([tuple(xyz[j:j+3]) for j in (0,3,6)])
    vertices=[v for t in triangles for v in t]
    low=[min(v[a] for v in vertices) for a in range(3)]
    high=[max(v[a] for v in vertices) for a in range(3)]
    dims=[high[a]-low[a] for a in range(3)]
    assert all(abs(a-b)<.02 for a,b in zip(dims,expected)), (path.name,dims,expected)
    edges=Counter()
    adjacency={}
    for t in triangles:
        q=[tuple(round(x,4) for x in v) for v in t]
        for a,b in ((0,1),(1,2),(2,0)):
            edges[tuple(sorted((q[a],q[b])))]+=1
            adjacency.setdefault(q[a],set()).add(q[b])
            adjacency.setdefault(q[b],set()).add(q[a])
    bad=sum(n!=2 for n in edges.values())
    assert bad==0,(path.name,'non-manifold edges',bad)
    remaining=set(adjacency)
    components=0
    while remaining:
        components+=1
        pending=[remaining.pop()]
        while pending:
            for v in adjacency[pending.pop()]:
                if v in remaining:
                    remaining.remove(v)
                    pending.append(v)
    assert components==1,(path.name,'disconnected mesh components',components)
    # Center XY and place the lowest surface at Z=0 for slicer import.
    shift=[(low[0]+high[0])/2,(low[1]+high[1])/2,low[2]]
    for i,t in enumerate(triangles):
        flat=[v[a]-shift[a] for v in t for a in range(3)]
        struct.pack_into('<9f',data,84+i*50+12,*flat)
    path.write_bytes(data)
    return {'file':path.name,'triangles':count,'dimensions_mm':dims,'non_manifold_edges':bad,'connected_components':components}


if __name__=='__main__':
    OUT.mkdir(parents=True,exist_ok=True)
    client=FusionClient()
    result=client.call('fusion_mcp_execute',{'featureType':'script','object':{'script':SCRIPT.replace('OUTPUT',repr(str(OUT)))}})
    report=json.loads(json.loads(result['content'][0]['text'])['message'])
    report['mesh_checks']=[validate_stl(OUT/p['file'],p['dimensions_mm']) for p in report['parts']]
    (OUT/'export-checks.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))
