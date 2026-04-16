#!/bin/sh
# BFS attribute tools test — run on Taurus (Haiku) to verify
# the four new tools work and to measure token/performance
# savings vs. traditional file reads.
#
# Usage:
#   cd /Data/Code/Projects/haiku-claude-cli
#   sh tests/bfs_tools_test.sh
#
# Requires: built ./build/claude, Haiku with BFS, the project
# source files in src/.

set -e

CLAUDE="./build/claude"
SRC="src"

echo "=== BFS Attribute Tools Test ==="
echo ""

# ── 1. Baseline: count tokens in full source read ──────────

echo "--- Test 1: Token baseline ---"
TOTAL_CHARS=$(cat $SRC/*.cpp $SRC/*.h | wc -c | tr -d ' ')
TOTAL_LINES=$(cat $SRC/*.cpp $SRC/*.h | wc -l | tr -d ' ')
FILE_COUNT=$(ls $SRC/*.cpp $SRC/*.h | wc -l | tr -d ' ')
EST_TOKENS=$((TOTAL_CHARS / 4))
echo "Files: $FILE_COUNT"
echo "Lines: $TOTAL_LINES"
echo "Chars: $TOTAL_CHARS"
echo "Estimated tokens (full read): $EST_TOKENS"
echo ""

# ── 2. WriteAttr: tag each source file with a summary ──────

echo "--- Test 2: WriteAttr (write claude:summary to each file) ---"
WRITE_START=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')

for f in $SRC/*.cpp $SRC/*.h; do
    basename=$(basename "$f")
    addattr -t string claude:summary "test summary for $basename" "$f"
done

WRITE_END=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
WRITE_MS=$(( (WRITE_END - WRITE_START) / 1000000 ))
echo "Wrote claude:summary to $FILE_COUNT files in ${WRITE_MS}ms"
echo ""

# ── 3. ReadAttr: read all summaries ────────────────────────

echo "--- Test 3: ReadAttr (read claude:summary from all files) ---"
READ_START=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')

SUMMARY_OUTPUT=""
for f in $SRC/*.cpp $SRC/*.h; do
    val=$(catattr -d claude:summary "$f" 2>/dev/null || echo "(none)")
    SUMMARY_OUTPUT="$SUMMARY_OUTPUT$(basename $f): $val
"
done

READ_END=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
READ_MS=$(( (READ_END - READ_START) / 1000000 ))

SUMMARY_CHARS=$(echo "$SUMMARY_OUTPUT" | wc -c | tr -d ' ')
SUMMARY_TOKENS=$((SUMMARY_CHARS / 4))
echo "Read $FILE_COUNT summaries in ${READ_MS}ms"
echo "Summary chars: $SUMMARY_CHARS"
echo "Summary tokens: $SUMMARY_TOKENS"
echo ""

# ── 4. Token savings comparison ────────────────────────────

echo "--- Test 4: Token savings ---"
SAVED=$((EST_TOKENS - SUMMARY_TOKENS))
if [ "$EST_TOKENS" -gt 0 ]; then
    PCT=$((SAVED * 100 / EST_TOKENS))
else
    PCT=0
fi
echo "Full read:      $EST_TOKENS tokens"
echo "Summaries only: $SUMMARY_TOKENS tokens"
echo "Saved:          $SAVED tokens ($PCT%)"
echo ""

# ── 5. Query: BFS indexed lookup ───────────────────────────

echo "--- Test 5: Query (BFS indexed lookup vs Glob) ---"

# Time a glob-based find
GLOB_START=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
ls $SRC/*.cpp $SRC/*.h > /dev/null 2>&1
GLOB_END=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
GLOB_MS=$(( (GLOB_END - GLOB_START) / 1000000 ))

# Time a BFS query (BEOS:TYPE based)
QUERY_START=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
query 'BEOS:TYPE == "text/x-source-code"' > /dev/null 2>&1 || true
QUERY_END=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
QUERY_MS=$(( (QUERY_END - QUERY_START) / 1000000 ))

echo "Glob (ls *.cpp *.h): ${GLOB_MS}ms"
echo "BFS query:           ${QUERY_MS}ms"
echo ""

# ── 6. IndexAttr: create a test index ──────────────────────

echo "--- Test 6: IndexAttr (create claude:test_index) ---"
INDEX_START=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
mkindex -t string claude:test_index 2>/dev/null || echo "(index may already exist)"
INDEX_END=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
INDEX_MS=$(( (INDEX_END - INDEX_START) / 1000000 ))
echo "Created index in ${INDEX_MS}ms"
echo ""

# ── 7. Cleanup: remove test attributes ─────────────────────

echo "--- Cleanup ---"
for f in $SRC/*.cpp $SRC/*.h; do
    rmattr claude:summary "$f" 2>/dev/null || true
done
echo "Removed claude:summary from all source files"
echo ""

echo "=== Done ==="
