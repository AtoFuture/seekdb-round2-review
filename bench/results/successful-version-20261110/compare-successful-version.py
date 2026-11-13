"""Replay identical JSONL traffic through the unchanged official transport."""
import hashlib
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'official-protocol-f4d4e26' / 'protocol_check'))
import _transport

assert __debug__
OUT = ROOT / 'successful-version-20261110'
OUT.mkdir(exist_ok=True)
CONFIG = json.loads((ROOT / 'official-protocol-f4d4e26/protocol_check/config.json').read_text())
ORIGINAL_JSON = _transport.strict_json
CAPTURE = []


def capture(text):
    value = ORIGINAL_JSON(text)
    CAPTURE.append(text)
    return value


_transport.strict_json = capture


def run(image, label):
    global CAPTURE
    CAPTURE = []
    steps = []
    checks = 0
    session = _transport.Session(CONFIG, OUT / (label + '-stderr.log'), image=image,
                                 case_timeout=600, data_root=str(ROOT.parent / 'data'))

    def query(sql, expected=None, ok=True, ident=None, mode='exact'):
        nonlocal checks
        response = session.request(sql, mode=mode, ident=ident)
        steps.append({'sql': sql, 'mode': mode, 'response': response})
        assert response['ok'] is ok, (label, sql, response)
        if not ok:
            assert isinstance(response['error']['code'], str) and response['error']['code']
        if expected is not None:
            assert response['rows'] == expected, (label, sql, response, expected)
        checks += 1
        return response

    try:
        session.start()
        query('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(300000),v VECTOR(2));')
        query('CREATE VECTOR INDEX tv ON t(v);')
        query("INSERT INTO t VALUES(-9223372036854775808,'quote'';slash\\tab\tline\n',[0,0]),"
              "(9223372036854775807,'中文😀',[3,4]);")
        expected = [[-9223372036854775808, "quote';slash\\tab\tline\n", [0, 0]],
                    [9223372036854775807, '中文😀', [3, 4]]]
        query('SELECT * FROM t ORDER BY id;', expected)
        for ident in [0, -5, 9223372036854775807, '1', '中文\n"\\', ['x', 7], {'k': [1, True]}, True]:
            query('SELECT id FROM t WHERE id=0;', [], ident=ident)
        for mode in ['exact', 'ann']:
            for _ in range(4):
                query("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 2;",
                      [[-9223372036854775808, 0], [9223372036854775807, 5]], mode=mode)
        query("INSERT INTO t VALUES(8,'atomic',[1,1]),(-9223372036854775808,'duplicate',[1,1]);", ok=False)
        query('SELECT id FROM t WHERE id=8;', [])
        query('BEGIN;')
        query("UPDATE t SET v='[6,8]' WHERE id=9223372036854775807;")
        query("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 2;",
              [[-9223372036854775808, 0], [9223372036854775807, 10]], mode='ann')
        query('ROLLBACK;')
        query('SELECT * FROM t ORDER BY id;', expected)
        query('BEGIN;')
        query("INSERT INTO t VALUES(11,'commit',[1,2]);")
        query('COMMIT;')
        query('BEGIN;')
        query("INSERT INTO t VALUES(12,'shutdown rollback',[1,2]);")
        session.shutdown()
        session.start()
        query('SELECT id FROM t WHERE id=11;', [[11]])
        query('SELECT id FROM t WHERE id=12;', [])
        query('BEGIN;')
        query('DELETE FROM t;')
        session.crash()
        session.start()
        query('SELECT id FROM t ORDER BY id;', [[-9223372036854775808], [11], [9223372036854775807]])
        query('CREATE TABLE bounds(id BIGINT PRIMARY KEY,v VECTOR(2));')
        query("INSERT INTO bounds VALUES(1,'[3.4028235e38,0]'),(2,'[-3.4028235e38,0]');")
        maximum = query('SELECT v FROM bounds WHERE id=1;')['rows'][0][0]
        query("INSERT INTO bounds VALUES(3,'%s');" % json.dumps(maximum, separators=(',', ':')))
        query("INSERT INTO bounds VALUES(4,'[3.4028236e38,0]');", ok=False)
        query('SELECT id FROM bounds ORDER BY id;', [[1], [2], [3]])
        # Large result goes through response spool; large id must retain exact type/value.
        large = 'a' * 70000
        query("INSERT INTO t VALUES(21,'%s',[1,1]),(22,'%s',[2,2]);" % (large, large), ident='i' * 65510)
        query('SELECT id,s FROM t WHERE id>=21 ORDER BY id;', [[21, large], [22, large], [9223372036854775807, '中文😀']])
        for malformed in [{'id': 'bad-op', 'op': 'no-such-op'}, {'id': 'bad-mode', 'op': 'execute', 'sql': 'SELECT * FROM t;', 'search_mode': 'bogus'}]:
            session.send(malformed)
            response = session.read(CONFIG['request_timeout'])
            assert response['id'] == malformed['id'] and response['ok'] is False
            assert isinstance(response['error']['code'], str) and response['error']['code']
            checks += 1
        query('SELECT id FROM t WHERE id=11;', [[11]])
        # Record intentional semantic fixes separately from common protocol equivalence.
        underflow = session.request("INSERT INTO bounds VALUES(5,'[1e-400,-1e-400]');")
        underflow_rows = session.request('SELECT v FROM bounds WHERE id=5;')
        query('CREATE TABLE ties(id BIGINT PRIMARY KEY,v VECTOR(2));')
        query("INSERT INTO ties VALUES(1,'[1,1.5e-8]'),(2,'[1,0]');")
        ties = session.request("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM ties ORDER BY d LIMIT 2;")
        session.shutdown()
        raw = ('\n'.join(CAPTURE) + '\n').encode('utf-8')
        (OUT / (label + '-responses.jsonl')).write_bytes(raw)
        result = {'label': label, 'image': image, 'common_checks': checks, 'captured_messages': len(CAPTURE),
                  'wire_sha256': hashlib.sha256(raw).hexdigest(), 'raw_stdout_bytes': len(raw),
                  'wire_line_sha256': [hashlib.sha256(s.encode('utf-8')).hexdigest() for s in CAPTURE],
                  'normal_restarts': 1, 'crash_restarts': 1,
                  'underflow': underflow, 'underflow_rows': underflow_rows, 'distance_ties': ties,
                  'common_responses': [s['response'] for s in steps]}
        (OUT / (label + '-result.json')).write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
        return result
    finally:
        session.close()


