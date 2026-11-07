### Fixed — update-log: an HE restart no longer collides with the retained AEN mailbox seq (Refs #2921)

aen_m55_owner: after an HE-only reset the client restarted its request seq at
1 while SRAM0 still held RSP->seq and the owner's last_seq from before, so the
first request could be answered by the stale response without ever reaching the
owner, or be skipped by the owner as already seen. The client now seeds its seq
from the retained RSP->seq on its first call. A host test drives HE-only,
HP-only and both-core restarts against a buffer standing in for SRAM0.
