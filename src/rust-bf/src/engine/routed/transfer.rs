//! One-body transfers whose writes cannot affect any later scan in that body.
//! Scan strides establish residue classes without assuming a CPU tape layout.
use super::{Kind, Machine, Part, Routed, SCAN_LIMIT};

pub(super) struct Transfer {
    write: usize,
    control_delta: u16,
    destination_delta: u16,
}

impl Transfer {
    pub(super) fn compile(parts: &[Part]) -> Option<Self> {
        let Kind::Add(control_delta) = parts.first()?.kind else {
            return None;
        };
        if control_delta & 1 == 0 {
            return None;
        }
        let mut writes = parts
            .iter()
            .enumerate()
            .filter_map(|(i, p)| matches!(p.kind, Kind::Add(_)).then_some(i));
        writes.next()?;
        let write = writes.next()?;
        if writes.next().is_some() || !matches!(parts[write - 1].kind, Kind::Scan { .. }) {
            return None;
        }
        let Kind::Add(destination_delta) = parts[write].kind else {
            return None;
        };
        if destination_delta == 0 {
            return None;
        }
        let mut lattice = 0usize;
        for part in parts {
            match part.kind {
                Kind::Search(_) => return None,
                Kind::Scan { stride, .. } => {
                    let mut step = stride.unsigned_abs();
                    while step != 0 {
                        (lattice, step) = (step, lattice % step);
                    }
                }
                _ => {}
            }
        }
        if lattice <= 1 {
            return None;
        }
        let mut residue = 0i128;
        let mut destination = None;
        for (i, part) in parts.iter().enumerate() {
            match part.kind {
                Kind::Move(motion) => {
                    residue = (residue + motion.advance as i128).rem_euclid(lattice as i128)
                }
                Kind::Add(_) if i == write => {
                    // Counter and destination cannot alias.
                    if residue == 0 {
                        return None;
                    }
                    destination = Some(residue);
                }
                Kind::Scan { .. } if residue == 0 || destination == Some(residue) => {
                    return None;
                }
                _ => {}
            }
        }
        Some(Self {
            write,
            control_delta,
            destination_delta,
        })
    }

    pub(super) fn execute(&self, routed: &Routed, machine: &mut Machine) -> bool {
        let start = machine.pointer;
        let value = machine.tape[start];
        if value == 0 {
            machine.pc = routed.next;
            return true;
        }
        let mut pointer = start;
        let mut destination = start;
        let mut probes = SCAN_LIMIT;
        for (i, part) in routed.parts.iter().enumerate() {
            match part.kind {
                Kind::Add(_) if i == self.write => destination = pointer,
                Kind::Add(_) => {}
                Kind::Move(motion) => {
                    let Some(next) = motion.resolve(pointer, machine.tape.len()) else {
                        return false;
                    };
                    pointer = next;
                }
                Kind::Scan {
                    stride,
                    minimum,
                    maximum,
                } => {
                    let Some(next) =
                        machine.trace_scan(pointer, stride, minimum, maximum, &mut probes)
                    else {
                        return false;
                    };
                    pointer = next;
                }
                Kind::Search(_) => unreachable!("compiled transfer has no searches"),
            }
        }
        if pointer != start {
            return false;
        }
        // The destination is a previously observed zero exit. Its write occurs
        // after that scan and on a different residue from every later scan.
        // The counter is disjoint from all scans. Every original excursion was
        // checked before committing either write, including cancelled moves.
        machine.write_tape(start, value.wrapping_add(self.control_delta));
        machine.write_tape(destination, self.destination_delta);
        machine.pc = if value.wrapping_add(self.control_delta) == 0 {
            routed.next
        } else {
            routed.start
        };
        true
    }
}

#[cfg(test)]
mod tests;
