"""Run with python3 -m unittest discover -s tools/tests."""
import contextlib
import io
from pathlib import Path
import plistlib
import runpy
import shutil
import tempfile
import unittest
from unittest.mock import patch


class CachedResourcesTest(unittest.TestCase):
    def test_cached_image_refreshes_resources(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            source, output = base / 'source.app', base / 'output.app'
            for bundle in (source, output):
                (bundle / 'Contents/MacOS').mkdir(parents=True)
            (source / 'Contents/Resources').mkdir()
            resource = source / 'Contents/Resources/resource.dat'
            resource.write_bytes(b'updated resource')
            (source / 'Contents/Info.plist').write_bytes(plistlib.dumps({
                'CFBundleIdentifier': 'com.popcap.plantsvszombies',
                'CFBundleExecutable': 'game',
            }))
            (source / 'Contents/MacOS/game').write_bytes(b'/src/drm/mach-o/')
            (output / 'Contents/SharedSupport').mkdir()
            (output / 'Contents/SharedSupport/PlantsVsZombies.image').write_bytes(b'cached')

            def run(arguments, **kwargs):
                # Exercise build decisions without compiling, signing, or requiring Steam.
                if arguments[0] == 'ditto' and arguments[-2] == str(resource.parent):
                    shutil.copytree(arguments[-2], arguments[-1], dirs_exist_ok=True)

            script = Path(__file__).resolve().parents[1] / 'build.py'
            with patch('sys.argv', [str(script), str(source), '--output', str(output)]), \
                    patch('subprocess.run', side_effect=run), contextlib.redirect_stdout(io.StringIO()):
                runpy.run_path(str(script), run_name='__main__')
            self.assertEqual((output / 'Contents/Resources/resource.dat').read_bytes(), b'updated resource')


if __name__ == '__main__':
    unittest.main()
