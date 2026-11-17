#!/usr/bin/env python3
"""Hand finishing for TL-SN-001 after route_pcbs.py (run with KiCad's bundled Python).

Freerouting could not close two fine-pitch power connections because plane-fanout vias
crowded the pins. This applies the edits a layout engineer would make:
  U4 (BQ25186): pin 4 GND to the exposed pad, CHG_PG_N moved up, VBAT straight to C10.
  U5 (TPS62840): pin 6 directly to output cap C12, pin 8 GND to the exposed pad, LX2 pin 7 to L1.
It matches exact coordinates of the current routed board and refuses to save if they moved.
"""
import pathlib
import pcbnew
P = str(pathlib.Path(__file__).resolve().parents[1] / "kicad/TL-SN-001/TL-SN-001.kicad_pcb")
b=pcbnew.LoadBoard(P)
F=pcbnew.FromMM; mm=pcbnew.ToMM
def at(pt,x,y,tol=0.01): return abs(mm(pt.x)-x)<tol and abs(mm(pt.y)-y)<tol
def kill(pred,label):
    gone=[t for t in b.GetTracks() if pred(t)]
    for t in gone: b.Remove(t)
    print(label,len(gone))
    if len(gone) != EXPECT[label]:
        raise SystemExit(f'{label}: expected {EXPECT[label]} items, found {len(gone)}; board changed, edit by hand')
    return gone
EXPECT={'U4 gnd via+stub':2,'U4 chg_pg_n':5,'U5 v3v3 via+stub':2,'U5 gnd via+stub':2}
net=lambda n: b.FindNet(n)
def trk(n,pts,w):
    for (x0,y0),(x1,y1) in zip(pts,pts[1:]):
        t=pcbnew.PCB_TRACK(b); t.SetStart(pcbnew.VECTOR2I(F(x0),F(y0))); t.SetEnd(pcbnew.VECTOR2I(F(x1),F(y1)))
        t.SetWidth(F(w)); t.SetLayer(pcbnew.F_Cu); t.SetNet(net(n)); b.Add(t)
isvia=lambda t:t.Type()==pcbnew.PCB_VIA_T
keep=[]
# --- U4: pin 4 GND to exposed pad; CHG_PG_N straight out then up; VBAT straight to C10
keep+=kill(lambda t: t.GetNetname()=='/GND' and ((not isvia(t) and at(t.GetStart(),152.825,161.6) and at(t.GetEnd(),153.575,161.6)) or (isvia(t) and at(t.GetPosition(),153.575,161.6))),'U4 gnd via+stub')
keep+=kill(lambda t: t.GetNetname()=='/CHG_PG_N' and not isvia(t) and any(at(p,x,y) for p in (t.GetStart(),t.GetEnd()) for x,y in ((153.202,162.0),(153.4,162.198),(153.791,162.198),(155.1,160.889))),'U4 chg_pg_n')
trk('/GND',[(152.825,161.6),(152.2,161.6)],0.2)
trk('/CHG_PG_N',[(152.825,162.0),(153.45,162.0),(154.79,160.66),(155.1,160.66)],0.15)
trk('/VBAT',[(152.825,162.4),(153.3,162.4)],0.2)
trk('/VBAT',[(153.3,162.4),(155.05,162.47)],0.25)
# --- U5: pin 6 V3V3_CORE straight to C12; pin 8 GND to exposed pad; LX2 from pin 7 to L1
keep+=kill(lambda t: t.GetNetname()=='/V3V3_CORE' and ((not isvia(t) and at(t.GetStart(),167.088,160.8) and at(t.GetEnd(),166.175,160.8)) or (isvia(t) and at(t.GetPosition(),166.175,160.8))),'U5 v3v3 via+stub')
keep+=kill(lambda t: t.GetNetname()=='/GND' and ((not isvia(t) and at(t.GetStart(),167.088,161.8) and at(t.GetEnd(),166.175,161.8)) or (isvia(t) and at(t.GetPosition(),166.175,161.8))),'U5 gnd via+stub')
trk('/V3V3_CORE',[(167.088,160.8),(167.088,158.15)],0.25)
trk('/GND',[(167.088,161.8),(168.3,161.8)],0.25)
trk('/REG_LX2',[(167.088,161.3),(166.6,161.3)],0.25)
trk('/REG_LX2',[(166.6,161.3),(166.2,161.3),(166.2,159.0),(164.275,159.0)],0.3)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(P); print('saved')
