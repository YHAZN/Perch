"""Inspect assembled and sliding-cover clearances in the active Fusion P1 prototype."""
import json
from pathlib import Path
from fusion_client import FusionClient

OUT = Path(__file__).resolve().parents[2] / "captures" / "case" / "prototype-p1"
SCRIPT = r'''
import adsk.core, adsk.fusion, json
def run(_context: str):
    app=adsk.core.Application.get()
    assert app.activeDocument.name.startswith('Enclosure P1'), 'Open P1 first'
    d=adsk.fusion.Design.cast(app.activeProduct)
    root=d.rootComponent
    lid=next(o for o in root.occurrences if o.component.name.startswith('PRINT 02'))
    before=lid.transform2
    def group(b):return b.assemblyContext.fullPathName.split('+')[0] if b.assemblyContext else 'root'
    def conflicts():
        bodies=adsk.core.ObjectCollection.create()
        for o in root.allOccurrences:
            for b in o.bRepBodies:
                if b.isVisible:bodies.add(b)
        result=d.analyzeInterference(d.createInterferenceInput(bodies))
        rows=[]
        for r in result:
            a,b=group(r.entityOne),group(r.entityTwo)
            if a==b or not (a.startswith('PRINT') or b.startswith('PRINT')):continue
            volume=r.interferenceBody.volume*1000
            if volume>0.00001:rows.append({'a':a,'b':b,'volume_mm3':round(volume,5)})
        return rows
    assembled=conflicts()
    movement=[]
    start_y=lid.boundingBox.minPoint.y*10
    for mm in (-5,-15,-30,-50,-65):
        pose=adsk.core.Matrix3D.create()
        pose.translation=adsk.core.Vector3D.create(0,mm/10,0)
        lid.transform2=pose
        d.computeAll()
        actual_y=lid.boundingBox.minPoint.y*10
        assert abs(actual_y-start_y-mm)<.01, 'Cover transform was not applied before analysis'
        rows=conflicts()
        movement.append({'cover_offset_mm':mm,'measured_cover_min_y_mm':round(actual_y,3),'conflicts':rows})
    lid.transform2=before
    d.computeAll()
    print(json.dumps({'assembled_conflicts':assembled,'cover_movement_samples':movement,
        'limitations':['Only provided solids checked. Ribbon, soldered wires, fasteners and dimensional tolerance are not fully modeled.',
        'Sampled cover positions are not a continuous swept-volume proof. Physical fit remains unverified.']}))
'''

if __name__ == "__main__":
    response = FusionClient().call("fusion_mcp_execute", {"featureType": "script", "object": {"script": SCRIPT}})
    payload = json.loads(response["content"][0]["text"])
    report = json.loads(payload["message"])
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "collision-checks.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report))
