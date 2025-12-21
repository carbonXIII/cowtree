import tempfile
import subprocess
from pathlib import Path
from contextlib import contextmanager

def subcmd(repo, *args):
    return subprocess.check_output(['ostree', '--repo={}'.format(repo), *[str(x) for x in args]], text=True)

@contextmanager
def make_repo(path=None):
    @contextmanager
    def __ensure_dir():
        if not path:
            with tempfile.TemporaryDirectory() as ret:
                yield Path(ret)
        else:
            yield Path(path)

    with __ensure_dir() as repo:
        subcmd(repo, 'init')
        yield Path(repo)

def commit_tree(repo, branch, path):
    return subcmd(repo, 'commit', '--branch={}'.format(branch), path)
