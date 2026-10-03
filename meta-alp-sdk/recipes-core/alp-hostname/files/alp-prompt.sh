# shellcheck shell=sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB
#
# /etc/profile.d snippet: keep the unit serial out of the interactive prompt.
#
# alp-hostname-set.sh names a provisioned board "<sku>-<serial>", for example
# "e1m-v2m103-2026w38-0001". The hostname stays that way (it is unique on the
# network and the provisioning tool keys on it), but the root prompt shows only
# "root@e1m-v2m103:~# ". A hostname that does not end in a "-<yyyy>w<ww>-<xxxx>"
# serial is left to the distro's own prompt (\h). Serial console logins and SSH
# sessions are both login shells, so both read this file.
#
# ALP_PROMPT_HOSTNAME_FILE overrides the hostname source for
# tests/scripts/test_alp_hostname_set.py only.

if [ -n "${PS1-}" ]; then
	_alp_host=$(cat "${ALP_PROMPT_HOSTNAME_FILE:-/proc/sys/kernel/hostname}" 2>/dev/null)
	_alp_short=${_alp_host%-[0-9][0-9][0-9][0-9]w[0-9][0-9]-[0-9a-z][0-9a-z][0-9a-z][0-9a-z]}
	if [ -n "$_alp_short" ] && [ "$_alp_short" != "$_alp_host" ]; then
		PS1="\\u@${_alp_short}:\\w\\\$ "
	fi
	unset _alp_host _alp_short
fi
