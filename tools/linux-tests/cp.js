const cp = require('child_process');
const r = cp.spawnSync('/programs/forktest', ['child']);
console.log('[CP] spawnSync', String(r.stdout).trim(), 'status', r.status, r.error ? r.error.message : '');
try { cp.execFileSync('/programs/forktest', ['child']); } catch (e) { console.log('[CP] execFileSync status', e.status, String(e.stdout).trim().slice(0, 16)); }
const c = cp.spawn('/programs/forktest', ['child']);
let out = '';
c.stdout.on('data', d => out += d);
c.on('close', code => {
  console.log('[CP] async spawn', out.trim().slice(0, 16), 'code', code);
  const k = cp.spawn('/programs/forktest', ['sleep']);
  k.on('exit', (code2, sig) => { console.log('[CP] kill ->', code2, sig); console.log('[CP] done'); });
  setTimeout(() => k.kill('SIGTERM'), 300);
});
