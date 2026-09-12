#!/bin/sh
# Keeps the documented current schema version in
# schemas/VGEO_RESOURCE_SCHEMA.md in lockstep with kSchemaVersion in
# prototype/src/builder_internal.h. Run by CI (see
# .github/workflows/ci.yml).
set -eu

repo_root=$(cd "$(dirname "$0")/.." && pwd)

doc_version=$(sed -n 's/^- schema version (current: \([0-9]*\))$/\1/p' \
    "$repo_root/schemas/VGEO_RESOURCE_SCHEMA.md" | head -n 1)
code_version=$(sed -n 's/^constexpr uint32_t kSchemaVersion = \([0-9]*\);$/\1/p' \
    "$repo_root/prototype/src/builder_internal.h" | head -n 1)

if [ -z "$doc_version" ] || [ -z "$code_version" ]; then
    echo "check_schema_version: could not extract versions " \
         "(doc='${doc_version}' code='${code_version}')" >&2
    exit 1
fi

if [ "$doc_version" != "$code_version" ]; then
    echo "check_schema_version: documented schema version $doc_version != " \
         "kSchemaVersion $code_version" >&2
    exit 1
fi

echo "check_schema_version: schema version $doc_version consistent"
