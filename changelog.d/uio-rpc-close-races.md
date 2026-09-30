### Fixed — A55 OpenAMP-over-UIO RPC backend: close/unsubscribe races and partial-init teardown

- **External close no longer frees the channel under a running IRQ handler.** The close drain now waits on a whole-handler in-flight count, not only the endpoint-callback window, so the MHU ack, notification pump and epilogue can no longer touch a freed channel.
- **A subscribe callback that sends or calls can no longer deadlock the IRQ thread.** `alp_rpc_call` serialises on a lock the IRQ thread never takes and releases the TX lock right after the send, so a pending call no longer blocks a callback's `alp_rpc_send`.
- **Partial-init failure frees the virtio device.** The raw device is kept from creation and `rpmsg_deinit_vdev` runs only on a vdev the rpmsg layer adopted.
- **`alp_rpc_unsubscribe` waits for an in-flight callback of that method** (skipped when called from the callback itself), so the caller may free the user pointer on return.
