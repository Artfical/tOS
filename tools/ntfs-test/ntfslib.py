"""Minimal independent NTFS parser used to dump structures and to check that
what the tOS driver wrote is consistent (no dependency on the C code)."""
import struct, sys

AT = {0x10: 'STD_INFO', 0x20: 'ATTR_LIST', 0x30: 'FILE_NAME', 0x40: 'OBJ_ID', 0x50: 'SECURITY',
      0x60: 'VOL_NAME', 0x70: 'VOL_INFO', 0x80: 'DATA', 0x90: 'INDEX_ROOT', 0xA0: 'INDEX_ALLOC',
      0xB0: 'BITMAP', 0xC0: 'REPARSE', 0xD0: 'EA_INFO', 0xE0: 'EA', 0x100: 'LOGGED_UTIL'}


class NtfsError(Exception):
    pass


def fixup(buf, magic):
    buf = bytearray(buf)
    if bytes(buf[0:4]) != magic:
        raise NtfsError('bad magic %r (want %r)' % (bytes(buf[0:4]), magic))
    uo, uc = struct.unpack_from('<HH', buf, 4)
    if (uc - 1) * 512 != len(buf):
        raise NtfsError('usa_count %d does not match size %d' % (uc, len(buf)))
    usn = struct.unpack_from('<H', buf, uo)[0]
    for i in range(1, uc):
        p = i * 512 - 2
        if struct.unpack_from('<H', buf, p)[0] != usn:
            raise NtfsError('fixup mismatch at sector %d' % (i - 1))
        buf[p:p + 2] = buf[uo + 2 * i:uo + 2 * i + 2]
    return bytes(buf)


def decode_runs(mp):
    runs = []
    p = 0
    vcn = 0
    lcn = 0
    while p < len(mp) and mp[p]:
        h = mp[p]
        p += 1
        lb, ob = h & 0xF, h >> 4
        ln = int.from_bytes(mp[p:p + lb], 'little')
        p += lb
        if ob:
            off = int.from_bytes(mp[p:p + ob], 'little', signed=True)
            p += ob
            lcn += off
            runs.append((vcn, ln, lcn))
        else:
            runs.append((vcn, ln, -1))
        vcn += ln
    return runs


class Attr:
    def __init__(self, rec, off):
        self.rec = rec
        self.off = off
        (self.type, self.length, self.nonres, self.name_len, self.name_off,
         self.flags, self.instance) = struct.unpack_from('<IIBBHHH', rec, off)
        self.name = rec[off + self.name_off: off + self.name_off + self.name_len * 2].decode('utf-16le') if self.name_len else ''
        if self.nonres:
            (self.start_vcn, self.last_vcn, self.mp_off, self.comp_unit) = struct.unpack_from('<QQHB', rec, off + 16)
            (self.alloc, self.size, self.init) = struct.unpack_from('<QQQ', rec, off + 40)
            self.runs = decode_runs(rec[off + self.mp_off: off + self.length])
            self.value = None
        else:
            (self.vlen, self.voff) = struct.unpack_from('<IH', rec, off + 16)
            self.value = rec[off + self.voff: off + self.voff + self.vlen]
            self.size = self.vlen
            self.runs = []

    def tname(self):
        return AT.get(self.type, hex(self.type))


class Ntfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        b = self.read(0, 512)
        self.bps, self.spc = struct.unpack_from('<HB', b, 0x0B)
        self.total_sectors, self.mft_lcn, self.mirr_lcn = struct.unpack_from('<QQQ', b, 0x28)
        cpr, = struct.unpack_from('<b', b, 0x40)
        cpi, = struct.unpack_from('<b', b, 0x44)
        self.cs = self.bps * self.spc
        self.rs = (1 << -cpr) if cpr < 0 else cpr * self.cs
        self.ibs = (1 << -cpi) if cpi < 0 else cpi * self.cs
        self.total_clusters = self.total_sectors // self.spc
        rec0 = self.raw_record(0)
        self.mft_attr = self.attr(rec0, 0x80)
        self.mft_size = self.mft_attr.size
        self.nrecs = self.mft_size // self.rs
        self._upcase = None

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def raw_record(self, no):
        if no == 0:
            data = self.read(self.mft_lcn * self.cs, self.rs)
        else:
            data = self.attr_read(self.mft_attr, no * self.rs, self.rs)
        return fixup(data, b'FILE')

    record = raw_record

    def attrs(self, rec):
        off = struct.unpack_from('<H', rec, 20)[0]
        while off + 8 <= len(rec):
            t = struct.unpack_from('<I', rec, off)[0]
            if t == 0xFFFFFFFF:
                break
            a = Attr(rec, off)
            yield a
            if a.length == 0:
                raise NtfsError('zero-length attribute')
            off += a.length

    def attr(self, rec, t, name=''):
        for a in self.attrs(rec):
            if a.type == t and a.name == name:
                return a
        return None

    def attr_read(self, a, off, n):
        if not a.nonres:
            return a.value[off:off + n]
        out = bytearray()
        end = min(off + n, a.size)
        pos = off
        while pos < end:
            vcn = pos // self.cs
            run = None
            for r in a.runs:
                if r[0] <= vcn < r[0] + r[1]:
                    run = r
                    break
            if run is None:
                raise NtfsError('read beyond runlist')
            in_run = pos - run[0] * self.cs
            chunk = min(end - pos, run[1] * self.cs - in_run)
            if run[2] < 0 or pos >= a.init:
                out += b'\0' * chunk
            else:
                out += self.read(run[2] * self.cs + in_run, chunk)
            pos += chunk
        return bytes(out)

    def attr_all(self, a):
        return self.attr_read(a, 0, a.size)

    def upcase(self):
        if self._upcase is None:
            r = self.record(10)
            d = self.attr_all(self.attr(r, 0x80))
            self._upcase = struct.unpack('<%dH' % (len(d) // 2), d)
        return self._upcase

    def up(self, c):
        u = self.upcase()
        return u[c] if c < len(u) else c

    def collate(self, a, b):
        n = min(len(a), len(b))
        for i in range(n):
            ua, ub = self.up(ord(a[i]) if isinstance(a[i], str) else a[i]), self.up(ord(b[i]) if isinstance(b[i], str) else b[i])
            if ua != ub:
                return -1 if ua < ub else 1
        if len(a) != len(b):
            return -1 if len(a) < len(b) else 1
        for i in range(n):
            if a[i] != b[i]:
                return -1 if a[i] < b[i] else 1
        return 0


def parse_fn(v):
    parent, ct, mt, mft_t, at, alloc, size, flags, rep, nl, nt = struct.unpack_from('<QQQQQQQIIBB', v, 0)
    name = v[66:66 + nl * 2].decode('utf-16le')
    return dict(parent=parent & 0xFFFFFFFFFFFF, pseq=parent >> 48, alloc=alloc, size=size, flags=flags, ntype=nt, name=name)


def index_entries(fs, node, hdr_off, check=None):
    """Yield (offset, entry_bytes) for entries in a node whose INDEX_HEADER is at hdr_off."""
    eo, il, al, fl = struct.unpack_from('<IIIB', node, hdr_off)
    p = hdr_off + eo
    end = hdr_off + il
    if end > len(node):
        raise NtfsError('index_length beyond node')
    while True:
        if p + 16 > end:
            raise NtfsError('index entry overruns node')
        ref, ln, kl, flags = struct.unpack_from('<QHHH', node, p)
        if ln < 16 or p + ln > end or ln % 8:
            raise NtfsError('bad index entry length %d at %d' % (ln, p))
        yield p, node[p:p + ln]
        if flags & 2:
            break
        p += ln


if __name__ == '__main__':
    n = Ntfs(sys.argv[1])
    print('bps', n.bps, 'spc', n.spc, 'rs', n.rs, 'ibs', n.ibs, 'total_clusters', n.total_clusters, 'mft_lcn', n.mft_lcn, 'mft_size', n.mft_size, 'nrecs', n.nrecs)
    recs = [int(x) for x in sys.argv[2:]] or [0, 3, 5]
    for no in recs:
        r = n.record(no)
        fl, biu, bal, base, nai = struct.unpack_from('<HIIQH', r, 22)
        print('--- record %d seq=%d links=%d attrs_off=%d flags=%#x bytes_in_use=%d alloc=%d base=%#x next_inst=%d' % (
            no, struct.unpack_from('<H', r, 16)[0], struct.unpack_from('<H', r, 18)[0], struct.unpack_from('<H', r, 20)[0], fl, biu, bal, base, nai))
        for a in n.attrs(r):
            line = '  %-11s len=%d inst=%d name=%r flags=%#x' % (a.tname(), a.length, a.instance, a.name, a.flags)
            if a.nonres:
                line += ' NONRES size=%d alloc=%d init=%d runs=%s' % (a.size, a.alloc, a.init, a.runs[:6])
            else:
                line += ' RES vlen=%d' % a.vlen
                if a.type == 0x30:
                    line += ' ' + str(parse_fn(a.value))
                if a.type == 0x10:
                    line += ' fileattr=%#x' % struct.unpack_from('<I', a.value, 32)[0] + (' sec=%d' % struct.unpack_from('<I', a.value, 52)[0] if a.vlen >= 72 else '')
                if a.type == 0x70:
                    line += ' major=%d minor=%d flags=%#x' % (a.value[8], a.value[9], struct.unpack_from('<H', a.value, 10)[0])
                if a.type == 0x90:
                    t, c, ibs, cpib = struct.unpack_from('<IIIB', a.value, 0)
                    eo, il, al, f = struct.unpack_from('<IIIB', a.value, 16)
                    line += ' type=%#x coll=%d ibs=%d idxhdr(eo=%d len=%d alloc=%d flags=%d)' % (t, c, ibs, eo, il, al, f)
                    for off, e in index_entries(n, a.value, 16):
                        ref, ln, kl, fl2 = struct.unpack_from('<QHHH', e, 0)
                        nm = parse_fn(e[16:16 + kl])['name'] if kl else '<END>'
                        line += '\n       entry rec=%d len=%d flags=%d %s' % (ref & 0xFFFFFFFFFFFF, ln, fl2, nm)
                if a.type == 0xB0:
                    line += ' bits=%s' % a.value.hex()
            print(line)
