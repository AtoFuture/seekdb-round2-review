param(
    [ValidateSet('Status','Build','Test','Image','ContainerTest','RuntimeImage','RuntimeTest','StartDocker')]
    [string]$Action = 'Test'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project = Split-Path -Parent $repo
if ([IO.Path]::GetPathRoot($project) -ne 'D:\') { throw 'All project artifacts must remain on D:.' }
$env:TEMP = Join-Path $project 'tmp'
$env:TMP = $env:TEMP
$env:TMPDIR = $env:TEMP
$env:PYTHONDONTWRITEBYTECODE = '1'
$linuxProject = '/mnt/d' + $project.Substring(2).Replace('\','/')
$linuxRepo = $linuxProject + '/repo'
$linuxBuild = $linuxProject + '/build/core'
$linuxPath = 'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin'
function Invoke-Linux {
    & wsl.exe --distribution MiniVectorDB --exec /usr/bin/env $linuxPath @args
    if ($LASTEXITCODE -ne 0) { throw "Linux command failed with exit code $LASTEXITCODE" }
}
function Invoke-Docker {
    Invoke-Linux docker --config /var/lib/minivectordb-docker-client @args
}
switch ($Action) {
    'Status' {
        Invoke-Docker info --format '{{.ServerVersion}} {{.OSType}} {{.Architecture}} {{.DockerRootDir}}'
        Invoke-Linux g++ --version
        Invoke-Linux cmake --version
    }
    'StartDocker' {
        $logDir = Join-Path $project 'work/environment'
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
        $startArgs = @('--distribution','MiniVectorDB','--exec','/bin/sh',($linuxRepo+'/scripts/start-docker.sh'))
        Start-Process -FilePath 'wsl.exe' -WindowStyle Hidden -WorkingDirectory $project -ArgumentList $startArgs -RedirectStandardOutput (Join-Path $logDir 'dockerd.stdout.log') -RedirectStandardError (Join-Path $logDir 'dockerd.stderr.log')
        Write-Output 'Launch requested; run -Action Status to verify readiness.'
    }
    'Build' {
        Invoke-Linux cmake -S $linuxRepo -B $linuxBuild -G Ninja -DCMAKE_BUILD_TYPE=Release
        Invoke-Linux cmake --build $linuxBuild --parallel 2
    }
    'Test' {
        Invoke-Linux env ("MVDB_TEST_DIR="+$linuxProject+"/data/core-tests") ctest --test-dir $linuxBuild --output-on-failure
    }
    'Image' {
        Invoke-Docker build --network host --target test -t minivectordb-core:dev $linuxRepo
    }
    'RuntimeImage' {
        Invoke-Docker build --network host --target runtime -t minivectordb:dev $linuxRepo
    }
    'RuntimeTest' {
        Invoke-Linux python3 ($linuxRepo+'/tests/runtime_smoke.py') minivectordb:dev ($linuxProject+'/data/runtime-tests')
    }
    'ContainerTest' {
        Invoke-Docker run --rm --network none --cpus 2 --memory 512m --mount ("type=bind,src="+$linuxProject+"/data,dst=/data") minivectordb-core:dev
    }
}
