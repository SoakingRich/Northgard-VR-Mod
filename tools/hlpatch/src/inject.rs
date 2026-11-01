//! Safe instruction insertion for HashLink functions.
//!
//! HL jump offsets are relative (`target = index + 1 + offset`), so inserting
//! instructions shifts targets. `insert_ops` rebuilds the op list and rewrites
//! every jump, following two rules:
//!  * a FORWARD jump to an op that received a prefix lands on the prefix, so
//!    injected "before Ret" code runs on every path that returns;
//!  * a BACKWARD jump must keep targeting its `Label` (an HL verifier rule),
//!    so it lands on the original op, never on injected code.

use hlbc::opcodes::Opcode;
use hlbc::types::Function;

/// Every jump offset stored in an opcode, mutably.
fn jump_offsets(op: &mut Opcode) -> Vec<&mut i32> {
    use Opcode::*;
    match op {
        JTrue { offset, .. }
        | JFalse { offset, .. }
        | JNull { offset, .. }
        | JNotNull { offset, .. }
        | JSLt { offset, .. }
        | JSGte { offset, .. }
        | JSGt { offset, .. }
        | JSLte { offset, .. }
        | JULt { offset, .. }
        | JUGte { offset, .. }
        | JNotLt { offset, .. }
        | JNotGte { offset, .. }
        | JEq { offset, .. }
        | JNotEq { offset, .. }
        | JAlways { offset }
        | Trap { offset, .. } => vec![offset],
        Switch { offsets, end, .. } => {
            let mut v: Vec<&mut i32> = offsets.iter_mut().collect();
            v.push(end);
            v
        }
        _ => vec![],
    }
}

/// Insert `ops` before the original instruction at each given index.
/// `before == f.ops.len()` is not allowed (every function ends in Ret/Throw/jump).
pub fn insert_ops(f: &mut Function, mut inserts: Vec<(usize, Vec<Opcode>)>) {
    inserts.sort_by_key(|(i, _)| *i);
    let n = f.ops.len();
    let mut prefix: Vec<Vec<Opcode>> = vec![Vec::new(); n];
    for (i, ops) in inserts {
        assert!(i < n, "insert index out of range");
        prefix[i].extend(ops);
    }

    // first[i]: new index of the first op emitted for old op i (prefix or op)
    // orig[i]:  new index of old op i itself
    let mut first = vec![0usize; n + 1];
    let mut orig = vec![0usize; n + 1];
    let mut k = 0;
    for i in 0..n {
        first[i] = k;
        k += prefix[i].len();
        orig[i] = k;
        k += 1;
    }
    first[n] = k;
    orig[n] = k;

    let old_ops = std::mem::take(&mut f.ops);
    let old_dbg = f.debug_info.take();
    let mut new_ops = Vec::with_capacity(k);
    let mut new_dbg = old_dbg.as_ref().map(|_| Vec::with_capacity(k));

    for (i, mut op) in old_ops.into_iter().enumerate() {
        for p in prefix[i].drain(..) {
            new_ops.push(p);
            if let (Some(nd), Some(od)) = (new_dbg.as_mut(), old_dbg.as_ref()) {
                nd.push(od[i]);
            }
        }
        for off in jump_offsets(&mut op) {
            let target = (i as i64 + 1 + *off as i64) as usize;
            let dest = if *off < 0 { orig[target] } else { first[target] };
            *off = (dest as i64 - orig[i] as i64 - 1) as i32;
        }
        new_ops.push(op);
        if let (Some(nd), Some(od)) = (new_dbg.as_mut(), old_dbg.as_ref()) {
            nd.push(od[i]);
        }
    }

    f.ops = new_ops;
    f.debug_info = new_dbg;
    if let Some(a) = f.assigns.as_mut() {
        for (_, idx) in a.iter_mut() {
            // assigns refer to "op index + 1" style positions; map through orig
            let i = *idx;
            *idx = if i < orig.len() { orig[i] } else { i };
        }
    }
}

/// Indices of every `Ret` in a function.
pub fn ret_indices(f: &Function) -> Vec<usize> {
    f.ops
        .iter()
        .enumerate()
        .filter(|(_, o)| matches!(o, Opcode::Ret { .. }))
        .map(|(i, _)| i)
        .collect()
}
