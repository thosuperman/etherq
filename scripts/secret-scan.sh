#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pre-publication scan for secrets and personal data in the working tree and in
# git history. Exits non-zero if anything suspicious is found. Matches are
# printed with the secret part masked.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PATTERNS=(
  '-----BEGIN [A-Z ]*PRIVATE KEY-----'
  'AKIA[0-9A-Z]{16}'                              # AWS access key id
  'aws_secret_access_key'
  'gh[pousr]_[A-Za-z0-9]{30,}'                    # GitHub tokens
  'xox[abp]-[A-Za-z0-9-]{10,}'                    # Slack tokens
  'sk-[A-Za-z0-9]{20,}'                           # generic API keys
  '-myqnx\.password +[^*" $]'                     # unredacted QNX Software Center logs (not "$VAR")
  '[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}'  # QNX license key format
  '[Pp]assword *[:=] *[^ *]{6,}'
  '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' # e-mail addresses
  '/Users/[a-z]'                                  # personal macOS paths
  '\b(13|18|3|35|43|52|54)\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\b'  # public cloud IPv4 ranges seen in this project
)
# addresses that are fine to publish
ALLOW='(XXXX-XXXX|XXXX-\*\*\*\*|noreply@|@users\.noreply\.github\.com|support@devpost\.com|sales@rt-labs\.com|info\.soem@rt-labs\.com|myqnx@blackberry\.com|example\.(com|org)|@[0-9]+\.[0-9]|git@github\.com)'

FOUND_FILE="$(mktemp)"
trap 'rm -f "$FOUND_FILE"' EXIT
scan() {   # $1 = description, stdin = "path:line:text"  (runs in a pipeline subshell)
  while IFS= read -r line; do
    if echo "$line" | grep -qE "$ALLOW"; then continue; fi
    echo "  $1: $(echo "$line" | cut -c1-160 | sed -E 's/([A-Z0-9]{4}-)[A-Z0-9-]{14,}/\1****/g; s/(password[^ ]* +)[^ ]+/\1****/Ig')"
    echo x >> "$FOUND_FILE"
  done
}

echo "== working tree"
for p in "${PATTERNS[@]}"; do
  git ls-files -co --exclude-standard 2>/dev/null | grep -vE '^(LICENSE|build/|scripts/secret-scan\.sh$)' | \
    xargs -r grep -nIE -- "$p" 2>/dev/null | scan "tree"
done

if git rev-parse --git-dir >/dev/null 2>&1; then
  echo "== git history (all commits)"
  for p in "${PATTERNS[@]}"; do
    git log --all -p --no-color -- . ':(exclude)scripts/secret-scan.sh' 2>/dev/null | grep -nE -- "^\+.*($p)" | grep -v '^[0-9]*:+++ ' | scan "history"
  done
  echo "== commit metadata"
  git log --all --format='%an <%ae> | %cn <%ce>' | sort -u | sed 's/^/  /'
fi

if [ -s "$FOUND_FILE" ]; then echo "secret-scan: REVIEW THE FINDINGS ABOVE"; exit 1; fi
echo "secret-scan: no findings"
exit 0
