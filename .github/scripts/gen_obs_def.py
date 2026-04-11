"""
Parse obs.dll PE export table and write obs.def for MSVC lib.exe.
Usage: python gen_obs_def.py <obs.dll path> <output obs.def path>
"""
import struct
import sys


def get_exports(data):
    e = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[e:e+4] == b'PE\x00\x00', "Not a PE file"
    coff = e + 4
    ns  = struct.unpack_from('<H', data, coff + 2)[0]
    oph = struct.unpack_from('<H', data, coff + 16)[0]
    opt = coff + 20
    m   = struct.unpack_from('<H', data, opt)[0]
    # DataDirectory[0] = Export Table; offset differs between PE32 and PE32+
    exp_rva = struct.unpack_from('<I', data, opt + (112 if m == 0x20b else 96))[0]
    secs = opt + oph

    def r2o(rva):
        for i in range(ns):
            s  = secs + i * 40
            va = struct.unpack_from('<I', data, s + 12)[0]
            vs = struct.unpack_from('<I', data, s + 16)[0]
            ro = struct.unpack_from('<I', data, s + 20)[0]
            if va <= rva < va + vs:
                return rva - va + ro
        raise ValueError(f'unmapped RVA 0x{rva:x}')

    eo  = r2o(exp_rva)
    nn  = struct.unpack_from('<I', data, eo + 24)[0]
    nro = struct.unpack_from('<I', data, eo + 32)[0]
    no  = r2o(nro)

    names = []
    for i in range(nn):
        nr  = struct.unpack_from('<I', data, no + i * 4)[0]
        noo = r2o(nr)
        end = data.index(b'\x00', noo)
        names.append(data[noo:end].decode('ascii'))
    return names


def main():
    dll_path, def_path = sys.argv[1], sys.argv[2]
    with open(dll_path, 'rb') as f:
        data = f.read()
    names = get_exports(data)
    with open(def_path, 'w') as f:
        f.write('LIBRARY obs\nEXPORTS\n')
        for n in names:
            f.write(f'    {n}\n')
    print(f'Generated {def_path} with {len(names)} exports')


if __name__ == '__main__':
    main()
