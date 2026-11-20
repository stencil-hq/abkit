# Writes OUTPUT, a source that defines the build's identity, which ABHost's welcome reports:
# AgentHost::buildCommit, the commit of SOURCE_DIR (`-dirty` when its tracked files differ from it,
# `unknown` without git), and AgentHost::buildDate, when this ran (UTC; SOURCE_DATE_EPOCH, when set).
# Run on every build (cmake -DSOURCE_DIR=… -DOUTPUT=… -P BuildIdentity.cmake); OUTPUT is left
# alone when it says the same.

execute_process(
    COMMAND git -C "${SOURCE_DIR}" describe --always --dirty --abbrev=11
    OUTPUT_VARIABLE commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE result
)
if (NOT result EQUAL 0 OR commit STREQUAL "")
    set(commit "unknown")
endif ()
string(TIMESTAMP date "%Y-%m-%dT%H:%M:%SZ" UTC)

set(source "// Written by BuildIdentity.cmake on every build.
namespace AgentHost {
extern const char buildCommit[];
extern const char buildDate[];
const char buildCommit[] = \"${commit}\";
const char buildDate[] = \"${date}\";
} // namespace AgentHost
")
if (EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" previous)
endif ()
if (NOT previous STREQUAL source)
    file(WRITE "${OUTPUT}" "${source}")
endif ()
