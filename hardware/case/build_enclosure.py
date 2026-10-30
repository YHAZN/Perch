"""Generate the current-battery enclosure prototype through Autodesk Fusion's local MCP.

All dimensions below are millimetres. Native solids, STEP, STL and a Fusion archive
are exported. This is an unsliced fit prototype, not an assertion of physical fit.
"""
import json
from pathlib import Path
from fusion_client import FusionClient

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "captures" / "case" / "prototype-p1"
OUT.mkdir(parents=True, exist_ok=True)
REFERENCE = ROOT / "captures" / "case" / "current-hardware-study.f3d"

BUILD = r'''
import adsk.core, adsk.fusion, json, math

def run(_context: str):
    app=adsk.core.Application.get()
    doc=app.importManager.importToNewDocument(app.importManager.createFusionArchiveImportOptions(REFERENCE_PATH))
    doc.name='Enclosure P1 - current battery - physical fit prototype'
    d=adsk.fusion.Design.cast(app.activeProduct)
    d.designType=adsk.fusion.DesignTypes.DirectDesignType
    root=d.rootComponent
    manager=adsk.fusion.TemporaryBRepManager.get()
    union=adsk.fusion.BooleanTypes.UnionBooleanType
    difference=adsk.fusion.BooleanTypes.DifferenceBooleanType
    def point(x,y,z):return adsk.core.Point3D.create(x/10,y/10,z/10)
    def box(w,l,h,x=0,y=0,z=0):
        obb=adsk.core.OrientedBoundingBox3D.create(point(x,y,z),adsk.core.Vector3D.create(1,0,0),
             adsk.core.Vector3D.create(0,1,0),w/10,l/10,h/10)
        return manager.createBox(obb)
    def cylinder(radius,z0,z1,x=0,y=0):
        return manager.createCylinderOrCone(point(x,y,z0),radius/10,point(x,y,z1),radius/10)
    def combine(a,b,kind=union):
        assert manager.booleanOperation(a,b,kind), 'Boolean failed'
        return a
    def rr(w,l,r,z0,z1,x=0,y=0):
        a=box(w-2*r,l,z1-z0,x,y,(z0+z1)/2)
        combine(a,box(w,l-2*r,z1-z0,x,y,(z0+z1)/2))
        for dx in (-w/2+r,w/2-r):
            for dy in (-l/2+r,l/2-r):combine(a,cylinder(r,z0,z1,x+dx,y+dy))
        return a
    def cut(a,b):return combine(a,b,difference)
    def component(name):
        o=root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
        o.component.name=name
        return o.component
    def add(comp,name,solid):
        assert solid and solid.isSolid, name+' is not solid'
        b=comp.bRepBodies.add(solid)
        b.name=name
        return b
    # Repack references. USB and microSD face the bottom service opening.
    for o in list(root.occurrences):
        m=o.transform2
        if o.component.name.startswith('Existing XIAO'):
            t=m.translation;t.y-=1.95;m.translation=t;o.transform2=m
        elif o.component.name.startswith('Waveshare'):
            t=m.translation;t.z+=.025;m.translation=t;o.transform2=m
        elif o.component.name.startswith('Current 503450'):
            m=adsk.core.Matrix3D.create();m.translation=adsk.core.Vector3D.create(0,-.55,1.73);o.transform2=m
        elif o.component.name.startswith('New camera'):
            m=adsk.core.Matrix3D.create();m.translation=adsk.core.Vector3D.create(0,2.85,.953);o.transform2=m
    case=component('PRINT 01 - housing')
    shell=rr(40.4,56.5,6,-.8,25.4,y=-5.5)
    combine(shell,rr(22,16,4,-.8,25.4,y=28.5))
    cut(shell,rr(37.2,53.3,4.4,0,27,y=-5.5))
    cut(shell,rr(18.8,12.8,2.4,1.0,27,y=28.5))
    cut(shell,box(8,6,11,0,21.5,18))
    cut(shell,rr(30.8,36.1,4.5,-2,1.0,y=0))
    # Shared service port: the real USB and SD connectors are stacked at this end.
    cut(shell,box(14,12,11.4,0,-33,11.3))
    # Recesses and open-ended rails for the sliding cover. No brittle snap hooks.
    cut(shell,rr(37.1,55.2,4.7,23.9,27,y=-5.5))
    cut(shell,rr(38.7,54.8,5.0,22.85,24.35,y=-5.5))
    cut(shell,box(38.7,9,3.2,0,-34,24.0))
    cut(shell,rr(20.4,14.4,2,23.7,27,y=28.5))
    cut(shell,box(10,5,3,0,-34,24.8))
    # External antenna lead exit; the antenna itself remains outside for P1.
    cut(shell,box(5,3,4,19.2,12.5,14.5))
    # Vent the camera pocket without changing the glued heatsink.
    for x in (-10.2,10.2):
        for z in (10.5,13.5,16.5,19.5):cut(shell,box(5,6,1.2,x,28.5,z))
    # Front floor supports the bare PCB near the USB end, clear of USB metal.
    for x in (-6,6):combine(shell,box(2.3,3.5,7.1,x,-25.6,3.55))
    # Tray seats support its edges; nothing protrudes through the LiPo floor.
    for x in (-18.3,18.3):
        combine(shell,box(2.8,4,16.2,x,-25.6,8.1))
        combine(shell,box(2.8,4,5.6,x,14.5,13.4))
    # Camera rests on the heatsink, not on its lens or ribbon.
    for x in (-4,4):
        for y in (24.5,32.5):combine(shell,cylinder(1.2,.9,9.2,x,y))
    # Drop-on camera cap avoids dragging a sliding collar across the lens.
    # Captive M2 nuts sit below the heatsink; screw axes clear its sides.
    for x in (-7.6,7.6):
        combine(shell,cylinder(2.8,.9,8.9,x,28.5))
        cut(shell,cylinder(1.15,1,28,x,28.5))
        hexagon=box(4.2,8.4,2.4,x,28.5,6.3)
        for angle in (60,120):
            tool=box(4.2,8.4,2.4,x,28.5,6.3)
            rotate=adsk.core.Matrix3D.create()
            rotate.setToRotation(math.radians(angle),adsk.core.Vector3D.create(0,0,1),point(x,28.5,0))
            manager.transform(tool,rotate)
            combine(hexagon,tool,adsk.fusion.BooleanTypes.IntersectionBooleanType)
        cut(shell,hexagon)
        # Insert captive nuts laterally before fitting the heatsink.
        cut(shell,box(4.3,8,2.4,x,25,6.3))
    add(case,'Housing - 1.6mm sidewalls',shell)
    lid_comp=component('PRINT 02 - sliding cover')
    lid=rr(38.1,54.2,4.7,23.15,24.15,y=-5.5)
    combine(lid,rr(36.6,54.7,4.7,24.1,25.4,y=-5.5))
    cut(lid,box(20.4,24,8,0,33.3,24.1))
    add(lid_comp,'Sliding cover - 0.25 to 0.30mm nominal running clearance',lid)
    cap_comp=component('PRINT 07 - camera cap')
    cap=rr(19.8,13.8,1.7,23.9,25.4,y=28.5)
    combine(cap,rr(12.0,12.0,1,19.8,24.1,y=28.5))
    cut(cap,rr(9.8,9.8,.3,19.3,27,y=28.5))
    cut(cap,box(7.5,7,6,0,22.5,18))
    for x in (-7.6,7.6):cut(cap,cylinder(1.15,19,27,x,28.5))
    add(cap_comp,'Camera cap - two M2 screws and nuts',cap)
    tray_comp=component('PRINT 03 - removable battery tray')
    tray=rr(35.8,51.8,2.0,16.2,17.0,y=-5.5)
    # Slots accept a loose insulating retaining strip; no screw enters the battery bay.
    for x in (-16,16):
        for y in (-20,9):cut(tray,box(1.1,5,2,x,y,16.6))
    cut(tray,box(7,3.5,2,12,20.4,16.6))
    cut(tray,box(7.5,4,2,0,20.4,16.6))
    add(tray_comp,'Battery tray - add thin insulating pad before assembly',tray)
    # Two independent rails screw to the screen's existing M2 mounting standoffs.
    for side in (-1,1):
        comp=component('PRINT 04 - screen rail '+('left' if side<0 else 'right'))
        rail=rr(5,38,1.0,10.65,12.15,x=side*12.5,y=-1)
        for y in (-17,15):cut(rail,cylinder(1.15,9,14,side*12.5,y))
        # Side feet locate the screen rails; front-border adhesive retains the screen.
        for y in (-17,15):combine(rail,box(4,3,1.5,side*15.8,y,11.4))
        add(comp,'Screen rail '+str(side),rail)
    # A small U-channel tests the actual lid cross-section before a full resin print.
    coupon=component('PRINT 05 - sliding fit test channel')
    channel=box(40.4,12,5.3,60,0,2.65)
    cut(channel,box(37.2,14,6,60,0,4.5))
    cut(channel,box(38.7,14,1.5,60,0,3.0))
    add(coupon,'Fit channel',channel)
    for i,clearance in enumerate((.2,.3,.4)):
        comp=component('PRINT 06 - fit slider '+str(clearance))
        x=60;y=20+i*17
        slider=box(38.7-2*clearance,12,1.0,x,y,3.0)
        combine(slider,box(37.2-2*clearance,12,1.3,x,y,4.1))
        add(comp,'Fit slider '+str(clearance),slider)
        # Coupons are printable but kept out of assembly pictures.
    for o in root.occurrences:
        if o.component.name.startswith(('PRINT 05','PRINT 06')):o.isLightBulbOn=False
    d.attributes.add('Enclosure','Status','P1 fit prototype. Ribbon route, solder clearance, antenna placement and thermal performance require physical verification.')
    d.attributes.add('Enclosure','Ribbon','60.96mm free length, 6.35mm width. Slack cavity provided; minimum bend radius and exact exit orientation are not known. Do not force a crease or twist.')
    app.activeViewport.fit()
    print(json.dumps({'document':doc.name,'parts':[o.component.name for o in root.occurrences if o.component.name.startswith('PRINT')]}))
'''


def main():
    client = FusionClient()
    script = BUILD.replace("REFERENCE_PATH", repr(str(REFERENCE)))
    print(json.dumps(client.call("fusion_mcp_execute", {"featureType": "script", "object": {"script": script}})))


if __name__ == "__main__":
    main()
