#!/bin/sh
# Notarizes a macOS engine directory that package-engine.sh signed with a
# Developer ID: submits it to Apple's notary service as a zip and waits for the
# verdict, printing the service's log when it refuses. Loose executables and
# libraries hold no stapled ticket; Gatekeeper fetches theirs from Apple.
# Usage: APPLE_API_KEY_ID=ID APPLE_API_ISSUER_ID=ISSUER notarize-engine.sh ENGINE_DIR KEY_P8
#   (KEY_P8 is the App Store Connect API key file of that ID and issuer)
set -e
engine=$1
key=$2
if [ ! -d "$engine" ] || [ ! -f "$key" ] || [ -z "${APPLE_API_KEY_ID:-}" ] || [ -z "${APPLE_API_ISSUER_ID:-}" ]; then
	echo "usage: APPLE_API_KEY_ID=ID APPLE_API_ISSUER_ID=ISSUER $0 ENGINE_DIR KEY_P8" >&2
	exit 2
fi
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
ditto -c -k --keepParent "$engine" "$stage/abkit.zip"
notary() {
	xcrun notarytool "$@" --key "$key" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER_ID"
}
field() {
	sed -n "s/.*\"$1\" *: *\"\([^\"]*\)\".*/\1/p" "$stage/result.json"
}
# Apple's notary requests time out now and then while the submission carries
# on, so a submission that got no verdict is retried; a verdict is final.
for attempt in 1 2 3; do
	if notary submit "$stage/abkit.zip" --wait --timeout 45m --output-format json >"$stage/result.json"; then
		:
	fi
	status=$(field status)
	case $status in
	Accepted)
		echo "notarized: submission $(field id)"
		exit 0
		;;
	Invalid | Rejected)
		echo "notarization refused ($status): submission $(field id)" >&2
		notary log "$(field id)" >&2 || true
		exit 1
		;;
	esac
	echo "notarize-engine: attempt $attempt got no verdict: $(cat "$stage/result.json")" >&2
	sleep 30
done
exit 1
