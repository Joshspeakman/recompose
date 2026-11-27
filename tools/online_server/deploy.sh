#!/bin/sh
# Sets up the online server on Fly.io, from this folder, and prints its address.
#
#     sh deploy.sh [name] [region]
#
# Needs flyctl (macOS: brew install flyctl; macOS or Linux: curl -L https://fly.io/install.sh | sh)
# and a Fly.io account. It signs in if needed (a browser opens), makes the app (or uses it if
# it is already yours), gives it a dedicated IPv4 address if it has none (UDP needs one; $2 a
# month at Fly's prices), writes fly.toml from fly.toml.template and deploys one machine.
# Run again after an update to deploy the new server; nothing is made twice.
# Remove it with: flyctl apps destroy <name>
set -e
cd "$(dirname "$0")"

fly=""
for c in flyctl fly; do
    if command -v "$c" >/dev/null 2>&1; then fly=$(command -v "$c"); break; fi
done
[ -z "$fly" ] && [ -x "$HOME/.fly/bin/flyctl" ] && fly="$HOME/.fly/bin/flyctl"
if [ -z "$fly" ]; then
    echo "flyctl is not installed. Install it with:"
    echo "    brew install flyctl                       (macOS, with Homebrew)"
    echo "    curl -L https://fly.io/install.sh | sh    (macOS or Linux)"
    echo "then open a new terminal and run this again."
    exit 1
fi

if ! "$fly" auth whoami >/dev/null 2>&1; then
    echo "Signing in to Fly.io: a browser opens."
    "$fly" auth login
fi

name=${1:-}
if [ -z "$name" ]; then
    suggest="timeslip-$(LC_ALL=C tr -dc 'a-z0-9' </dev/urandom | head -c 6)"
    printf "A name for the server: letters, digits and dashes, unused on Fly (it becomes name.fly.dev) [%s]: " "$suggest"
    read -r name
    [ -z "$name" ] && name=$suggest
fi
name=$(printf '%s' "$name" | tr 'A-Z' 'a-z')
case "$name" in
    *[!a-z0-9-]*|-*|*-|?|??) echo "A name has only letters, digits and dashes, 3 to 63 of them."; exit 1 ;;
esac

region=${2:-}
if [ -z "$region" ]; then
    echo "Fly's regions:"
    "$fly" platform regions
    printf "The code of the region nearest you and your friends (like iad, ord, dfw, lax, lhr, fra, syd): "
    read -r region
fi
region=$(printf '%s' "$region" | tr 'A-Z' 'a-z')
case "$region" in
    [a-z][a-z][a-z]) ;;
    *) echo "A region is a three-letter code from the list."; exit 1 ;;
esac

if ! "$fly" status -a "$name" >/dev/null 2>&1; then
    echo "Making the app $name..."
    "$fly" apps create --name "$name" --org personal --yes
fi

sed -e "s/__APP__/$name/" -e "s/__REGION__/$region/" fly.toml.template > fly.toml

# A dedicated IPv4 address, once: UDP does not reach an app on the shared one.
if ! "$fly" ips list -a "$name" --json 2>/dev/null | grep -Eq '"[Tt]ype": *"v4"'; then
    echo "Giving it a dedicated IPv4 address (UDP needs one)..."
    "$fly" ips allocate-v4 -a "$name" --yes
fi

echo "Deploying: Fly builds the server and starts one machine (a few minutes the first time)..."
"$fly" deploy -a "$name" -c fly.toml --ha=false --remote-only

echo
echo "The online server is running."
echo "In the launcher: Play Together > Connect: Through an online server"
echo "    Server address:  $name.fly.dev"
echo "Everyone playing types the same Server address and Session code."
echo "To remove it, and its cost: flyctl apps destroy $name"
