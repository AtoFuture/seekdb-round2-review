"""Portable CLI regressions; no competition service or external evidence required.

Run with Python 3.7+ and -B. Set TEMP/TMP/TMPDIR to the project's temporary
directory first (D:/Codex/MiniVectorDB/tmp on Windows, /data/... in Docker).
"""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).resolve().parents[1] / 'scripts' / 'check-submission-contract.py'
TEMP_ROOT = Path(os.environ.get('TMPDIR') or os.environ.get('TEMP') or
                 os.environ.get('TMP') or ('/data/tmp' if os.name != 'nt' else
                                          'D:/Codex/MiniVectorDB/tmp'))


class SubmissionContractTests(unittest.TestCase):
    def setUp(self):
        TEMP_ROOT.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='submission-contract-', dir=str(TEMP_ROOT))
        self.repo = Path(self.temp.name)

    def tearDown(self):
        # Python 3.7's TemporaryDirectory does not clear Windows read-only Git blobs.
        resolved = self.repo.resolve()
        self.assertEqual(resolved.parent, TEMP_ROOT.resolve())
        self.assertTrue(resolved.name.startswith('submission-contract-'))
        for directory, _, files in os.walk(str(resolved)):
            for name in files:
                target = Path(directory) / name
                target.chmod(target.stat().st_mode | stat.S_IWRITE)
        self.temp.cleanup()

    def write(self, data=b'{}', path='agent/conversation.json'):
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def check(self, *arguments, **options):
        command = [sys.executable, '-B', str(CHECKER)]
        if not options.get('default_repository'):
            command += ['--repository', str(self.repo)]
        result = subprocess.run(command + list(arguments), cwd=str(self.repo),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertTrue(CHECKER.is_file(), 'Missing standalone submission contract checker')
        self.assertTrue(result.stdout, result.stderr.decode('utf-8', errors='replace'))
        diagnostic = json.loads(result.stdout.decode('utf-8'))
        self.assertEqual(diagnostic['scope'], 'local_packaging_only')
        self.assertEqual(diagnostic['local_packaging_only'], True)
        self.assertNotIn('official_score', diagnostic)
        self.assertNotIn('VALID', result.stdout.decode('utf-8'))
        for key in ('requested_path', 'resolved_path', 'git_path', 'ref', 'bytes',
                    'sha256', 'git_blob', 'format_pass', 'mirror', 'errors'):
            self.assertIn(key, diagnostic)
        return result.returncode, diagnostic

    def assert_failure(self, data, error):
        self.write(data)
        code, report = self.check()
        self.assertNotEqual(code, 0)
        self.assertFalse(report['format_pass'])
        self.assertIn(error, [item['code'] for item in report['errors']])

    def git(self, *arguments, **options):
        result = subprocess.run(['git', '-c', 'core.bare=false', '-c', 'core.autocrlf=false'] +
                                list(arguments), cwd=str(self.repo),
                                input=options.get('input'), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
        return result.stdout.decode('utf-8').strip()

    def initialize_git(self):
        self.git('init', '--quiet')
        self.git('config', 'user.name', 'Submission contract test')
        self.git('config', 'user.email', 'submission-test@example.invalid')
        self.git('config', 'commit.gpgsign', 'false')

    def commit(self):
        self.git('add', '--all')
        self.git('commit', '--quiet', '-m', 'Synthetic material fixture')
        return self.git('rev-parse', 'HEAD')

    def test_free_business_schema_and_default_worktree(self):
        raw = '{"anything":[1,true,null,"自由格式"],"count":1e400}'.encode('utf-8')
        self.write(raw)
        code, report = self.check(default_repository=True)
        self.assertEqual(code, 0)
        self.assertTrue(report['format_pass'])
        self.assertEqual(report['requested_path'], 'agent/conversation.json')
        self.assertEqual(report['bytes'], len(raw))
        self.assertEqual(report['sha256'], hashlib.sha256(raw).hexdigest())
        self.assertIsNone(report['ref'])
        self.assertEqual(report['mirror']['status'], 'absent')

    def test_root_only_is_not_default_material(self):
        self.write(b'{}', 'conversation.json')
        code, report = self.check()
        self.assertNotEqual(code, 0)
        self.assertFalse(report['format_pass'])
        self.assertIn('missing_path', [item['code'] for item in report['errors']])

    def test_explicit_material_path(self):
        self.write(b'{}', 'notes/record.json')
        code, report = self.check('--material-path', 'notes/record.json')
        self.assertEqual(code, 0)
        self.assertEqual(report['requested_path'], 'notes/record.json')

    def test_case_is_strict_at_each_directory_and_filename(self):
        for path in ('Agent/conversation.json', 'agent/Conversation.json'):
            with self.subTest(path=path):
                target = self.write(b'{}', path)
                code, report = self.check()
                self.assertNotEqual(code, 0)
                self.assertIn('case_mismatch', [item['code'] for item in report['errors']])
                target.unlink()
                target.parent.rmdir()

    def test_unsafe_material_paths(self):
        self.write()
        for path in ('../conversation.json', '/agent/conversation.json',
                     'C:/agent/conversation.json', r'agent\conversation.json',
                     'agent/../conversation.json', './agent/conversation.json',
                     'agent//conversation.json', 'agent/conversation.json/', ''):
            with self.subTest(path=path):
                code, report = self.check('--material-path', path)
                self.assertNotEqual(code, 0)
                self.assertIn('unsafe_path', [item['code'] for item in report['errors']])

    def test_empty_official_placeholder_is_not_a_json_template(self):
        self.assert_failure(b'', 'invalid_json')

    def test_bom_is_rejected(self):
        self.assert_failure(b'\xef\xbb\xbf{}', 'utf8_bom')

    def test_non_utf8_is_rejected(self):
        self.assert_failure(b'{"x":"\xff"}', 'invalid_utf8')

    def test_duplicate_keys_are_rejected_at_any_depth(self):
        self.assert_failure(b'{"nested":{"x":1,"x":2}}', 'invalid_json')

    def test_nonstandard_constants_are_rejected(self):
        for token in ('NaN', 'Infinity', '-Infinity'):
            with self.subTest(token=token):
                self.assert_failure(('{"x":' + token + '}').encode('ascii'), 'invalid_json')

    def test_trailing_garbage_and_nonobjects_are_rejected(self):
        for raw in (b'[]', b'null', b'"x"', b'{} trailing', b'{}{}'):
            with self.subTest(raw=raw):
                self.assert_failure(raw, 'invalid_json')

    def test_exact_size_limit(self):
        raw = b'{"x":"' + b'x' * (128 * 1024 - 8) + b'"}'
        self.assertEqual(len(raw), 128 * 1024)
        self.write(raw)
        code, report = self.check()
        self.assertEqual(code, 0)
        self.assertTrue(report['format_pass'])
        self.assert_failure(raw + b' ', 'size_limit')

    def test_mirror_matching_and_local_mismatch(self):
        self.write(b'{}')
        self.write(b'{}', 'conversation.json')
        code, report = self.check()
        self.assertEqual(code, 0)
        self.assertEqual(report['mirror']['status'], 'match')
        self.write(b'{"different":true}', 'conversation.json')
        code, report = self.check()
        self.assertNotEqual(code, 0)
        self.assertTrue(report['format_pass'])
        self.assertEqual(report['mirror']['status'], 'mismatch')
        self.assertEqual(report['mirror']['scope'], 'local_mirror_check')
        self.assertIn('mirror_mismatch', [item['code'] for item in report['errors']])

    def test_mirror_identity_is_reported_even_for_invalid_json(self):
        self.write(b'')
        self.write(b'', 'conversation.json')
        code, report = self.check()
        self.assertNotEqual(code, 0)
        self.assertFalse(report['format_pass'])
        self.assertEqual(report['mirror']['status'], 'match')

    def test_root_selected_explicitly_is_not_its_own_mirror(self):
        self.write(b'{}', 'conversation.json')
        code, report = self.check('--material-path', 'conversation.json')
        self.assertEqual(code, 0)
        self.assertEqual(report['mirror']['status'], 'same_as_material')

    def test_optional_internal_pointer_quotes(self):
        self.write(json.dumps({'text': 'original quotation',
                               'evidence': {'pointer': '/text', 'quote': 'quotation'},
                               'arbitrary': {'pointer': 'not a quote reference'}}).encode('utf-8'))
        code, report = self.check('--check-internal-references')
        self.assertEqual(code, 0)
        self.assertEqual(report['internal_references']['checked'], 1)
        self.write(b'{"text":"original","evidence":{"pointer":"/text","quote":"wrong"}}')
        self.assertEqual(self.check()[0], 0)
        code, report = self.check('--check-internal-references')
        self.assertNotEqual(code, 0)
        self.assertTrue(report['format_pass'])
        self.assertIn('internal_reference', [item['code'] for item in report['errors']])

    def test_optional_pointer_escaping_and_list_index(self):
        self.write(b'{"a/b":[{"~name":"quoted"}],"e":{"pointer":"/a~1b/0/~0name","quote":"quoted"}}')
        self.assertEqual(self.check('--check-internal-references')[0], 0)
        for pointer in ('/a~1b/00/~0name', '/a~2b', '/a~1b/9', '/missing'):
            with self.subTest(pointer=pointer):
                self.write(json.dumps({'e': {'pointer': pointer, 'quote': 'x'}}).encode('utf-8'))
                self.assertNotEqual(self.check('--check-internal-references')[0], 0)

    def test_git_history_is_distinct_from_dirty_worktree(self):
        self.initialize_git()
        committed = b'{"source":"committed"}'
        self.write(committed)
        self.write(committed, 'conversation.json')
        ref = self.commit()
        expected_blob = self.git('rev-parse', ref + ':agent/conversation.json')
        dirty = b'{"source":"dirty"}'
        self.write(dirty)
        self.write(dirty, 'conversation.json')
        code, report = self.check('--ref', ref)
        self.assertEqual(code, 0)
        self.assertEqual(report['ref'], ref)
        self.assertEqual(report['sha256'], hashlib.sha256(committed).hexdigest())
        self.assertEqual(report['git_blob'], expected_blob)
        self.assertEqual(report['git_path'], 'agent/conversation.json')
        self.assertEqual(report['mirror']['status'], 'match')
        code, report = self.check()
        self.assertEqual(code, 0)
        self.assertEqual(report['sha256'], hashlib.sha256(dirty).hexdigest())

    def test_git_requires_full_commit_sha(self):
        self.initialize_git()
        self.write()
        ref = self.commit()
        for invalid in ('HEAD', ref[:12], '--help', '0' * 40):
            with self.subTest(ref=invalid):
                code, report = self.check('--ref=' + invalid)
                self.assertNotEqual(code, 0)
                self.assertIn('invalid_ref', [item['code'] for item in report['errors']])

    def test_git_exact_case_and_missing_entry(self):
        self.initialize_git()
        self.write(b'{}', 'Agent/conversation.json')
        self.write(b'{}', 'conversation.json')
        ref = self.commit()
        code, report = self.check('--ref', ref)
        self.assertNotEqual(code, 0)
        self.assertIn('case_mismatch', [item['code'] for item in report['errors']])
        code, report = self.check('--ref', ref, '--material-path', 'missing.json')
        self.assertNotEqual(code, 0)
        self.assertIn('missing_path', [item['code'] for item in report['errors']])

    def test_git_rejects_symbolic_link_mode_even_if_blob_is_json(self):
        self.initialize_git()
        self.write()
        self.commit()
        blob = self.git('hash-object', '-w', '--stdin', input=b'{}')
        self.git('update-index', '--cacheinfo', '120000,' + blob + ',agent/conversation.json')
        self.git('commit', '--quiet', '-m', 'Synthetic symlink entry')
        ref = self.git('rev-parse', 'HEAD')
        code, report = self.check('--ref', ref)
        self.assertNotEqual(code, 0)
        self.assertIn('not_regular_file', [item['code'] for item in report['errors']])

    def test_git_rejects_lfs_pointer(self):
        self.initialize_git()
        self.write(b'version https://git-lfs.github.com/spec/v1\noid sha256:' + b'0' * 64 + b'\nsize 2\n')
        ref = self.commit()
        code, report = self.check('--ref', ref)
        self.assertNotEqual(code, 0)
        self.assertIn('lfs_pointer', [item['code'] for item in report['errors']])

    def test_git_executable_regular_file_is_allowed(self):
        self.initialize_git()
        self.write()
        self.commit()
        self.git('update-index', '--chmod=+x', 'agent/conversation.json')
        self.git('commit', '--quiet', '-m', 'Synthetic executable regular file')
        code, report = self.check('--ref', self.git('rev-parse', 'HEAD'))
        self.assertEqual(code, 0)
        self.assertEqual(report['git_mode'], '100755')


if __name__ == '__main__':
    unittest.main(verbosity=2)
