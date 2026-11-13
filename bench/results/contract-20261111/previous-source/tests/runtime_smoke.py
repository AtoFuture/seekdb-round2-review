"""Verify the default JSONL runtime image across independent container lifetimes."""
import json
import pathlib
import subprocess
import sys
import tempfile

image = sys.argv[1] if len(sys.argv) > 1 else 'minivectordb:dev'
root = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else '/data/runtime-tests').resolve()
root.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='runtime-', dir=root) as directory:
    args = ['docker', '--config', '/var/lib/minivectordb-docker-client',
            'run', '--rm', '--read-only', '--network', 'none', '--cpus', '2',
            '--memory', '512m', '-i', '--mount', f'type=bind,src={directory},dst=/data', image]
    def sql(statements, shutdown=True):
        requests = [dict(id=i,op='execute',sql=s,search_mode='exact') for i,s in enumerate(statements)]
        if shutdown: requests.append(dict(id=len(requests),op='shutdown'))
        result = subprocess.run(args,input=''.join(json.dumps(r)+'\n' for r in requests),
                                text=True,capture_output=True,timeout=45)
        assert result.returncode == 0, result.stdout + result.stderr
        output = [json.loads(line) for line in result.stdout.splitlines()]
        assert output.pop(0) == {'type':'ready'}
        assert len(output) == len(requests)
        for req,res in zip(requests,output):
            assert res['id'] == req['id'] and res['ok'] is True, res
        return output
    first = sql(['CREATE TABLE t(id BIGINT PRIMARY KEY,label VARCHAR(20),v VECTOR(2));',
                 'BEGIN;', "INSERT INTO t VALUES(1,'durable',[3,4]),(2,'second',[0,0]);",
                 'COMMIT;', 'BEGIN;', 'DELETE FROM t WHERE id=1;'], shutdown=False)
    assert first[2]['affected_rows'] == 2
    second = sql(['SELECT id,label,L2_DISTANCE(v,[0,0]) AS d FROM t ORDER BY d;'])
    assert second[0]['columns'] == ['id','label','d']
    assert second[0]['rows'] == [[2,'second',0],[1,'durable',5]]
    sql(['BEGIN;', 'DELETE FROM t;'])
    assert len(sql(['SELECT id FROM t;'])[0]['rows']) == 2
    assert (pathlib.Path(directory) / 'database.mvdb').is_file()
    print('PASS runtime: default JSONL, read-only root, network disabled, 2 CPU/512 MiB, persistence and EOF/shutdown rollback')
