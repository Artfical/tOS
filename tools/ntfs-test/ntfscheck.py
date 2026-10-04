#!/usr/bin/env python3
"""Independent consistency checker for NTFS images. Usage: ntfscheck.py IMG [--tree]"""
import struct, sys, hashlib
from ntfslib import *

errors = []
warns = []
stale = []


def err(m):
    errors.append(m)


def warn(m):
    warns.append(m)


def main(path, tree=False):
    n = Ntfs(path)
    rs, cs = n.rs, n.cs
    nrecs = n.nrecs
    # MFT bitmap
    rec0 = n.record(0)
    mbm = n.attr(rec0, 0xB0)
    mbits = n.attr_all(mbm)

    def mft_bit(no):
        i = no >> 3
        return bool(mbits[i] >> (no & 7) & 1) if i < len(mbits) else False

    # cluster bitmap
    bm_attr = n.attr(n.record(6), 0x80)
    cbits = n.attr_all(bm_attr)

    def cl_bit(c):
        return bool(cbits[c >> 3] >> (c & 7) & 1)

    owner = {}          # cluster -> description
    records = {}
    allnames = {}       # rec -> list of parsed FILE_NAME dicts
    for no in range(nrecs):
        try:
            r = n.record(no)
        except NtfsError as e:
            if mft_bit(no) and no < 27:
                err('rec %d unreadable: %s' % (no, e))
            elif mft_bit(no):
                err('rec %d marked in use in bitmap but unreadable: %s' % (no, e))
            continue
        flags = struct.unpack_from('<H', r, 22)[0]
        inuse = bool(flags & 1)
        used, alloc = struct.unpack_from('<II', r, 24)
        if used > alloc or alloc != rs:
            err('rec %d bytes_in_use %d / allocated %d invalid' % (no, used, alloc))
        if inuse != mft_bit(no) and no >= 24:
            err('rec %d in_use=%s but MFT bitmap bit=%s' % (no, inuse, mft_bit(no)))
        if not inuse:
            continue
        # attribute chain must end exactly at bytes_in_use
        off = struct.unpack_from('<H', r, 20)[0]
        end_seen = False
        names = []
        data_attrs = []
        try:
            for a in n.attrs(r):
                if a.type == 0x30 and not a.nonres:
                    names.append(parse_fn(a.value))
                if a.nonres:
                    if a.length < 0x40:
                        err('rec %d attr %s nonres header too short' % (no, a.tname()))
                    total = sum(x[1] for x in a.runs)
                    if a.alloc != total * cs:
                        err('rec %d %s%s alloc %d != runs %d clusters' % (no, a.tname(), a.name, a.alloc, total))
                    if a.size > a.alloc or a.init > a.size and a.type != 0xA0 or a.init > a.alloc:
                        err('rec %d %s sizes bad size=%d init=%d alloc=%d' % (no, a.tname(), a.size, a.init, a.alloc))
                    expect_last = total - 1 if total else 0xFFFFFFFFFFFFFFFF
                    if a.last_vcn != expect_last and not (total == 0 and a.last_vcn in (0, 0xFFFFFFFFFFFFFFFF)):
                        err('rec %d %s last_vcn %d != %d' % (no, a.tname(), a.last_vcn, expect_last))
                    for (v, ln, lcn) in a.runs:
                        if lcn < 0:
                            continue
                        for c in range(lcn, lcn + ln):
                            if c >= n.total_clusters:
                                err('rec %d %s cluster %d beyond volume' % (no, a.tname(), c))
                                break
                            if c in owner and owner[c][0] != (no, a.type, a.name):
                                err('cluster %d owned by both %s and rec %d %s' % (c, owner[c], no, a.tname()))
                            owner[c] = ((no, a.type, a.name),)
                            if not cl_bit(c):
                                err('rec %d %s%s uses cluster %d not marked in $Bitmap' % (no, a.tname(), a.name, c))
        except NtfsError as e:
            err('rec %d attribute walk: %s' % (no, e))
            continue
        # chain end
        p = off
        while True:
            t = struct.unpack_from('<I', r, p)[0]
            if t == 0xFFFFFFFF:
                p += 8
                break
            p += struct.unpack_from('<I', r, p + 4)[0]
        if p != used:
            err('rec %d attribute chain ends at %d but bytes_in_use=%d' % (no, p, used))
        links = struct.unpack_from('<H', r, 18)[0]
        nondos = len([x for x in names if x['ntype'] != 2])
        if no >= 27 or no == 5:
            if links not in (nondos, len(names)) and not (no == 5):
                err('rec %d link_count %d != non-DOS names %d' % (no, links, nondos))
        records[no] = dict(flags=flags, names=names, rec=r)
        allnames[no] = names

    # bitmap clusters not referenced
    leaked = [c for c in range(n.total_clusters) if cl_bit(c) and c not in owner]
    if leaked:
        warn('%d clusters marked used in $Bitmap but owned by no attribute (first: %s)' % (len(leaked), leaked[:5]))
    # padding bits beyond the volume should be set
    # directory walk
    seen = set()
    listing = []

    def walk_dir(no, path):
        r = records[no]['rec']
        ir = n.attr(r, 0x90, '$I30')
        if ir is None:
            err('dir rec %d has no $I30 INDEX_ROOT' % no)
            return
        ia = n.attr(r, 0xA0, '$I30')
        ib = n.attr(r, 0xB0, '$I30')
        t, coll, ibs, cpib = struct.unpack_from('<IIIB', ir.value, 0)
        if t != 0x30 or coll != 1:
            err('dir %d index type/collation %#x/%d' % (no, t, coll))
        if ibs != n.ibs:
            warn('dir %d index block size %d != volume %d' % (no, ibs, n.ibs))
        unit = cs if cs <= ibs else 512
        entries = []
        used_blocks = set()

        def visit(node, hdr_off, depth, where):
            if depth > 12:
                err('dir %d index too deep' % no)
                return
            eo, il, al, fl = struct.unpack_from('<IIIB', node, hdr_off)
            for off, e in index_entries(n, node, hdr_off):
                ref, ln, kl, f = struct.unpack_from('<QHHH', e, 0)
                if f & 1:
                    vcn = struct.unpack_from('<Q', e, ln - 8)[0]
                    if ia is None:
                        err('dir %d subnode entry without INDEX_ALLOCATION' % no)
                    else:
                        blk = vcn * unit // ibs
                        used_blocks.add(blk)
                        if ib is not None:
                            bb = ib.value if not ib.nonres else n.attr_all(ib)
                            if blk // 8 >= len(bb) or not (bb[blk // 8] >> (blk % 8) & 1):
                                err('dir %d block vcn %d not set in index bitmap' % (no, vcn))
                        try:
                            data = n.attr_read(ia, vcn * unit, ibs)
                            fx = fixup(data, b'INDX')
                        except NtfsError as ex:
                            err('dir %d INDX vcn %d: %s' % (no, vcn, ex))
                            continue
                        bv = struct.unpack_from('<Q', fx, 16)[0]
                        if bv != vcn:
                            err('dir %d INDX at vcn %d claims vcn %d' % (no, vcn, bv))
                        b_eo, b_il, b_al, b_fl = struct.unpack_from('<IIIB', fx, 0x18)
                        if b_il > b_al or b_al != ibs - 0x18:
                            err('dir %d INDX vcn %d index_length %d alloc %d' % (no, vcn, b_il, b_al))
                        if (b_fl & 1) != 0 and not any(
                                struct.unpack_from('<H', ee, 12)[0] & 1 for _, ee in index_entries(n, fx, 0x18)):
                            err('dir %d INDX vcn %d flagged non-leaf but no subnodes' % (no, vcn))
                        visit(fx, 0x18, depth + 1, 'blk%d' % vcn)
                if f & 2:
                    continue
                entries.append(e)

        # in-order: need proper recursion order, so re-implement with explicit order
        order = []

        def inorder(node, hdr_off, depth):
            for off, e in index_entries(n, node, hdr_off):
                ref, ln, kl, f = struct.unpack_from('<QHHH', e, 0)
                if f & 1:
                    vcn = struct.unpack_from('<Q', e, ln - 8)[0]
                    data = fixup(n.attr_read(ia, vcn * unit, ibs), b'INDX')
                    inorder(data, 0x18, depth + 1)
                if not (f & 2):
                    order.append(e)

        visit(ir.value, 16, 0, 'root')
        try:
            inorder(ir.value, 16, 0)
        except NtfsError as ex:
            err('dir %d traversal: %s' % (no, ex))
            return
        if ia is not None and ib is not None:
            bb = ib.value if not ib.nonres else n.attr_all(ib)
            for blk in range(len(bb) * 8):
                if bb[blk // 8] >> (blk % 8) & 1 and blk not in used_blocks:
                    warn('dir %d index block %d marked in bitmap but unreachable' % (no, blk))
        prev = None
        for e in order:
            ref, ln, kl, f = struct.unpack_from('<QHHH', e, 0)
            fn = parse_fn(e[16:16 + kl])
            if prev is not None:
                c = n.collate(prev['name'], fn['name'])
                if c >= 0:
                    err('dir %d index order violation: %r then %r' % (no, prev['name'], fn['name']))
            prev = fn
            child = ref & 0xFFFFFFFFFFFF
            if child not in records:
                err('dir %d entry %r -> record %d not in use' % (no, fn['name'], child))
                continue
            if fn['parent'] != no:
                err('dir %d entry %r key parent is %d' % (no, fn['name'], fn['parent']))
            cn = records[child]['names']
            match = [x for x in cn if x['name'] == fn['name'] and x['parent'] == no]
            if not match:
                err('dir %d entry %r -> record %d has no matching FILE_NAME' % (no, fn['name'], child))
            seen.add((child, no, fn['name']))
            if fn['ntype'] == 2:
                continue
            cr = records[child]['rec']
            isdir = bool(records[child]['flags'] & 2)
            p = path + '/' + fn['name']
            if isdir:
                if child in (5,):
                    continue
                walk_dir(child, p)
                listing.append((p + '/', 0, ''))
            else:
                da = n.attr(cr, 0x80)
                size = da.size if da else 0
                h = ''
                if tree and da:
                    try:
                        h = hashlib.sha1(n.attr_all(da)).hexdigest()[:12]
                    except Exception as ex:
                        h = 'ERR:%s' % ex
                listing.append((p, size, h))
                if match and match[0]['size'] != size and not isdir:
                    stale.append(p)

    walk_dir(5, '')
    # every in-use user record must be reachable from a parent index
    for no, info in records.items():
        if no < 27:
            continue
        for fn in info['names']:
            if (no, fn['parent'], fn['name']) not in seen:
                err('rec %d name %r (parent %d) is not in its parent index' % (no, fn['name'], fn['parent']))
    if tree:
        for p, s, h in sorted(listing):
            print('%-60s %8d %s' % (p, s, h))
    if stale:
        print('NOTE  %d files have a stale size in their FILE_NAME copy (harmless)' % len(stale))
    for w in warns:
        print('WARN ', w)
    for e in errors:
        print('ERROR', e)
    print('%s: %d records in use, %d errors, %d warnings' % (path, len(records), len(errors), len(warns)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], '--tree' in sys.argv))
