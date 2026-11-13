# Evidence scope

`snapshot-final-*` contains the final probation-policy same-binary on/off experiments. `snapshot-pilot`, `snapshot-dense`, and `snapshot-varied` preserve the earlier immediate-build policy, including regressions. Their historical interleaved file/CPU counters have the limitations described in `docs/scan-snapshot-results.md`.

`jsonl/` contains all six final default-runtime samples and workload metadata. The baseline is the existing `bench/results/paired-20261110` archive. `probe-float-d55b183` records the failing baseline numeric probe; `snapshot-numeric-probe` records the repaired runtime. `snapshot-internal-initial-failure.log` deliberately retains an unsuccessful intermediate run; `snapshot-internal.log` is the final passing run.

The copied `snapshot-resource-check.py` is the exact external host driver used. For replay, place it in an empty D-backed work directory with the official `f4d4e26653af823a7601668802f80e374ad6643c` protocol package at `official-protocol-f4d4e26/protocol_check/`, including `_transport.py` and `config.json`. Run it under the project's dedicated Linux/Docker environment with the runtime image ID as its argument. It creates `snapshot-resource/` once and stores Docker data under `/var/lib/minivectordb-snapshot-resource`, inside the dedicated D-drive WSL disk. The archived script is not intended to run directly from this evidence directory. Use a new work directory for another run; do not overwrite evidence.

The manifest hashes the archived bytes, excluding itself. Logs have only line endings and trailing whitespace normalized. Source and image identifiers are local working-tree evidence, not a formal submission or score.
