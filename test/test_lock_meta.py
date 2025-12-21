import os
import ostree
import subprocess

BLOCK_SIZE = 4096

def erofs_cat(fs, path):
    return subprocess.check_output(['dump.erofs', fs, '--path={}'.format(path), '--cat'])

def test_lock_meta():
    SMALL = b'hello, repo!\n'
    SMALL2 = b'hello again, repo!\n'

    with ostree.make_repo() as repo:
        os.mkdir(repo / 'tree')
        os.mkdir(repo / 'tree/foo')
        os.mkdir(repo / 'tree/bar')
        os.mkdir(repo / 'tree/bar/baz')

        test_files = {
            'foo/small.txt': SMALL,
            'foo/small2.txt': SMALL2,
            'bar/small.txt': SMALL,
        }

        for f,content in test_files.items():
            with open(repo / 'tree' / f, 'wb') as f:
                f.write(content)

        with open(repo / 'tree/foo/big.txt', 'wb') as f:
            f.truncate(BLOCK_SIZE * 2)

        commit = ostree.commit_tree(repo, 'test', repo / 'tree').strip()
        print('commit:', commit)
        print(ostree.subcmd(repo, 'ls', 'test'))
        print(ostree.subcmd(repo, 'ls', 'test', 'bar'))

        res = subprocess.check_output(['../build/src/cowtree', 'build_image', '-o', '-', str(repo), str(commit)])
        with open(repo / 'test.bin', 'wb') as f:
            f.write(res)

        for f,expected in test_files.items():
            got = erofs_cat(repo / 'test.bin', f)
            assert got == expected

if __name__ == '__main__':
    test_lock_meta()
