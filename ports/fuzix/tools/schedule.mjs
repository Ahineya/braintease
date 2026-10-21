// Scheduling is available but never automatically activated by installing it.
import fs from 'node:fs';
export function scheduleFiles() {
  return [
    {name:'/var/spool',directory:true,mode:0o40755},
    ...['/var/spool/at','/var/spool/at/jobs','/var/spool/at/running','/var/spool/at/output']
      .map(name=>({name,directory:true,mode:0o40700})),
    {name:'/etc/crontab',mode:0o100600,preserve:true,data:Buffer.from(
      '# Riplix system scheduler: minute hour day-of-month month day-of-week command\n'+
      '# Commands run as the supervisor user; there is no username column.\n'+
      '# Example: */5 * * * * echo Scheduled hello\n')},
    {name:'/etc/services/cron.ini',mode:0o100644,preserve:true,
      data:fs.readFileSync(new URL('../../services/examples/cron.ini',import.meta.url))}
  ];
}
