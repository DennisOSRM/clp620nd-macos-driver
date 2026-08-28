#!/usr/bin/env python3
"""Disassembles a PCL-XL stream. Used to validate rastertoclp620 output
against a known-good reference. Usage: pclxl-dis.py file.pcl [--data]"""
import sys, struct

OPS = {
 0x41:'BeginSession',0x42:'EndSession',0x43:'BeginPage',0x44:'EndPage',
 0x45:'Comment',0x46:'OpenDataSource',0x47:'CloseDataSource',
 0x48:'OpenDataSource',0x49:'CloseDataSource',
 0x4A:'BeginFontHeader',0x4B:'ReadFontHeader',0x4C:'EndFontHeader',
 0x4F:'RemoveFont',0x52:'SetCharAttributes',
 0x6A:'SetColorSpace',0x6B:'SetCursor',0x6C:'SetCursorRel',
 0x55:'SetBrushSource',0x56:'SetCharAngle',0x57:'SetCharScale',
 0x58:'SetCharShear',0x59:'SetClipReplace',0x5A:'SetClipIntersect',
 0x5B:'SetClipRectangle',0x5C:'SetClipToPage',0x5D:'SetColorSpace',
 0x5E:'SetColorTrapping',0x5F:'SetColorTreatment',0x60:'SetCursor',
 0x61:'SetCursorRel',0x62:'SetHalftoneMethod',0x63:'SetFillMode',
 0x64:'SetFont',0x65:'SetLineDash',0x66:'SetLineCap',0x67:'SetLineJoin',
 0x68:'SetMiterLimit',0x69:'SetPageDefaultCTM',0x6D:'SetPageOrigin',
 0x6E:'SetPageRotation',0x6F:'SetPageScale',0x70:'SetPaintTxMode',
 0x71:'SetPenSource',0x72:'SetPenWidth',0x73:'SetROP',0x74:'SetSourceTxMode',
 0x75:'SetCharBoldValue',0x77:'SetClipMode',0x78:'SetPathToClip',
 0x79:'SetCharSubMode',
 0x91:'CloseSubPath',0x92:'NewPath',0x93:'PaintPath',
 0xA8:'Rectangle',0xB0:'BeginImage',0xB1:'ReadImage',0xB2:'EndImage',
 0xB3:'BeginRastPattern',0xB4:'ReadRastPattern',0xB5:'EndRastPattern',
}
ATTRS = {
 3:'ColorDepth',4:'ColorImaging',8:'ColorSpace',11:'BlockHeight',
 12:'ColorDepth',14:'BlockByteLength',16:'CompressMode',17:'DestinationBox',
 18:'DestinationSize',23:'PatternPersistence',24:'PatternDefineID',
 25:'SourceHeight',26:'SourceWidth',27:'StartPosition',28:'PatternSelectID',
 29:'GrayLevel',31:'PenGray',33:'RGBColor',34:'PatternOrigin',
 35:'NewDestinationSize',37:'MediaSize',38:'MediaSource',
 39:'MediaDestination',40:'Orientation',41:'PageAngle',42:'PageOrigin',
 43:'PageScale',44:'ROP3',45:'TxMode',46:'CustomMediaSize',
 47:'CustomMediaSizeUnits',48:'PageCopies',49:'DitherMatrixSize',
 50:'DitherMatrixDepth',51:'SimplexPageMode',52:'DuplexPageMode',
 53:'DuplexPageSide',56:'ArcDirection',57:'BoundingBox',58:'DashOffset',
 59:'EllipseDimension',60:'EndPoint',61:'FillMode',62:'LineCapStyle',
 63:'LineJoinStyle',64:'MiterLength',65:'LineDashStyle',66:'PenWidth',
 67:'Point',68:'NumberOfPoints',69:'SolidLine',70:'StartPoint',
 71:'PointType',72:'ControlPoint1',73:'ControlPoint2',74:'ClipRegion',
 75:'ClipMode',76:'ColorTreatment',
 98:'ColorMapping',99:'ColorDepth',100:'ContrastLevel',101:'Gamma',
 109:'DeviceMatrix',110:'DitherMatrixDataType',111:'DitherOrigin',
 112:'MediaType',113:'DitherMatrixSize',
 130:'AllObjectTypes',134:'Measure',143:'ErrorReport',
 133:'SourceType',136:'SourceType',137:'UnitsPerMeasure',
 172:'DataOrg',175:'DataSource',
}
TAGS = {0xC0:('ubyte',1),0xC1:('uint16',2),0xC2:('uint32',4),
        0xC3:('sint16',2),0xC4:('sint32',4),0xC5:('real32',4)}
