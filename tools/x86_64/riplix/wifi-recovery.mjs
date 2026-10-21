// Explicit HP development policy. No shared service-manager source changes.
export function wifiRecoveryFiles(ssh=true) {
  const file=(name,data,mode=0o100644)=>({name,mode,data:Buffer.from(data)});
  return [
    file('/etc/services/wifi.ini','[service]\nprogram=/bin/wifid\nready=exec\nrestart=on-failure\nrestart_limit=5\nrestart_delay_ms=2000\nstop_timeout_ms=60000\n'),
    file('/etc/services/network.ini','[service]\ntype=oneshot\nprogram=/bin/sh\nrequires=wifi\nrestart=on-failure\nrestart_limit=5\nrestart_delay_ms=1000\n[arguments]\n1=/etc/net-start.sh\n'),
    file('/etc/wifi-online.sh',
      '# Explicit development recovery policy; edit this list to change services.\n'+
      '/bin/sh /etc/net-start.sh || exit 1\n'+
      '/bin/svc --runtime /var/run/svcd start network || exit 1\n'+
      '/bin/svc --runtime /var/run/svcd start telnetd || exit 1\n'+
      (ssh?'/bin/svc --runtime /var/run/svcd start sshd || exit 1\n':'')+
      'echo WIFI_ADDRESS_AND_SERVICES_REQUESTED\n',0o100700),
  ];
}
