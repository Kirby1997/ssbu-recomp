"""Write config\\dlc.txt (the add-on contents the player owns) from the file names of the player's own DLC dumps.

    python dlc_list.py <folder with your DLC dumps> [output file]       (this file: tools_audit\\ of the study tree, tools/audit/ of the repository)

A dump's file name carries its add-on content id in square brackets, e.g. `... [01006A800016F041][v0].nsp`. The id is
application id + 0x1000 + index; the runtime's nn::aoc answers the game with the indices listed in the output file.
Default output = the file the runtime reads (nnmisc.c aoc_load): <folder of the exe>\\..\\config\\dlc.txt, that is
<repository>\\build\\config\\dlc.txt (the exe is build\\work\\ssbu_native.exe), or config\\dlc.txt beside the study folder;
SSBU_DLC_LIST, when set, is the runtime's override and the default here too. A build made with scripts\\setup.py --build <folder>
reads <folder>\\config\\dlc.txt: give that path as the output file. Nothing is opened or decrypted: only the names are read.
No game content in this file.

Linux (Steam Frame): the runtime (runtime/datapaths.c) reads <build>/config/dlc.txt when it exists, else
$XDG_CONFIG_HOME/ssbu-recomp/dlc.txt (~/.config/ssbu-recomp/dlc.txt); the default output here is the same file. The runtime also
takes the DLC dump folder itself (--dlc <folder>, SSBU_DLC_LIST=<folder>, or `dlc=<folder>` in ~/.config/ssbu-recomp/config)
and reads the same names, so this script is optional there."""
import os
import re
import sys

APP = 0x01006A800016E000          # the application these add-on contents belong to
HERE = os.path.dirname(os.path.abspath(__file__))


def default_output():
    """Where the runtime looks: <exe folder>\\..\\config\\dlc.txt, or SSBU_DLC_LIST."""
    env = os.environ.get('SSBU_DLC_LIST')
    if env:
        return env
    if os.name != 'nt':
        return default_output_linux()
    if os.path.basename(os.path.dirname(HERE)) == 'tools':           # repository: <repo>\tools\audit, the exe is <repo>\build\work\ssbu_native.exe
        exe_dir = os.path.join(HERE, '..', '..', 'build', 'work')
    else:                                                          # study tree: <study>\tools_audit, the exe is in <study>
        exe_dir = os.path.join(HERE, '..')
    return os.path.normpath(os.path.join(exe_dir, '..', 'config', 'dlc.txt'))


def default_output_linux():
    """runtime/datapaths.c's order: <exe folder>/../config/dlc.txt when it exists (the repository's build/linux-arm64), else
    $XDG_CONFIG_HOME/ssbu-recomp/dlc.txt."""
    if os.path.basename(os.path.dirname(HERE)) == 'tools':
        layout = os.path.join(HERE, '..', '..', 'build', 'linux-arm64', 'config', 'dlc.txt')
    else:
        layout = os.path.join(HERE, '..', '..', 'config', 'dlc.txt')
    if os.path.isfile(layout):
        return os.path.normpath(layout)
    xdg = os.environ.get('XDG_CONFIG_HOME', '')
    if not xdg.startswith('/'):                                    # the XDG spec: a relative value is ignored
        xdg = os.path.join(os.path.expanduser('~'), '.config')
    return os.path.join(xdg, 'ssbu-recomp', 'dlc.txt')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    folder = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else default_output()
    rows = {}
    for name in sorted(os.listdir(folder)):
        for m in re.finditer(r'\[([0-9A-Fa-f]{16})\]', name):
            tid = int(m.group(1), 16)
            index = tid - APP - 0x1000
            if 0 < index < 0x1000:
                label = re.sub(r'\[[^\]]*\]', ' ', os.path.splitext(name)[0])
                rows[index] = ' '.join(label.split())
    if not rows:
        print('no add-on content id of this application found in the file names of %s' % folder)
        return 1
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('# add-on contents owned (index = add-on content id - application id - 0x1000); written by dlc_list.py\n')
        for index in sorted(rows):
            f.write('0x%03x   # %s\n' % (index, rows[index]))
    print('%d add-on contents -> %s' % (len(rows), out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
