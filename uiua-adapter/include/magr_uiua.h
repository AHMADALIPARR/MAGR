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
 * magr_uiua.h -- Uiua integration.
 *
 * Uiua is tacit and stack-based: a program is a sequence of glyphs applied
 * right-to-left to a stack. Its compositional style maps well onto a DAG, and
 * its array semantics are close to what BLAS wants -- with the important
 * exception that Uiua arrays are row-major-ish by intuition while the numerical
 * backbone is column-major. That difference is the whole risk in this adapter.
 *
 * The policy here mirrors the Klong adapter: lower a documented subset, refuse
 * everything else explicitly, and never insert a layout conversion that the
 * caller did not ask for. A silent transpose inside an adapter is a bug that
 * only shows up as wrong numbers three layers away.
 *
 * SUPPORTED SUBSET
 *
 *   Uiua glyphs        |  Array IR            |  Fortran
 *   -------------------+----------------------+--------------
 *   /+×                |  fortran.dot         |  DDOT
 *   +×                 |  fortran.axpy        |  DAXPY
 *   √/+×.  (norm)      |  fortran.nrm2        |  DNRM2
 *   ×∸+                |  fortran.gemm        |  DGEMM
 *   /+×+               |  fortran.gemv        |  DGEMV
 *   /+                 |  reduce.sum          |  MAGR kernel
 *   ¬                  |  elementwise.neg     |  MAGR kernel
 *   ⍜(...|...)        |  (not lowered)       |  modifier form
 *
 * Uiua's norm idiom deserves a note. `√/+×.` in Uiua computes sqrt of the sum
 * of squares of the *squared* values in one common spelling, and
 * `√/⊕×.⟩+⟜×.` in another, depending on the version and the glyph set. MAGR
 * lowers the *intent* -- a 2-norm -- to DNRM2, and it does so through the typed
 * entry point, never by pattern-matching the glyph string, because a glyph
 * parser that has to track every Uiua release is a bug factory. String
 * classification exists for diagnostics only.
 *
 * DELIBERATELY NOT LOWERED
 *
 *   ⊞  table/reshape       needs a reshape that can change rank on a view
 *   ⊏  slice               as with Klong: declared, not implemented
 *   ⍜  modifier             modifiers take/return functions; the IR has no
 *                           higher-order node
 *   ÷  elementwise divide   implemented in the IR but its IEEE div-by-zero
 *                           behaviour is a language-semantics decision that
 *                           should be made deliberately, not by default
 *
 * NOTE ON EVIDENCE
 *
 * Nothing here has been compiled or executed. See STATUS.md.
 */

#ifndef MAGR_UIUA_H
#define MAGR_UIUA_H

#include "magr/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

magr_status magr_uiua_lower_dot(const magr_ir_value *x, const magr_ir_value *y, magr_ir *ir);
magr_status magr_uiua_lower_axpy(const magr_ir_value *x, const magr_ir_value *y, double alpha,
                                 magr_ir *ir);
magr_status magr_uiua_lower_norm(const magr_ir_value *x, magr_ir *ir);
magr_status magr_uiua_lower_matmul(const magr_ir_value *a, const magr_ir_value *b, magr_ir *ir);
magr_status magr_uiua_lower_gemv(const magr_ir_value *a, const magr_ir_value *x,
                                 const magr_ir_value *y, magr_trans trans, magr_ir *ir);

/* Recognises a supported glyph sequence for diagnostics. Never builds a
 * program: lowering goes through the typed entry points above. */
magr_status magr_uiua_classify(const char *glyphs, magr_op_kind *kind_out);

const char *magr_uiua_unsupported_reason(const char *glyphs);

/*
 * Reports whether a Uiua value can be handed to the numerical backbone
 * without a copy. Uiua values carry no layout tag of their own, so this is an
 * explicit request from the caller rather than something MAGR infers: it is
 * asking whether the strides it has are column-major.
 */
magr_status magr_uiua_check_column_major(const magr_ir_value *v);

#ifdef __cplusplus
}
#endif

#endif /* MAGR_UIUA_H */