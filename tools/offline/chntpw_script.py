#!/usr/bin/env python3
"""chntpw command script that registers guest/drivers/drivers.json in an offline SYSTEM hive.
Host-side bring-up helper; the phone's machine prep has its own writer.
usage: chntpw_script.py drivers.json [instance-substring ...] > cmds; chntpw -e SYSTEM < cmds"""
import json, sys

def main():
    manifest = json.load(open(sys.argv[1]))
    keep = sys.argv[2:]
    out = []
    def go(path):
        parts = path.split('\\')
        out.append('cd \\')
        for p in parts:
            out.append('nk ' + p)
            out.append('cd ' + p)
    def sz(n, v, t=1): out.extend([f'nv {t} {n}', f'ed {n}', v])
    def dw(n, v): out.extend([f'nv 4 {n}', f'ed {n}', str(v)])
    def ms(n, vals): out.extend([f'nv 7 {n}', f'ed {n}', *vals, '--q'])
    def values(base, tree):
        for sub, vals in tree.items():
            go(base + '\\' + sub)
            for k, v in vals.items():
                if isinstance(v, str): sz(k, v)
                else: dw(k, v)
    for i, d in enumerate(manifest['drivers']):
        svc = 'ControlSet001\\Services\\' + d['service']
        go(svc)
        dw('Type', d.get('type', 1)); dw('Start', d['start']); dw('ErrorControl', 1)
        sz('ImagePath', 'System32\\drivers\\' + d['file'].split('/')[-1], 2)
        sz('DisplayName', d.get('description', d['service']))
        if d.get('group'): sz('Group', d['group'])
        values(svc, d.get('values', {}).get('service', {}))
        for j, dev in enumerate(d['devices']):
            if keep and not any(k in dev['instance'] for k in keep) and not dev['instance'].startswith(('ACPI', 'ROOT')):
                continue
            klass = dev.get('class', d['class'])
            slot = '%04d' % (90 + 4 * i + min(j, 3))
            cls = 'ControlSet001\\Control\\Class\\' + klass + '\\' + slot
            go(cls)
            sz('DriverDesc', dev.get('description', d.get('description', d['service']))); sz('ProviderName', 'HyperWin')
            sz('MatchingDeviceId', dev['hardwareIds'][0].lower()); sz('DriverVersion', '1.0.0.0')
            inst = 'ControlSet001\\Enum\\' + dev['instance']
            go(inst)
            sz('Service', d['service']); sz('ClassGUID', klass); sz('Class', dev.get('className', d.get('className', '')))
            ms('HardwareID', dev['hardwareIds'])
            if dev.get('compatibleIds'): ms('CompatibleIDs', dev['compatibleIds'])
            sz('DeviceDesc', dev.get('description', d.get('description', d['service']))); sz('Mfg', 'HyperWin')
            dw('ConfigFlags', 0); sz('Driver', klass + '\\' + slot)
            values(inst, d.get('values', {}).get('device', {}))
    out += ['q', 'y', '']
    print('\n'.join(out))

main()
