"""Create a separate Fusion fit study from the manufacturer's screen STEP and existing board CAD.

Run with the local Python interpreter while Fusion's official MCP server is enabled.
Does not edit or save over the existing cloud designs. No battery or enclosure size is assumed.
"""
import json
from pathlib import Path

from fusion_client import FusionClient

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "captures" / "case"
SCREEN = next((OUT / "vendor").rglob("*.stp"))
BOARD = OUT / "xiao-existing-reference.step"

SCRIPT = r'''
import adsk.core, adsk.fusion, json, math

def run(_context: str):
    app = adsk.core.Application.get()
    old = next(item for item in app.documents if item.name == 'AI Companion Device')
    original = adsk.fusion.Design.cast(old.products.itemByProductType('DesignProductType'))
    source_pose = original.rootComponent.occurrences.item(0).transform2.asArray()
    doc = app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    doc.name = 'Enclosure fit study - actual component references'
    design = adsk.fusion.Design.cast(app.activeProduct)
    design.designType = adsk.fusion.DesignTypes.DirectDesignType
    root = design.rootComponent
    screen = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    screen.component.name = 'Waveshare 1.83 touch - official STEP - check hardware revision'
    assert app.importManager.importToTarget(app.importManager.createSTEPImportOptions(SCREEN_PATH), screen.component)
    screen_transform = adsk.core.Matrix3D.create()
    screen_transform.translation = adsk.core.Vector3D.create(0, 0, .22222669047067316)
    screen.transform2 = screen_transform
    board = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    board.component.name = 'Existing XIAO Sense - OLD CAMERA - solder not included'
    assert app.importManager.importToTarget(app.importManager.createSTEPImportOptions(BOARD_PATH), board.component)
    # Front glass lies at Z=0. Camera points toward the back. Position is a fit study,
    # not a verified mounted assembly; exact solid interference is checked separately.
    transform = adsk.core.Matrix3D.create()
    transform.setToRotation(math.pi, adsk.core.Vector3D.create(1,0,0), adsk.core.Point3D.create(0,0,0))
    transform.translation = adsk.core.Vector3D.create(0,0,1.2332)
    # STEP exports a component in its local coordinates. Restore the source
    # occurrence pose before applying the intended assembly placement.
    desired = transform.asArray()
    combined = [sum(desired[i*4+k]*source_pose[k*4+j] for k in range(4)) for i in range(4) for j in range(4)]
    transform.setWithArray(combined)
    board.transform2 = transform
    screen.transform2 = screen_transform
    design.computeAll()
    app.activeViewport.fit()
    design.attributes.add('FitStudy', 'Status', 'No final case. New camera, solder and battery remain unverified.')
    print(json.dumps({'document':doc.name,'fit_verified':False}))
'''


if __name__ == "__main__":
    script = SCRIPT.replace("SCREEN_PATH", repr(str(SCREEN))).replace("BOARD_PATH", repr(str(BOARD)))
    script = script.replace("OUTPUT_PATH", repr(str(OUT / "component-fit-study.f3d")))
    client = FusionClient()
    result = client.call("fusion_mcp_execute", {"featureType": "script", "object": {"script": script}})
    print(json.dumps(result, ensure_ascii=True))
    # Finish the modeling transaction before inspecting transformed bounds/exporting.
    verify = '''import adsk.core, adsk.fusion, json
def run(_context: str):
    app=adsk.core.Application.get()
    design=adsk.fusion.Design.cast(app.activeProduct)
    rows=[{'name':o.name,'bounds_mm':[[v*10 for v in o.boundingBox.minPoint.asArray()],[v*10 for v in o.boundingBox.maxPoint.asArray()]]} for o in design.rootComponent.occurrences]
    assert len(rows)==2
    assert abs(rows[0]['bounds_mm'][0][2]) < .03, 'Display front must lie at Z=0'
    assert abs(rows[1]['bounds_mm'][0][2]-7.322) < .03, 'Board pose differs from the intended placement'
    assert design.exportManager.execute(design.exportManager.createFusionArchiveExportOptions(OUTPUT_PATH))
    print(json.dumps({'components':rows,'file':OUTPUT_PATH,'fit_verified':False}))
'''.replace("OUTPUT_PATH", repr(str(OUT / "component-fit-study.f3d")))
    print(json.dumps(client.call("fusion_mcp_execute", {"featureType": "script", "object": {"script": verify}}), ensure_ascii=True))
