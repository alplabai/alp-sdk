/* SPDX-License-Identifier: Apache-2.0 -- host stub. */
#define barrier_dmem_fence_full() __atomic_thread_fence(__ATOMIC_SEQ_CST)
