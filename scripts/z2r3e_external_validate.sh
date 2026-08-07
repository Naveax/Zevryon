#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/z2r3e_external_validate.sh <40-char-head> [compiler] [output-directory]

Defaults:
  compiler: gcc-release
  output-directory: $HOME/Downloads when present, otherwise current directory
EOF
}

if [[ $# -lt 1 || $# -gt 3 ]]; then
  usage >&2
  exit 64
fi

expected_head="${1,,}"
compiler="${2:-gcc-release}"
if [[ -d "${HOME:-}/Downloads" ]]; then
  default_output="${HOME}/Downloads"
else
  default_output="$PWD"
fi
output_directory="${3:-$default_output}"
repository_url="${ZEVRYON_REPOSITORY_URL:-https://github.com/Naveax/Zevryon.git}"

if [[ ! "$expected_head" =~ ^[0-9a-f]{40}$ ]]; then
  echo "Expected head must be a 40-character hexadecimal commit SHA." >&2
  exit 64
fi

for tool in git python3 cmake ctest cargo rustc tar sha256sum; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required tool is unavailable: $tool" >&2
    exit 69
  fi
done

short_head="${expected_head:0:12}"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/zevryon-z2r3eu-${short_head}-XXXXXX")"
clone_root="$work_root/repo"
evidence_root="$clone_root/evidence/z2r3eu"
summary_path="$evidence_root/linux-authority-validation.json"
package_base="zevryon-z2r3eu-linux-${short_head}-${stamp}"
archive_path="$output_directory/${package_base}.tar.gz"
hash_path="${archive_path}.sha256"
validation_status=1

mkdir -p "$output_directory"

preserve_evidence() {
  local status=$?
  trap - EXIT
  if [[ -d "$evidence_root" ]]; then
    tar -C "$evidence_root" -czf "$archive_path" .
    (
      cd "$output_directory"
      sha256sum "$(basename "$archive_path")" >"$(basename "$hash_path")"
    )
    printf 'Evidence archive: %s\n' "$archive_path"
    printf 'SHA-256 file:    %s\n' "$hash_path"
  fi
  if [[ $validation_status -ne 0 ]]; then
    exit "$validation_status"
  fi
  exit "$status"
}
trap preserve_evidence EXIT

git clone \
  --no-tags \
  --filter=blob:none \
  --single-branch \
  --branch agent/z2r3eu-unicode-authority \
  "$repository_url" \
  "$clone_root"

cd "$clone_root"
git fetch --no-tags origin "$expected_head"
git checkout --detach "$expected_head"

actual_head="$(git rev-parse HEAD | tr '[:upper:]' '[:lower:]')"
if [[ "$actual_head" != "$expected_head" ]]; then
  echo "Exact-head mismatch. Expected $expected_head, got $actual_head" >&2
  exit 65
fi

if [[ -n "$(git status --porcelain=v1)" ]]; then
  echo 'Validation checkout is not clean.' >&2
  exit 65
fi

mkdir -p "$evidence_root"
cat >"$evidence_root/external-run.txt" <<EOF
expected_head=$expected_head
actual_head=$actual_head
repository=$repository_url
platform=linux
compiler=$compiler
started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

set +e
python3 scripts/z2r3e_validate_unicode_authority.py \
  --sha "$expected_head" \
  --platform linux \
  --compiler "$compiler" \
  --output "$summary_path"
validation_status=$?
set -e

if [[ $validation_status -ne 0 ]]; then
  echo "Z2R-3E-U Linux validation failed with exit code $validation_status. Evidence will be preserved." >&2
  exit "$validation_status"
fi

printf 'Z2R-3E-U Linux exact-head validation passed: %s\n' "$expected_head"
