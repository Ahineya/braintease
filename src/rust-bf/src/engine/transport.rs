//! Guarded moving state: local affine transfers carry a small vector through
//! zero cells, either along an invariant predicate or a migrating countdown.
//! Everything is derived from BF offsets; no guest layout or opcode is known.
use super::{Machine, Op, Program, BLOCK_LIMIT, SCAN_LIMIT};

#[derive(Debug)]
pub(super) struct Transport {
    sources: Vec<isize>,
    // Each contiguous source group on one residue, ordered in walk direction.
    groups: Vec<(isize, isize)>,
    mark: Option<(isize, u16)>,
    counted: bool,
    stride: isize,
    minimum: isize,
    maximum: isize,
    next: usize,
}
impl Transport {
    pub(super) fn parse(p: &Program, at: usize) -> Option<Self> {
        let Op::Open(next) = p.code[at].op else {
            return None;
        };
        if next - at > BLOCK_LIMIT {
            return None;
        }
        let mut cursor = at + 1;
        let mut pos = 0isize;
        let mut minimum = 0;
        let mut maximum = 0;
        let mut sources = Vec::new();
        let mut adds = Vec::new();
        while cursor < next - 1 {
            match p.code[cursor].op {
                Op::Move(d) => {
                    pos = pos.checked_add(d)?;
                    minimum = minimum.min(pos);
                    maximum = maximum.max(pos);
                    cursor += 1;
                }
                Op::Add(d) => {
                    adds.push((pos, d, sources.len()));
                    cursor += 1;
                }
                Op::Open(after) => {
                    let a = p.arithmetic(cursor + 1, after - 1)?;
                    if a.advance != 0 || a.changes.len() != 2 {
                        return None;
                    }
                    let &(dest, delta) = a.changes.iter().find(|(off, _)| *off != 0)?;
                    if delta != 1 || a.changes.iter().find(|(off, _)| *off == 0)?.1 != 65535 {
                        return None;
                    }
                    minimum = minimum.min(pos.checked_add(a.minimum)?);
                    maximum = maximum.max(pos.checked_add(a.maximum)?);
                    sources.push((pos, dest));
                    cursor = after;
                }
                _ => return None,
            }
        }
        let stride = pos;
        if stride == 0
            || sources.is_empty()
            || sources.len() > 8
            || sources.iter().any(|(_, d)| *d != stride)
        {
            return None;
        }
        let step = stride.unsigned_abs() as isize;
        // A destination that is also a source must have been consumed earlier.
        for (i, (source, _)) in sources.iter().enumerate() {
            if sources[i + 1..].iter().any(|(s, _)| *s == *source + stride) {
                return None;
            }
            if sources[..i].iter().any(|(s, _)| s == source) {
                return None;
            }
        }
        let counted = sources.iter().any(|(s, _)| *s == 0);
        let mut decrement = false;
        let mut mark = None;
        for (offset, delta, before) in adds {
            if counted && offset == 0 && delta == 65535 && !decrement {
                if sources[..before].iter().any(|(s, _)| *s == 0) {
                    return None;
                }
                decrement = true;
            } else {
                if mark.is_some()
                    || offset.rem_euclid(step) == 0
                    || sources
                        .iter()
                        .any(|(s, _)| (offset - s).rem_euclid(step) == 0)
                {
                    return None;
                }
                mark = Some((offset, delta));
            }
        }
        if counted != decrement {
            return None;
        }
        if !counted && sources.iter().any(|(s, _)| s.rem_euclid(step) == 0) {
            return None;
        }
        let sources: Vec<_> = sources.into_iter().map(|(s, _)| s).collect();
        let mut ordered = sources.clone();
        ordered.sort_by_key(|s| (s.rem_euclid(step), s * stride.signum()));
        let mut groups = Vec::new();
        for s in ordered {
            if let Some((_, last)) = groups.last_mut() {
                if s == *last + stride {
                    *last = s;
                    continue;
                }
                // Gaps on the same residue need a more general proof.
                if (s - *last).rem_euclid(step) == 0 {
                    return None;
                }
            }
            groups.push((s, s));
        }
        Some(Self {
            sources,
            groups,
            mark,
            counted,
            stride,
            minimum,
            maximum,
            next,
        })
    }
    pub(super) fn execute(&self, m: &mut Machine) -> bool {
        let start = m.pointer;
        let control = m.tape[start];
        if control == 0 {
            m.pc = self.next;
            return true;
        }
        if !m.range_fits(self.minimum, self.maximum) {
            return false;
        }
        let step = self.stride.unsigned_abs();
        let safe = if self.stride > 0 {
            (m.tape.len() - 1 - start - self.maximum as usize) / step + 1
        } else {
            (start - self.minimum.unsigned_abs()) / step + 1
        };
        let probe_limit = (SCAN_LIMIT / (self.groups.len() + 2)).min(64);
        let mut count = safe;
        if self.counted {
            count = count.min(control as usize);
        } else {
            let mut known = m
                .scan_cache
                .known_run(step, start)
                .map_or(0, |r| r.steps_from(start, self.stride).min(count));
            while known < count.min(probe_limit) {
                let index = (start as isize + known as isize * self.stride) as usize;
                if m.tape[index] == 0 {
                    break;
                }
                known += 1;
            }
            if known == 0 {
                return false;
            }
            let last = (start as isize + (known - 1) as isize * self.stride) as usize;
            let (slot, _) = m.scan_cache.prepare(step, start);
            m.scan_cache.remember(slot, start, last);
            count = known;
        }
        // All cells ahead of each initial state vector must be zero. Cold
        // proofs visit bounded prefixes; warm proofs reuse exact zero facts.
        for &(_, front) in &self.groups {
            let first = (start as isize + front + self.stride) as usize;
            let mut zeros = m
                .zero_cache
                .known_run(step, first)
                .map_or(0, |r| r.steps_from(first, self.stride).min(count));
            if let Some((n, 0)) = m.tape.uniform_prefix(first, self.stride, count) {
                zeros = zeros.max(n);
            }
            while zeros < count.min(probe_limit) {
                if m.tape[(first as isize + zeros as isize * self.stride) as usize] != 0 {
                    break;
                }
                zeros += 1;
            }
            count = count.min(zeros);
        }
        if count == 0 {
            return false;
        }
        let mut marker = None;
        if let Some((offset, delta)) = self.mark {
            let first = (start as isize + offset) as usize;
            let old = m.tape[first];
            let mut uniform = m
                .tape
                .uniform_prefix(first, self.stride, count)
                .filter(|(_, v)| *v == old)
                .map_or(0, |(n, _)| n);
            if old == 0 {
                uniform = uniform.max(
                    m.zero_cache
                        .known_run(step, first)
                        .map_or(0, |r| r.steps_from(first, self.stride).min(count)),
                );
            }
            while uniform < count.min(probe_limit)
                && m.tape[(first as isize + uniform as isize * self.stride) as usize] == old
            {
                uniform += 1;
            }
            count = count.min(uniform);
            if count == 0 {
                return false;
            }
            marker = Some((first, old, old.wrapping_add(delta)));
        }
        // Only the marker writes can span a long range. Try one atomic uniform
        // assignment before committing state; incompatible lattices use a
        // bounded physical prefix instead. No failed guard changes tape data.
        if let Some((first, old, value)) = marker {
            let mut last = (first as isize + (count - 1) as isize * self.stride) as usize;
            if count < 64 || !m.tape.assign_uniform(first, last, step, old, value) {
                count = count.min(probe_limit);
                last = (first as isize + (count - 1) as isize * self.stride) as usize;
                for i in 0..count {
                    m.tape[(first as isize + i as isize * self.stride) as usize] = value;
                }
                if count >= 64 {
                    m.tape.assign_uniform(first, last, step, value, value);
                }
            }
            if value == 0 && old != 0 {
                m.scan_cache.invalidate_run(first, last, step);
            }
            if value != 0 && old == 0 {
                m.zero_cache.invalidate_run(first, last, step);
            }
            let (slot, _) = if value == 0 {
                m.zero_cache.prepare(step, first)
            } else {
                m.scan_cache.prepare(step, first)
            };
            if value == 0 {
                m.zero_cache.remember(slot, first, last);
            } else {
                m.scan_cache.remember(slot, first, last);
            }
        }
        let mut values = [0u16; 8];
        for (i, &source) in self.sources.iter().enumerate() {
            values[i] = m.tape[(start as isize + source) as usize];
            if self.counted && source == 0 {
                values[i] -= count as u16;
            }
        }
        for &source in &self.sources {
            m.write_tape((start as isize + source) as usize, 0);
        }
        let distance = count as isize * self.stride;
        for (i, &source) in self.sources.iter().enumerate() {
            m.write_tape((start as isize + source + distance) as usize, values[i]);
        }
        for &(back, _) in &self.groups {
            let first = (start as isize + back) as usize;
            let last = (first as isize + (count - 1) as isize * self.stride) as usize;
            // The starting vector was cleared and every intermediate destination
            // was proved zero. The live vector begins one stride beyond this
            // range. Preserve its exact zero value across predicate-cache
            // eviction; incompatible deferred lattices safely reject the fact.
            m.tape.assign_uniform(first, last, step, 0, 0);
            let (slot, _) = m.zero_cache.prepare(step, first);
            m.zero_cache.remember(slot, first, last);
        }
        m.pointer = (start as isize + distance) as usize;
        m.pc = if m.tape[m.pointer] == 0 {
            self.next
        } else {
            self.next - 1
        };
        true
    }
}

#[cfg(test)]
mod tests;
