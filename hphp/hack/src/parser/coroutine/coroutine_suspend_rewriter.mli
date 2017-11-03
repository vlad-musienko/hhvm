(**
 * Copyright (c) 2017, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *)
module Syntax = Full_fidelity_editable_positioned_syntax

val fix_up_lambda_body: Syntax.t -> Syntax.t

val only_tail_call_suspends:
  Syntax.t -> bool

val rewrite_suspends:
  ?only_tail_call_suspends:bool -> Syntax.t ->
  (* (next_label * next_temp) * new_node *)
  (int * int) * Syntax.t
