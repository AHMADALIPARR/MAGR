/-
SPDX-License-Identifier: AGPL-3.0-only
Copyright (C) 2026 MAGR contributors

This file is part of MAGR (Merkle Array Graph Runtime).

MAGR is free software: you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, version 3.

MAGR is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public
License along with MAGR. If not, see <https://www.gnu.org/licenses/>.

Note for AGPL section 13: MAGR is designed to be used over a
network. If you run a modified version as a service, section 13
requires you to offer those users the corresponding source of
your modified version. See docs/licensing.md.
-/

/-!
# MAGR formal model

Theorems here are about `MagrFormal`'s **mathematical model**, not about any
compiled artefact. See `MagrFormal.Disclaimer` before drawing any conclusion
from a green `lake build`.
-/
import MagrFormal.Disclaimer
import MagrFormal.ArrayModel
import MagrFormal.DotProduct
import MagrFormal.Axpy
import MagrFormal.Norm
import MagrFormal.MerkleLink