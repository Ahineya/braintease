#!/usr/bin/env python3
"""Read-only HP I2C input inventory. Run as root in Ubuntu; emits JSON to stdout.
No unbind, I2C transaction, power change or MMIO write. MMIO reads are restricted
strictly to non-destructive identity/timing registers, never FIFO/clear/status.
"""
import base64
import glob
import json
import os
import pathlib
import struct

def text(path):
    try: return pathlib.Path(path).read_text().strip()
    except (OSError, UnicodeError): return None

def binary(path):
    try: return base64.b64encode(pathlib.Path(path).read_bytes()).decode()
    except OSError: return None

def link(path):
    try: return str(pathlib.Path(path).resolve(strict=True))
    except OSError: return None

out = {'schema': 1, 'dmi': {n: text('/sys/class/dmi/id/'+n) for n in
       ('sys_vendor', 'product_name', 'board_name')}, 'pci': {}, 'acpi': {}, 'hid': {}, 'i2c': {}}
for bdf in ('0000:00:15.2',):
    root = pathlib.Path('/sys/bus/pci/devices')/bdf
    item = {n: text(root/n) for n in ('vendor','device','subsystem_vendor','subsystem_device',
                                    'revision','resource','irq','power/runtime_status')}
    item['config'] = binary(root/'config')
    item['firmware_node'] = link(root/'firmware_node')
    item['driver'] = link(root/'driver')
    item['timing_mmio'] = {}
    # These DW and LPSS offsets are read-only identity or plain configuration;
    # do not extend this list to read-to-clear or DATA_CMD registers.
    try:
        fd = os.open(root/'resource0', os.O_RDONLY)
        try:
            for offset in (0x00,0x14,0x18,0x1c,0x20,0x7c,0xf4,0xf8,0xfc,0x200,0x204,0x2fc):
                value = os.pread(fd,4,offset)
                item['timing_mmio'][hex(offset)] = hex(struct.unpack('<I',value)[0]) if len(value)==4 else 'short read'
        finally: os.close(fd)
    except OSError as error: item['timing_mmio_error'] = str(error)
    out['pci'][bdf] = item
for pattern in ('/sys/bus/i2c/devices/*SYNA32F6*', '/sys/bus/i2c/devices/i2c-*'):
    for name in glob.glob(pattern):
        root=pathlib.Path(name)
        if 'SYNA32F6' not in name and '00:15.2' not in str(root.resolve()): continue
        out['i2c'][root.name]={n:text(root/n) for n in ('name','modalias','uevent')}
        out['i2c'][root.name]['realpath']=str(root.resolve())
        out['i2c'][root.name]['firmware_node']=link(root/'firmware_node')
for name in glob.glob('/sys/bus/acpi/devices/SYNA32F6:*'):
    root=pathlib.Path(name)
    out['acpi'][root.name]={n:text(root/n) for n in ('path','hid','modalias','status','power_state','resources')}
    out['acpi'][root.name]['physical_node']=link(root/'physical_node')
for name in glob.glob('/sys/bus/hid/devices/*:06CB:CFC6.*'):
    root=pathlib.Path(name)
    item={n:text(root/n) for n in ('uevent','country')}
    item['report_descriptor']=binary(root/'report_descriptor')
    item['realpath']=str(root.resolve())
    item['hidraw']=[str(p) for p in root.glob('hidraw/*')]
    out['hid'][root.name]=item
out['interrupts']=text('/proc/interrupts')
out['gpio']=text('/sys/kernel/debug/gpio')
out['acpi_tables']={pathlib.Path(n).name: binary(n) for pattern in
    ('/sys/firmware/acpi/tables/DSDT','/sys/firmware/acpi/tables/SSDT*') for n in glob.glob(pattern)}
print(json.dumps(out,indent=2,sort_keys=True))
