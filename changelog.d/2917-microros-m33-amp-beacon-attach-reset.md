### Fixed — publish the AMP beacon and honour the attach reset on the M33 (#2917)

The micro-ROS M33 firmware published no AMP beacon and never re-created its
virtio device, but its A55 bridge uses yocto_uio_drv, which fails alp_rpc_open()
with ALP_ERR_NOT_READY without the beacon magic, waits for an even attach epoch
on a re-attach, and zeroes the vrings (leaving this side's ring indices stale).

Port the rpmsg-v2n beacon v2 + attach-reset handshake into rpmsg_link.c:
publish the beacon first, bump the epoch odd when bound, and on a doorbell with
DRIVER_OK clear tear the vdev down, bump the epoch even and rebuild it.
The carried micro-ROS patches are untouched.
