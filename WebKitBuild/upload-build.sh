#!/usr/bin/env bash
# Publishes an ABKit build to build.stencil.so from GitHub Actions.
#
#   WebKitBuild/upload-build.sh VERSION FILE:PLATFORM:ARCH...
#
#   WebKitBuild/upload-build.sh 0.1.0 dist/abkit-0.1.0-darwin-arm64.tar.gz:macos:arm64
#
# Authenticates with the job's GitHub OIDC token (the workflow needs
# `permissions: id-token: write`) for audience $BUILD_AUDIENCE (default
# build.stencil.so), declares the build of product `abkit` with each file's
# name, size and sha256, PUTs every file to the presigned URL the service
# returns, then publishes the build. PLATFORM is macos, linux, linux-musl or
# windows; ARCH is arm64 or x86_64. Every file is an `archive`. $BUILD_URL
# (default https://build.stencil.so) selects the service. The service accepts
# only runs of stencil-hq/abkit for the tag `v<VERSION>`.
set -euo pipefail

if (($# < 2)); then
	echo "usage: $0 VERSION FILE:PLATFORM:ARCH..." >&2
	exit 2
fi
version=$1
shift
: "${ACTIONS_ID_TOKEN_REQUEST_URL:?not set: run in GitHub Actions with permissions: id-token: write}"
: "${ACTIONS_ID_TOKEN_REQUEST_TOKEN:?not set: run in GitHub Actions with permissions: id-token: write}"
url=${BUILD_URL:-https://build.stencil.so}
audience=${BUILD_AUDIENCE:-build.stencil.so}

# curl with the given arguments: prints the response body, or on an HTTP
# error prints it to stderr and fails.
request() {
	local body
	if ! body=$(curl --fail-with-body --silent --show-error "$@"); then
		printf '%s\n' "$body" >&2
		return 1
	fi
	printf '%s' "$body"
}

token=$(request --header "Authorization: bearer $ACTIONS_ID_TOKEN_REQUEST_TOKEN" \
	"$ACTIONS_ID_TOKEN_REQUEST_URL&audience=$(jq -rn --arg a "$audience" '$a | @uri')")
token=$(jq -r .value <<<"$token")

# Splits FILE:PLATFORM:ARCH into $file, $platform and $arch, from the right.
parse() {
	arch=${1##*:}
	local rest=${1%:*}
	platform=${rest##*:}
	file=${rest%:*}
	if [[ $rest == "$1" || $file == "$rest" || -z $platform || -z $arch || ! -f $file ]]; then
		echo "$0: $1: expected FILE:PLATFORM:ARCH naming an existing file" >&2
		exit 2
	fi
}

files='[]'
for spec in "$@"; do
	parse "$spec"
	size=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file")
	sha256=$(shasum -a 256 "$file" | cut -d' ' -f1)
	files=$(jq -c --arg name "$(basename "$file")" --arg platform "$platform" --arg arch "$arch" \
		--argjson size "$size" --arg sha256 "$sha256" \
		'. + [{name: $name, platform: $platform, arch: $arch, kind: "archive", size: $size, sha256: $sha256}]' <<<"$files")
done

created=$(jq -n --arg version "$version" --argjson files "$files" '{version: $version, files: $files}' |
	request --header "Authorization: Bearer $token" \
		--header "Content-Type: application/json" \
		--data-binary @- \
		"$url/api/products/abkit/builds")
id=$(jq -r .build.id <<<"$created")
echo "created abkit build $id ($version)"

for spec in "$@"; do
	parse "$spec"
	name=$(basename "$file")
	upload=$(jq -r --arg name "$name" '.uploads[] | select(.name == $name) | .url' <<<"$created")
	echo "uploading $name"
	request --retry 3 --upload-file "$file" "$upload"
done

published=$(request --retry 3 --header "Authorization: Bearer $token" \
	--data-binary '' \
	"$url/api/products/abkit/builds/$id/publish")
echo "published abkit build $id"
jq -r --arg url "$url" '.build.files[] | "  \(.kind)\t\($url)\(.url)"' <<<"$published"
