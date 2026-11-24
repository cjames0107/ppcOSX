#!/bin/sh
# Run a command in the development guest over SSH (port 2222 -> guest 22;
# PPCOSX_SSH_PORT picks another, for a second guest).
# Tiger's OpenSSH only speaks algorithms that modern clients disable.
root=$(cd "$(dirname "$0")/.." && pwd)
exec ssh -p "${PPCOSX_SSH_PORT:-2222}" -i "$root/vms/guest_key" -o BatchMode=yes \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR -o ConnectTimeout=20 \
    -o KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o Ciphers=+aes128-cbc,3des-cbc -o MACs=+hmac-sha1 \
    ppcosx@127.0.0.1 "$@"
