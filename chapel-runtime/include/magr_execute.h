// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 MAGR contributors
//
// This file is part of MAGR (Merkle Array Graph Runtime).
//
// MAGR is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, version 3.
//
// MAGR is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with MAGR. If not, see <https://www.gnu.org/licenses/>.
//
// Note for AGPL section 13: MAGR is designed to be used over a
// network. If you run a modified version as a service, section 13
// requires you to offer those users the corresponding source of
// your modified version. See docs/licensing.md.

/*
 * magr_execute.h -- the executor: Array IR in, validated Fortran calls out.
 *
 * This lives in chapel-runtime rather than array-core because it is the join
 * point. array-core owns metadata and knows nothing about Fortran;
 * fortran-functors owns the numerical backend and knows nothing about the IR.
 * The executor is the only component that needs both, and it is therefore the
 * right place for the Chapel scheduler's entry point too.
 *
 * Two executors are provided and they are named for what they actually do:
 *
 *   magr_ir_execute_serial     runs nodes one at a time in topological order.
 *   magr_ir_execute_wavefronts runs each ready set through a caller-supplied
 *                              task-launch callback.
 *
 * The distinction matters. An earlier revision named a serial executor
 * `..._parallel` because it accepted a callback that, when NULL, degraded to
 * serial execution. A caller reading the name would reasonably assume the two
 * paths were equivalent in cost, and they are not: one uses every core and one
 * uses one.
 */

#ifndef MAGR_EXECUTE_H
#define MAGR_EXECUTE_H

#include "magr/abi/array_abi.h"
#include "magr/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Execution report, for manifests and for tests. Not an optimisation: it is
 * the record of what ran, and a manifest that cannot say this is not
 * reproducible. */
typedef struct magr_exec_report {
    uint32_t nodes_executed;
    uint32_t waves;
    uint32_t max_wave_width;
    uint32_t serialised_for_conflict; /* nodes that could not run concurrently */
    uint32_t functors_invoked;
    magr_status first_failure;
    uint32_t first_failure_node;
} magr_exec_report;

/* Runs every node in topological order, one at a time.
 * A node that fails stops the run; nodes already executed are not undone. */
magr_status magr_ir_execute_serial(magr_ir *ir, magr_exec_report *report);

/*
 * Task-launch callback. The scheduler calls `fn` once per ready set with the
 * node ids that may run concurrently and the count. The callback is
 * responsible for launching them (Chapel: cobegin/coforall) and for joining
 * before returning. Returning anything other than MAGR_STATUS_OK aborts the
 * run.
 */
typedef magr_status_t (*magr_task_launch_fn)(magr_ir *ir, const uint32_t *nodes, uint32_t count,
                                             void *ctx, uint32_t *launched_concurrently);

magr_status magr_ir_execute_wavefronts(magr_ir *ir, magr_task_launch_fn launch, void *ctx,
                                       magr_exec_report *report);

/* Executes a single node. Its operands must already be valid. Exposed for
 * Chapel, which wants to own the loop and call in. */
magr_status magr_ir_execute_node(magr_ir *ir, uint32_t node, magr_exec_report *report);

/*
 * Executes ONE node serially. This is what a wavefront callback calls after it
 * has decided to run a node itself, so that there is exactly one implementation
 * of "what does this IR node mean".
 */
magr_status magr_ir_run_node_serial(magr_ir *ir, uint32_t node, magr_exec_report *report);

/* Builds a descriptor for a value from its magr_ir_value. Fails if the value
 * has no backing buffer, which is how an unbound IR program fails: loudly, at
 * execution, instead of by reading a null pointer inside Fortran. */
magr_status magr_ir_bind_value(const magr_ir *ir, uint32_t value_index, magr_array *out);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_EXECUTE_H */