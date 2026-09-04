"""Minimal SNMPv1 GETNEXT responder, for testing the backend's jam detection."""
import socket, sys, struct

def enc_len(n):
    if n < 128: return bytes([n])
    b = n.to_bytes((n.bit_length()+7)//8, 'big')
    return bytes([0x80 | len(b)]) + b

def tlv(tag, val): return bytes([tag]) + enc_len(len(val)) + val

def enc_oid(o):
    out = bytes([o[0]*40 + o[1]])
    for v in o[2:]:
        t = [v & 0x7f]; v >>= 7
        while v: t.append((v & 0x7f) | 0x80); v >>= 7
        out += bytes(reversed(t))
    return out

def dec_len(b, i):
    n = b[i]; i += 1
    if n & 0x80:
        k = n & 0x7f; n = int.from_bytes(b[i:i+k], 'big'); i += k
    return n, i

def dec_oid(b):
    o = [b[0]//40, b[0]%40]; acc = 0
    for c in b[1:]:
        acc = (acc << 7) | (c & 0x7f)
        if not c & 0x80: o.append(acc); acc = 0
    return tuple(o)

def parse_req(pkt):
    """-> (community, reqid_bytes, requested_oid)"""
    i = 0
    assert pkt[i] == 0x30; _, i = dec_len(pkt, i+1)
    assert pkt[i] == 0x02; n, i = dec_len(pkt, i+1); i += n          # version
    assert pkt[i] == 0x04; n, i = dec_len(pkt, i+1)
    comm = pkt[i:i+n].decode(); i += n
    assert pkt[i] in (0xA0, 0xA1); _, i = dec_len(pkt, i+1)
    assert pkt[i] == 0x02; n, i = dec_len(pkt, i+1)
    reqid = pkt[i:i+n]; i += n
    assert pkt[i] == 0x02; n, i = dec_len(pkt, i+1); i += n          # err-status
    assert pkt[i] == 0x02; n, i = dec_len(pkt, i+1); i += n          # err-index
    assert pkt[i] == 0x30; _, i = dec_len(pkt, i+1)
    assert pkt[i] == 0x30; _, i = dec_len(pkt, i+1)
    assert pkt[i] == 0x06; n, i = dec_len(pkt, i+1)
    return comm, reqid, dec_oid(pkt[i:i+n])

SCENARIOS = {
  "nojam": [((1,3,6,1,2,1,43,18,1,1,7,1,1), 0x02, 808),
            ((1,3,6,1,2,1,43,18,1,1,7,1,2), 0x02, 23),
            ((1,3,6,1,2,1,43,18,1,1,8,1,1), 0x04,
             b"The paper supply in the Bypass Tray is empty. User intervention is "
             b"required to add paper to the Bypass Tray."),
            ((1,3,6,1,2,1,43,18,1,1,8,1,2), 0x04, b"The machine is in Power Saver Mode.")],
  "jamcode": [((1,3,6,1,2,1,43,18,1,1,7,1,1), 0x02, 8),
              ((1,3,6,1,2,1,43,18,1,1,8,1,1), 0x04, b"Something is wrong.")],
  "jamdesc": [((1,3,6,1,2,1,43,18,1,1,7,1,1), 0x02, 1301),
              ((1,3,6,1,2,1,43,18,1,1,8,1,1), 0x04,
               b"A media JAM has occurred in the fuser area.")],
}

def main():
    scen, port = sys.argv[1], int(sys.argv[2])
    table = sorted(SCENARIOS[scen])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    print("ready", flush=True)
    s.settimeout(20)
    while True:
        try: pkt, addr = s.recvfrom(4096)
        except socket.timeout: return
        try: comm, reqid, want = parse_req(pkt)
        except Exception: continue
        nxt = next((e for e in table if e[0] > want), None)
        if nxt is None:
            oid, tag, val = (1,3,6,1,2,1,43,99), 0x05, b''
            body = b''
        else:
            oid, tag, val = nxt
            body = (val.to_bytes(max(1,(val.bit_length()+8)//8), 'big')
                    if tag == 0x02 else val)
        vb  = tlv(0x30, tlv(0x06, enc_oid(oid)) + tlv(tag, body))
        pdu = tlv(0xA2, tlv(0x02, reqid) + tlv(0x02, b'\0') + tlv(0x02, b'\0')
                        + tlv(0x30, vb))
        msg = tlv(0x30, tlv(0x02, b'\0') + tlv(0x04, comm.encode()) + pdu)
        s.sendto(msg, addr)

main()
