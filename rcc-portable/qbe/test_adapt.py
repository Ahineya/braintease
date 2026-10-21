#!/usr/bin/env python3
"""Contracts at the C16 adaptation boundary; vendor sources stay unchanged."""
import re
import unittest
from adapt_qbe import adapt, format_calls, widen_integers, UPSTREAM, SOURCES


class Adaptation(unittest.TestCase):
    def test_numeric_tokens_and_noncode(self):
        text='int a=65535; unsigned b=0xff; double f=0.5+1e-3+0x1.fp+4; /* int 1 */ char c=\'1\';'
        self.assertEqual(widen_integers(text),'long a=65535L; unsigned long b=0xffL; double f=0.5+1e-3+0x1.fp+4; /* int 1 */ char c=\'1\';')
        self.assertEqual(widen_integers('unsigned int a; uint b=1ULL;'),'unsigned long a; uint b=1ULL;')
        self.assertEqual(widen_integers('1u 42U 42ul 0xffu'), '1UL 42UL 42ul 0xffUL')
    def test_varargs_and_escaped_registers(self):
        text=widen_integers('fprintf(f,"%%xmm%d %c %*u", n, c, w, v);')
        self.assertIn('%%xmm%ld %c %*lu',text)
        self.assertEqual(format_calls(text),'fprintf(f,"%%xmm%ld %c %*lu",(long)( n),(short)( c),(short)( w),(unsigned long)( v));')
    def test_snapshot_contract(self):
        for name in SOURCES+['all.h','ops.h','amd64/all.h']:
            result=adapt(name,(UPSTREAM/name).read_text())
            self.assertNotIn('__attribute__((noreturn))',result)
            if name=='main.c':
                self.assertIn('qbe_output_ready();',result)
                self.assertEqual(re.findall(r'&T_\w+,',result),['&T_amd64_sysv,'])
            if name=='spill.c':
                self.assertIn('qbe_spill_instruction',result)
                self.assertIn('qbe_spill_block',result)
            if name=='mem.c':
                self.assertIn('qbe_coalesce_liveness',result)
                self.assertIn('qbe_coalesce_substitute',result)
            if name=='amd64/targ.c':
                self.assertNotIn('Target T_amd64_win',result)
                self.assertNotIn('Target T_amd64_apple',result)
            if name=='amd64/sysv.c':
                self.assertIn('qbe_call_result',result)
                self.assertIn('qbe_call_registers',result)
                self.assertIn('qbe_call_stack',result)
            if name=='all.h':
                self.assertIn('typedef unsigned long qbe_uint;',result)
                self.assertNotIn('typedef unsigned long uint;',result)
            if name=='rega.c':
                self.assertIn('pm_capacity = 2L * fn->ntmp + Tmp0;',result)
                self.assertIn('npm == pm_capacity',result)
                self.assertIn('qbe_registers_assign',result)
                self.assertIn('qbe_registers_edges',result)
            if name=='amd64/isel.c':
                self.assertIn('qbe_sel_division',result)
                self.assertIn('qbe_sel_ftoui',result)
                self.assertIn('qbe_sel_other',result)


if __name__=='__main__':unittest.main()
