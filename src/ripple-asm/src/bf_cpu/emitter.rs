//! Small *compile-time* emitter. Every emitted operation is ordinary Brainfuck;
//! neither the BF interpreter nor its JIT knows about Ripple instructions.
#[derive(Default)]
pub(super) struct Bf {
    pub code: String,
    pub(super) at: usize,
    depth: usize,
    local_scratch: Option<(usize, usize)>,
}
pub(super) const fn cell(word: usize) -> usize {
    word * 8 + 1
}
impl Bf {
    // Arithmetic is relative to the current instruction, with scratch confined
    // to its reserved tail. System routines keep their fixed origin scratch.
    pub fn local(copy: usize, conditions: usize) -> Self {
        Self {
            local_scratch: Some((copy, conditions)),
            ..Self::default()
        }
    }
    fn condition(&self) -> (usize, usize) {
        if let Some((_, base)) = self.local_scratch {
            assert!(self.depth < 8, "local BF condition scratch exhausted");
            let t = base + self.depth * 8;
            (t, t + 1)
        } else {
            assert!(self.depth < 16, "BF CPU conditional scratch exhausted");
            let t = cell(128 + self.depth * 2);
            (t, t + 8)
        }
    }
    pub fn at(&mut self, to: usize) {
        self.code.extend(std::iter::repeat_n(
            if to >= self.at { '>' } else { '<' },
            to.abs_diff(self.at),
        ));
        self.at = to;
    }
    pub fn raw(&mut self, s: &str) {
        self.code.push_str(s);
    }
    pub fn add(&mut self, p: usize, v: u16) {
        self.at(p);
        self.code.extend(std::iter::repeat_n(
            if v <= 32768 { '+' } else { '-' },
            usize::from(v.min(v.wrapping_neg())),
        ));
    }
    pub fn clear(&mut self, p: usize) {
        self.at(p);
        self.raw("[-]");
    }
    pub fn set(&mut self, p: usize, v: u16) {
        self.clear(p);
        self.add(p, v);
    }
    pub fn loop_(&mut self, p: usize, body: impl FnOnce(&mut Self)) {
        self.at(p);
        self.raw("[");
        body(self);
        self.at(p);
        self.raw("]");
    }
    pub fn take(&mut self, from: usize, to: usize) {
        self.loop_(from, |b| {
            b.add(from, u16::MAX);
            b.add(to, 1);
        });
    }
    pub fn copy(&mut self, from: usize, to: usize) {
        assert_ne!(from, to);
        let t = self.local_scratch.map_or(cell(170), |(copy, _)| copy);
        assert!(from != t && to != t);
        self.clear(to);
        self.clear(t);
        self.loop_(from, |b| {
            b.add(from, u16::MAX);
            b.add(to, 1);
            b.add(t, 1);
        });
        self.take(t, from);
    }
    pub fn if_(&mut self, p: usize, body: impl FnOnce(&mut Self)) {
        let (t, _) = self.condition();
        self.depth += 1;
        self.copy(p, t);
        self.loop_(t, |b| {
            b.clear(t);
            body(b);
        });
        self.depth -= 1;
    }
    pub fn zero(&mut self, p: usize, body: impl FnOnce(&mut Self)) {
        let (t, f) = self.condition();
        self.depth += 1;
        self.copy(p, t);
        self.set(f, 1);
        self.loop_(t, |b| {
            b.clear(t);
            b.clear(f);
        });
        self.loop_(f, |b| {
            b.clear(f);
            body(b);
        });
        self.depth -= 1;
    }
    pub fn eq(&mut self, p: usize, n: u16, body: impl FnOnce(&mut Self)) {
        let (t, f) = self.condition();
        self.depth += 1;
        self.copy(p, t);
        self.add(t, n.wrapping_neg());
        // t is already a disposable copy. Test it destructively instead of
        // copying a wrapped negative value (e.g. 0-1 = 65535) a second time.
        self.set(f, 1);
        self.loop_(t, |b| {
            b.clear(t);
            b.clear(f);
        });
        self.loop_(f, |b| {
            b.clear(f);
            body(b);
        });
        self.depth -= 1;
    }
    /// Unsigned comparison, with private temporaries and a caller-owned result.
    pub fn lt(&mut self, a: usize, c: usize, result: usize) {
        let x = cell(160);
        let y = cell(161);
        let stop = cell(162);
        self.copy(a, x);
        self.copy(c, y);
        self.clear(result);
        self.loop_(y, |b| {
            b.add(y, u16::MAX);
            b.set(stop, 1);
            b.zero(x, |b| {
                b.set(result, 1);
                b.clear(y);
                b.clear(stop);
            });
            b.if_(stop, |b| b.add(x, u16::MAX));
        });
        self.clear(x);
        self.clear(stop);
    }
    pub fn finish(mut self) -> String {
        self.at(0);
        self.code
    }
}