if __name__ == '__main__':
    old = run(sys.argv[1], 'platform-success-8b7054')
    new = run(sys.argv[2], 'latest-optimized-local')
    assert old['common_responses'] == new['common_responses']
    assert old['underflow']['ok'] is True
    assert new['underflow']['ok'] is True
    assert new['underflow_rows']['rows'] == [[[0, 0]]]
    assert new['distance_ties']['rows'] == [[1, 1], [2, 1]]
    assert old['distance_ties']['rows'] == [[2, 1], [1, 1]]
    wire_differences = [i for i, (a, b) in enumerate(zip(old['wire_line_sha256'], new['wire_line_sha256'])) if a != b]
    assert old['captured_messages'] == new['captured_messages']
    assert wire_differences == [new['captured_messages'] - 2], wire_differences
    differences = {k: {'old': old[k], 'new': new[k]} for k in ['underflow', 'underflow_rows', 'distance_ties'] if old[k] != new[k]}
    summary = {'passed': True, 'common_responses_equal': True, 'checks_per_version': new['common_checks'],
               'normal_restarts_total': 2, 'crash_restarts_total': 2, 'intentional_differences': differences,
               'wire_messages_per_version': new['captured_messages'], 'wire_different_line_indexes': wire_differences,
               'old_image': old['image'], 'new_image': new['image'],
               'scope': 'Local differential validation; platform aggregate does not provide raw scores.'}
    (OUT / 'comparison.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(summary, ensure_ascii=False), flush=True)
