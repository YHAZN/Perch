"""Add measured battery/camera envelopes to the separate Fusion reference study.

The component positions are exploded for inspection, not a final packed layout.
The original cloud designs are never edited. Native Fusion archive is exported locally.
"""
import json
from pathlib import Path
from fusion_client import FusionClient

ROOT = Path(__file__).resolve().parents[2]
dimensions = json.loads(Path(__file__).with_name("current_hardware.json").read_text())
script = '''import adsk.core, adsk.fusion, json
def run(_context: str):
    app=adsk.core.Application.get()
    assert app.activeDocument.name.startswith('Enclosure fit study'), 'Open the separate reference study first'
    design=adsk.fusion.Design.cast(app.activeProduct)
    root=design.rootComponent
    assert not any(o.component.name.startswith('Current 503450') for o in root.occurrences), 'References already exist'
    manager=adsk.fusion.TemporaryBRepManager.get()
    data=DIMENSIONS
    def component(name, position):
        matrix=adsk.core.Matrix3D.create()
        matrix.translation=adsk.core.Vector3D.create(*[v/10 for v in position])
        item=root.occurrences.addNewComponent(matrix)
        item.component.name=name
        return item.component
    def solid(comp, name, size, center):
        box=adsk.core.OrientedBoundingBox3D.create(adsk.core.Point3D.create(*[v/10 for v in center]),
            adsk.core.Vector3D.create(1,0,0),adsk.core.Vector3D.create(0,1,0),*[v/10 for v in size])
        body=comp.bRepBodies.add(manager.createBox(box))
        body.name=name
        return body
    battery=component('Current 503450 - 1000mAh - confirmed baseline', (42,0,10))
    w,l,h=data['battery']['size']
    solid(battery,'Existing battery pouch - nominal size',(w,l,h),(0,0,h/2))
    camera=component('New camera - measured package envelopes',(-27,0,5))
    hs=data['camera']['heatsink_size']
    head=data['camera']['head_size']
    solid(camera,'Glued heatsink - retain for current prototype',hs,(0,0,hs[2]/2))
    solid(camera,'Camera head - package envelope, no invented lens detail',head,(0,0,hs[2]+head[2]/2))
    camera.attributes.add('Measurements','Ribbon',json.dumps({'width':data['camera']['ribbon_width'],
        'free_length':data['camera']['ribbon_free_length'],'minimum_bend_radius':None,'route_verified':False}))
    for o in root.allOccurrences:
        if o.component.name.lower().startswith('camer module'):
            o.isLightBulbOn=False
    design.attributes.add('FitStudy','Hardware',json.dumps(data))
    app.activeViewport.fit()
    print(json.dumps({'battery_size_mm':data['battery']['size'],'camera_head_mm':head,
        'heatsink_mm':hs,'layout':'exploded dimensions only','fit_verified':False}))
'''.replace("DIMENSIONS", repr(dimensions))

if __name__ == "__main__":
    client = FusionClient()
    result = client.call("fusion_mcp_execute", {"featureType": "script", "object": {"script": script}})
    print(json.dumps(result, ensure_ascii=True))
    # Export after the modeling transaction has finished.
    target = ROOT / "captures" / "case" / "current-hardware-study.f3d"
    export = '''import adsk.core, adsk.fusion
def run(_context: str):
    design=adsk.fusion.Design.cast(adsk.core.Application.get().activeProduct)
    assert design.exportManager.execute(design.exportManager.createFusionArchiveExportOptions(OUTPUT))
    print('Exported current hardware references')
'''.replace("OUTPUT", repr(str(target)))
    print(json.dumps(client.call("fusion_mcp_execute", {"featureType": "script", "object": {"script": export}})))
