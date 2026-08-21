#!/usr/bin/env bash
# Прогон хостовых тестов. Плата не нужна.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec cmd.exe /c "$(cygpath -w "$ROOT/tools/run_tests.cmd")"