ARR  = {0xC8:('ubyte',1),0xC9:('uint16',2),0xCA:('uint32',4),
        0xCB:('sint16',2),0xCC:('sint32',4),0xCD:('real32',4)}
XY   = {0xD0:('ubyte',1),0xD1:('uint16',2),0xD2:('uint32',4),
        0xD3:('sint16',2),0xD4:('sint32',4),0xD5:('real32',4)}
BOX  = {0xE0:('ubyte',1),0xE1:('uint16',2),0xE2:('uint32',4),
        0xE3:('sint16',2),0xE4:('sint32',4),0xE5:('real32',4)}
FMT = {'ubyte':'<B','uint16':'<H','uint32':'<I','sint16':'<h','sint32':'<i','real32':'<f'}

def main():
    path = sys.argv[1]; show_data = '--data' in sys.argv
    d = open(path,'rb').read()
    # PJL prologue
    i = d.find(b'PCLXL')
    i = d.find(b'\n', i) + 1
    j = d.find(b'\n', i)
    print(f"binding header: {d[i:j]!r}")
    i = j + 1
    stack = []
    while i < len(d):
        t = d[i]
        if t in TAGS:
            nm,n = TAGS[t]; v = struct.unpack(FMT[nm], d[i+1:i+1+n])[0]
            stack.append(v); i += 1+n
        elif t in XY:
            nm,n = XY[t]
            a = struct.unpack(FMT[nm], d[i+1:i+1+n])[0]
            b = struct.unpack(FMT[nm], d[i+1+n:i+1+2*n])[0]
            stack.append((a,b)); i += 1+2*n
        elif t in BOX:
            nm,n = BOX[t]
            v = tuple(struct.unpack(FMT[nm], d[i+1+k*n:i+1+(k+1)*n])[0] for k in range(4))
            stack.append(v); i += 1+4*n
        elif t in ARR:
            nm,n = ARR[t]; i += 1
            lt = d[i]; ln,lnn = TAGS[lt]
            cnt = struct.unpack(FMT[ln], d[i+1:i+1+lnn])[0]; i += 1+lnn
            stack.append(f"<{nm}[{cnt}]>"); i += cnt*n
        elif t == 0xF8:
            aid = d[i+1]; nm = ATTRS.get(aid, f"attr{aid}")
            val = stack.pop() if stack else None
            stack.append(f"{nm}={val}"); i += 2
        elif t == 0xF9:
            aid = struct.unpack('<H', d[i+1:i+3])[0]; nm = ATTRS.get(aid, f"attr{aid}")
            val = stack.pop() if stack else None
            stack.append(f"{nm}={val}"); i += 3
        elif t == 0xFA:
            n = struct.unpack('<I', d[i+1:i+5])[0]
            print(f"    [embedded data {n} bytes]")
            i += 5 + n
        elif t == 0xFB:
            n = d[i+1]; print(f"    [embedded data {n} bytes]"); i += 2 + n
        elif t in (0x00,0x09,0x0A,0x0D,0x20):
            i += 1
        elif t == 0x1B:   # UEL
            print("UEL"); break
        elif t in OPS or 0x40 <= t <= 0xBF:
            print(f"{OPS.get(t,f'op0x{t:02X}')}({', '.join(map(str,stack))})")
            stack = []; i += 1
        else:
            print(f"?? byte 0x{t:02X} at {i}"); i += 1

main()
