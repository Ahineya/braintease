#!/usr/bin/env python3
"""Generate hosted filesystem dispatch hooks, leaving canonical FUZIX intact."""
import argparse
from pathlib import Path


def adapt(unit, source):
    def replace(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise ValueError(f'{unit}: filesystem hook contract changed: {old}')
        source = source.replace(old, new)

    def enter(signature, body):
        replace(signature + '\n{', signature + '\n{\n    ' + body)

    if unit == 'filesys':
        enter('inoptr srch_dir(register inoptr wd, uint8_t *compname)',
              'if(fuse_host_inode(wd))return fuse_host_search(wd,compname);')
        enter('bool emptydir(register inoptr wd)',
              'if(fuse_host_inode(wd))return fuse_host_empty(wd);')
        enter('inoptr i_open(uint16_t dev, uint16_t ino)',
              'if(dev==FUSE_HOST_DEV)return fuse_host_node(ino);')
        enter('inoptr newfile(register inoptr pino, uint8_t *name)',
              'if(fuse_host_inode(pino))return fuse_host_create(pino,name);')
        enter('bool ch_link(register inoptr wd, uint8_t *oldname, uint8_t *newname, inoptr nindex)',
              'if(fuse_host_inode(wd))return fuse_host_link(wd,oldname,newname,nindex);')
        enter('void i_deref(register inoptr ino)',
              'if(fuse_host_inode(ino)){fuse_host_deref(ino);return;}')
        enter('void wr_inode(register inoptr ino)',
              'if(fuse_host_inode(ino)){fuse_host_store(ino);return;}')
        enter('int f_trunc(register inoptr ino)',
              'if(fuse_host_inode(ino))return fuse_host_truncate(ino,0);')
        enter('int f_trunc_blocks(register inoptr ino, blkno_t nblock)',
              'if(fuse_host_inode(ino)){udata.u_error=EOPNOTSUPP;return -1;}')
        replace('    magic(inoindex);\n    return(inoindex);',
                '    magic(inoindex);\n    if(fuse_host_inode(inoindex) && fuse_host_refresh(inoindex)<0)return NULLINODE;\n    return(inoindex);')
        replace('            if (fp != lastname + 30)\n                *fp++ = c;',
                '            if (fp != lastname + 30)\n                *fp++ = c;\n'
                '            else if(fuse_host_inode(wd)){n_open_fault=1;n_fault_type=ENAMETOOLONG;}')
        replace('        *fp = 0;', '        *fp = 0;\n        if(n_open_fault){udata.u_error=n_fault_type;goto nodir;}')
    elif unit == 'inode':
        enter('void readi(register inoptr ino, uint_fast8_t flag)',
              'if(fuse_host_inode(ino)){fuse_host_read(ino);return;}')
        enter('void writei(register inoptr ino, uint_fast8_t flag)',
              'if(fuse_host_inode(ino)){fuse_host_write(ino,flag);return;}')
        replace('if (m->m_dev != NO_DEVICE &&\n\t\t\tm->m_fs.s_fmod != FMOD_CLEAN)',
                'if (m->m_dev != NO_DEVICE && m->m_dev != FUSE_HOST_DEV &&\n\t\t\tm->m_fs.s_fmod != FMOD_CLEAN)')
    elif unit == 'devio':
        enter('int d_flush(uint16_t dev)', 'if(dev==FUSE_HOST_DEV)return fuse_host_sync();')
    elif unit == 'syscall_fs3':
        replace('\t\tif (udata.u_error == EFAULT || parent == NULL)',
                '        if(fuse_host_inode(parent) && udata.u_error)goto pdrop;\n'
                '\t\tif (udata.u_error == EFAULT || parent == NULL)')
        replace('\tof_tab[oftindex].o_inode = ino;',
                '\tof_tab[oftindex].o_inode = ino;\n'
                '    if(fuse_host_inode(ino) && fuse_host_open(ino,flag)<0)goto idrop;')
        replace('\tif (n == ino->c_node.i_size)',
                '    if(fuse_host_inode(ino))return fuse_host_truncate(ino,n);\n\tif (n == ino->c_node.i_size)')
    elif unit == 'syscall_other':
        enter('arg_t _rename(void)',
              'int host_handled;arg_t host_result=fuse_host_rename(&host_handled);if(host_handled)return host_result;')
    elif unit == 'syscall_fs2':
        enter('static arg_t chmod_op(inoptr ino)',
              'if(fuse_host_inode(ino))return fuse_host_chmod(ino,udata.u_argn1);')
        enter('static int chown_op(register inoptr ino)',
              'if(fuse_host_inode(ino)){udata.u_error=EOPNOTSUPP;return -1;}')
        replace('        m = fs_tab_get(ino->c_dev);',
                '        if(fuse_host_inode(ino)){i_deref(ino);udata.u_error=EOPNOTSUPP;return -1;}\n'
                '        m = fs_tab_get(ino->c_dev);')
    else:
        raise ValueError('unknown hosted filesystem unit: ' + unit)
    return '#include "hostfs.h"\n' + source


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = adapt(args.source.stem, args.source.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result)
