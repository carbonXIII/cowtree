import os
from pathlib import Path
import subprocess
import tempfile
from contextlib import contextmanager
import uuid
import hashlib

import ostree

def strip_newline(s):
    if s.endswith('\n'):
        return s[:-1]
    return s

@contextmanager
def loop_dev(path: Path) -> Path:
    loop = Path(
        strip_newline(
            subprocess.check_output(
                ['losetup', '--show', '-f', path], text=True
            )
        )
    )
    try:
        yield loop
    finally:
        subprocess.check_output(['losetup', '-d', loop])

@contextmanager
def mount(dev, path, *args):
    subprocess.check_output([
        'mount',
        *args,
        str(dev),
        str(path),
    ])

    try:
        yield
    finally:
        subprocess.check_output([
            'umount',
            path,
        ])

@contextmanager
def make_btrfs(root, size):
    dev = root / 'test.btrfs'
    with open(dev, 'wb') as f:
        f.truncate(size)
    subprocess.check_output([
        'mkfs.btrfs',
        dev,
    ])

    with loop_dev(dev) as loop:
        with mount(loop, root / 'mnt', '-t', 'btrfs', '--mkdir'):
            yield loop

@contextmanager
def clone_dev(name, dev, size):
    try:
        subprocess.check_output([
            'dmsetup',
            'create',
            name,
            '--table',
            '0 {} linear {} 0'.format(size // 512, dev)
        ])

        yield '/dev/mapper/{}'.format(name)
    finally:
        subprocess.check_output(['dmsetup', 'remove', name])

def test_lock_extents():
    FS_SIZE = 128<<20

    with tempfile.TemporaryDirectory() as _root:
        root = Path(_root)
        with make_btrfs(root, FS_SIZE) as loop:
            with ostree.make_repo(root / 'mnt') as repo:
                # create large test file
                os.mkdir(repo / 'tree')

                test_files = {
                    'big.bin': [FS_SIZE // 8 + 7],
                    'odd.bin': [4096 + 7],
                    'block.bin': [4095],
                }
                for name, data  in test_files.items():
                    size = data[0]
                    with open(repo / 'tree' / name, 'wb') as f:
                        f.truncate(data[0])
                        os.pwrite(f.fileno(),  b'START', 0)
                        os.pwrite(f.fileno(),  b'MIDDLE', size // 2)
                        os.pwrite(f.fileno(),  b'END', size - 3)

                    with open(repo / 'tree' / name, 'rb') as f:
                        test_files[name] += [hashlib.file_digest(f, 'sha256').hexdigest()]

                os.symlink(repo / 'tree/big.bin', repo / 'tree/symlink')

                commit = ostree.commit_tree(repo, 'test', repo / 'tree').strip()
                print('commit:', commit)

                # note: for erofs file-backed mount, this needs to be on a filesystem with
                #       read_folio (i.e. not tmpfs) or it will cryptically fail with ENOTBLK
                erofs_dev = Path(repo / 'image/test.bin')

                res = subprocess.check_output(['../build/src/cowtree', 'build_image', '-o', '-', str(repo), str(commit)])
                with open(erofs_dev, 'wb') as f:
                    f.write(res)

                print(subprocess.check_output(['stat', repo / 'image/mirror'], text=True))

                # remount erofs+btrfs
                # with sys_mount(erofs_dev, root / 'mnt2', root / 'test.btrfs'):
                with mount(erofs_dev, root / 'mnt2', '--mkdir', '-t', 'erofs', '-o', 'device={}'.format(repo / 'image/mirror')):
                    for name in test_files.keys():
                        with open(root / 'mnt2' / name, 'rb') as f:
                            got_hash = hashlib.file_digest(f, 'sha256').hexdigest()
                            expected_hash = test_files[name][1]
                            assert expected_hash == got_hash, '{} != {}'.format(expected_hash, got_hash)

                    with open(root / 'mnt2/symlink', 'rb') as f:
                        got_hash = hashlib.file_digest(f, 'sha256').hexdigest()
                        expected_hash = test_files['big.bin'][1]
                        assert expected_hash == got_hash, '{} != {}'.format(expected_hash, got_hash)


if __name__ == '__main__':
    test_lock_extents()
