### Fixed — update-log: clear stale owner request on restart, cache-maintain the AEN mailbox, report NVS mount failure (#2921, #2922, #2926)

aen_m55_owner: the HP owner reset only RSP->seq and last_seq at start, so a
request already served by the previous owner and still retained in SRAM0
(REQ->seq != 0) was treated as new and its APPEND replayed into the log.
Clear REQ->seq too. The HE/HP mailbox at 0x02003000/0x02003100 was shared
with plain volatile accesses and only worked with CONFIG_DCACHE=n; the
writer now flushes the payload, fences, then flushes the seq line, and the
reader invalidates before reading (no-ops when the D-cache is off).

sw_tier: with an alp_ulog_partition, any NVS mount failure latched a RAM
store for the rest of the boot, so append() returned ALP_OK for entries lost
on reboot. A failed mount is now reported (ALP_ERR_NOT_READY from ready() and
every store/counter call) and retried on the next call; the RAM store is only
used when the build has no partition. Adds a ztest covering failure then
recovery.
